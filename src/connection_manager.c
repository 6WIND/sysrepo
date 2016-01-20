/**
 * @file connection_manager.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Implementation of Connection Manager - module that handles all connections to Sysrepo Engine.
 *
 * @copyright
 * Copyright 2015 Cisco Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <ev.h>

#include "sr_common.h"
#include "session_manager.h"
#include "request_processor.h"
#include "connection_manager.h"

#define CM_IN_BUFF_MIN_SPACE 512  /**< Minimal empty space in the input buffer. */
#define CM_BUFF_ALLOC_CHUNK 1024  /**< Chunk size for buffer expansions. */

#define CM_SESSION_REQ_QUEUE_SIZE 2  /**< Initial size of the request queue buffer. */

/**
 * @brief Connection Manager context.
 */
typedef struct cm_ctx_s {
    /** Mode in which Connection Manager will operate. */
    cm_connection_mode_t mode;

    /** Session Manager context. */
    sm_ctx_t *sm_ctx;
    /** Request Processor context. */
    rp_ctx_t *rp_ctx;

    /** Path where unix-domain server is binded to. */
    const char *server_socket_path;
    /** Socket descriptor used to listen & accept new unix-domain connections. */
    int listen_socket_fd;

    /** Thread where event loop will be running in case of library mode. */
    pthread_t event_loop_thread;

    /** Event loop context. */
    struct ev_loop *event_loop;
    /** Watcher for events on server unix-domain socket. */
    ev_io server_watcher;
    /** Watcher for stop request events. */
    ev_async stop_watcher;
} cm_ctx_t;

/**
 * @brief Buffer of raw data received from / to be sent to the other side.
 */
typedef struct cm_buffer_s {
    uint8_t *data;  /**< data of the buffer */
    size_t size;    /**< Current size of the buffer. */
    size_t pos;     /**< Current position in the buffer */
} cm_buffer_t;

/**
 * @brief Context used to store session-related data managed by Connection Manager.
 */
typedef struct cm_session_ctx_s {
    uint32_t rp_req_cnt;        /**< Number of session-related outstanding requests in Request Processor. */
    sr_cbuff_t *request_queue;  /**< Queue of requests waiting for forwarding to Request Processor. */
    uint32_t rp_resp_expected;  /**< Number of expected session-related responses to be forwarded to Request Processor. */
    rp_session_t *rp_session;   /**< Request Processor's session context. */
} cm_session_ctx_t;

/**
 * @brief Context used to store connection-related data managed by Connection Manager.
 */
typedef struct cm_connection_ctx_s {
    cm_ctx_t *cm_ctx;      /**< Connection manager context assigned to this connection. */
    cm_buffer_t in_buff;   /**< Input buffer. If not empty, there is some received data to be processed. */
    cm_buffer_t out_buff;  /**< Output buffer. If not empty, there is some data to be sent when receiver is ready. */
    ev_io read_watcher;    /**< Watcher for readable events on connection's socket. */
    ev_io write_watcher;   /**< Watcher for writable events on connection's socket. */
} cm_connection_ctx_t;

/**
 * @brief Sets the file descriptor to non-blocking I/O mode.
 */
static int
cm_fd_set_nonblock(int fd)
{
    int flags = 0, rc = 0;

    flags = fcntl(fd, F_GETFL, 0);
    if (-1 == flags) {
        SR_LOG_WRN("Socket fcntl error (skipped): %s", strerror(errno));
        flags = 0;
    }
    rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (-1 == rc) {
        SR_LOG_ERR("Socket fcntl error: %s", strerror(errno));
        return SR_ERR_INTERNAL;
    }

    return SR_ERR_OK;
}

/**
 * @brief Initializes unix-domain socket server.
 */
static int
cm_server_init(cm_ctx_t *cm_ctx, const char *socket_path)
{
    int fd = -1;
    int rc = SR_ERR_OK;
    struct sockaddr_un addr;

    CHECK_NULL_ARG2(cm_ctx, socket_path);

    SR_LOG_DBG("Initializing sysrepo server at socket=%s", socket_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (-1 == fd){
        SR_LOG_ERR("Socket create error: %s", strerror(errno));
        rc = SR_ERR_INIT_FAILED;
        goto cleanup;
    }

    rc = cm_fd_set_nonblock(fd);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Cannot set socket to nonblocking mode.");
        rc = SR_ERR_INIT_FAILED;
        goto cleanup;
    }

    cm_ctx->server_socket_path = strdup(socket_path);
    if (NULL == cm_ctx->server_socket_path) {
        SR_LOG_ERR_MSG("Cannot allocate string for socket path.");
        rc = SR_ERR_NOMEM;
        goto cleanup;
    }
    unlink(socket_path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

    rc = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (-1 == rc) {
        SR_LOG_ERR("Socket bind error: %s", strerror(errno));
        rc = SR_ERR_INIT_FAILED;
        goto cleanup;
    }

    rc = listen(fd, SOMAXCONN);
    if (-1 == rc) {
        SR_LOG_ERR("Socket listen error: %s", strerror(errno));
        rc = SR_ERR_INIT_FAILED;
        goto cleanup;
    }

    cm_ctx->listen_socket_fd = fd;
    return SR_ERR_OK;

cleanup:
    if (-1 != fd) {
        close(fd);
    }
    unlink(socket_path);
    free((char*)cm_ctx->server_socket_path);
    return rc;
}

/**
 * @brief Cleans up unix-domain socket server.
 */
static void
cm_server_cleanup(cm_ctx_t *cm_ctx)
{
    if (NULL != cm_ctx) {
        if (-1 != cm_ctx->listen_socket_fd) {
            close(cm_ctx->listen_socket_fd);
        }
        if (NULL != cm_ctx->server_socket_path) {
            unlink(cm_ctx->server_socket_path);
            free((char*)cm_ctx->server_socket_path);
        }
    }
}

/**
 * @brief Cleans up Connection Manager-related session data. Automatically called from Session Manager.
 */
static void
cm_session_data_cleanup(void *session)
{
    sm_session_t *sm_session = (sm_session_t*)session;
    if ((NULL != sm_session) && (NULL != sm_session->cm_data)) {
        sr_cbuff_cleanup(sm_session->cm_data->request_queue);
        free(sm_session->cm_data);
        sm_session->cm_data = NULL;
    }
}

/**
 * @brief Cleans up Connection Manager-related connection data. Automatically called from Session Manager.
 */
static void
cm_connection_data_cleanup(void *connection)
{
    sm_connection_t *sm_connection = (sm_connection_t*)connection;
    if ((NULL != sm_connection) && (NULL != sm_connection->cm_data)) {
        free(sm_connection->cm_data->in_buff.data);
        free(sm_connection->cm_data->out_buff.data);
        free(sm_connection->cm_data);
        sm_connection->cm_data = NULL;
    }
}

/**
 * @brief Close the connection inside of Connection Manager and Request Processor.
 */
static int
cm_conn_close(cm_ctx_t *cm_ctx, sm_connection_t *conn)
{
    sm_session_list_t *sess = NULL;

    CHECK_NULL_ARG3(cm_ctx, conn, conn->cm_data);

    SR_LOG_INF("Closing the connection %p.", (void*)conn);

    ev_io_stop(cm_ctx->event_loop, &conn->cm_data->read_watcher);
    ev_io_stop(cm_ctx->event_loop, &conn->cm_data->write_watcher);

    close(conn->fd);

    /* close all sessions assigned to this connection */
    while (NULL != conn->session_list) {
        sess = conn->session_list;
        if (NULL != sess->session) {
            if (NULL != sess->session->cm_data) {
                /* stop the session in Request Processor */
                rp_session_stop(cm_ctx->rp_ctx, sess->session->cm_data->rp_session);
            }
            /* drop the session in Session manager */
            sm_session_drop(cm_ctx->sm_ctx, sess->session); /* also removes from conn->session_list */
        }
    }

    sm_connection_stop(cm_ctx->sm_ctx, conn);

    return SR_ERR_OK;
}

/**
 * @brief Expand the size of the buffer of given connection.
 */
static int
cm_conn_buffer_expand(const sm_connection_t *conn, cm_buffer_t *buff, size_t requested_space)
{
    uint8_t *tmp = NULL;

    CHECK_NULL_ARG3(conn, conn->cm_data, buff);

    if ((buff->size - buff->pos) < requested_space) {
        if (requested_space < CM_BUFF_ALLOC_CHUNK) {
            requested_space = CM_BUFF_ALLOC_CHUNK;
        }
        tmp = realloc(buff->data, buff->size + requested_space);
        if (NULL != tmp) {
            buff->data = tmp;
            buff->size += requested_space;
            SR_LOG_DBG("%s buffer for fd=%d expanded to %zu bytes.",
                    (&conn->cm_data->in_buff == buff ? "Input" : "Output"), conn->fd, buff->size);
        } else {
            SR_LOG_ERR("Cannot expand %s buffer for fd=%d - not enough memory.",
                    (&conn->cm_data->in_buff == buff ? "input" : "output"), conn->fd);
            return SR_ERR_NOMEM;
        }
    }

    return SR_ERR_OK;
}

/**
 * @brief Flush contents of the output buffer of the given connection.
 */
static int
cm_conn_out_buff_flush(cm_ctx_t *cm_ctx, sm_connection_t *connection)
{
    cm_buffer_t *buff = NULL;
    int written = 0;
    size_t buff_size = 0, buff_pos = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(cm_ctx, connection, connection->cm_data);

    buff = &connection->cm_data->out_buff;
    buff_size = buff->pos;
    buff_pos = 0;

    do {
        /* try to send all data */
        written = send(connection->fd, (buff->data + buff_pos), (buff_size - buff_pos), 0);
        if (written > 0) {
            buff_pos += written;
        } else {
            if ((EWOULDBLOCK == errno) || (EAGAIN == errno)) {
                /* no more data can be sent now */
                SR_LOG_DBG("fd %d would block", connection->fd);
                /* monitor fd for writable event */
                ev_io_start(cm_ctx->event_loop, &connection->cm_data->write_watcher);
            } else {
                /* error by writing - close the connection due to an error */
                SR_LOG_ERR("Error by writing data to fd %d: %s.", connection->fd, strerror(errno));
                connection->close_requested = true;
                break;
            }
        }
    } while ((buff_pos < buff_size) && (written > 0));

    if ((0 != buff_pos) && (buff_size - buff_pos) > 0) {
        /* move unsent data to the front of the buffer */
        memmove(buff->data, (buff->data + buff_pos), (buff_size - buff_pos));
        buff->pos = buff_size - buff_pos;
    } else {
        /* no more data left in the buffer */
        buff->pos = 0;
    }

    return rc;
}

/**
 * @brief Sends a message to the recipient identified by session context.
 */
static int
cm_msg_send_session(cm_ctx_t *cm_ctx, sm_session_t *session, Sr__Msg *msg)
{
    cm_buffer_t *buff = NULL;
    size_t msg_size = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(cm_ctx, session, session->connection, session->connection->cm_data, msg);

    buff = &session->connection->cm_data->out_buff;

    /* find out required message size */
    msg_size = sr__msg__get_packed_size(msg);
    if ((msg_size <= 0) || (msg_size > SR_MAX_MSG_SIZE)) {
        SR_LOG_ERR("Unable to send the message of size %zuB.", msg_size);
        return SR_ERR_INTERNAL;
    }

    /* expand the buffer if needed */
    rc = cm_conn_buffer_expand(session->connection, buff, SR_MSG_PREAM_SIZE + msg_size);

    if (SR_ERR_OK == rc) {
        /* write the pramble */
        sr_uint32_to_buff(msg_size, (buff->data + buff->pos));
        buff->pos += SR_MSG_PREAM_SIZE;

        /* write the message */
        sr__msg__pack(msg, (buff->data + buff->pos));
        buff->pos += msg_size;

        /* flush the buffer */
        rc = cm_conn_out_buff_flush(cm_ctx, session->connection);
        if ((session->connection->close_requested) || (SR_ERR_OK != rc)) {
            cm_conn_close(cm_ctx, session->connection);
        }
    }

    return rc;
}

/**
 * @brief Processes a session start request.
 */
static int
cm_session_start_req_process(cm_ctx_t *cm_ctx, sm_connection_t *conn, Sr__Msg *msg_in)
{
    sm_session_t *session = NULL;
    struct passwd *pws = NULL;
    Sr__Msg *msg = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(cm_ctx, conn, msg_in, msg_in->request, msg_in->request->session_start_req);

    SR_LOG_DBG("Processing session_start request (conn=%p).", (void*)conn);

    /* retrieve real user name */
    pws = getpwuid(conn->uid);

    /* create the session in SM */
    rc = sm_session_create(cm_ctx->sm_ctx, conn, pws->pw_name,
            msg_in->request->session_start_req->user_name, &session);
    if ((SR_ERR_OK != rc) || (NULL == session)) {
        SR_LOG_ERR("Unable to create the session in Session Manager (conn=%p).", (void*)conn);
        return rc;
    }

    /* prepare the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__SESSION_START, session->id, &msg);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Cannot allocate the response for session_start request (conn=%p).", (void*)conn);
        if (NULL != session) {
            sm_session_drop(cm_ctx->sm_ctx, session);
        }
        return SR_ERR_NOMEM;
    }

    /* prepare CM session data */
    session->cm_data = calloc(1, sizeof(*(session->cm_data)));
    if (NULL == session->cm_data) {
        SR_LOG_ERR_MSG("Cannot allocate CM session data.");
        rc = SR_ERR_NOMEM;
    }

    /* initialize session request queue */
    if (SR_ERR_OK == rc) {
        rc = sr_cbuff_init(CM_SESSION_REQ_QUEUE_SIZE, sizeof(Sr__Msg*), &session->cm_data->request_queue);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Cannot initialize session request queue (session id=%"PRIu32").", session->id);
            rc = SR_ERR_NOMEM;
        }
    }

    /* start session in Request Processor */
    if (SR_ERR_OK == rc) {
        rc = rp_session_start(cm_ctx->rp_ctx, session->real_user, session->effective_user, session->id,
                sr_datastore_gpb_to_sr(msg_in->request->session_start_req->datastore), &session->cm_data->rp_session);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Cannot start Request Processor session (conn=%p).", (void*)conn);
        }
    }

    if (SR_ERR_OK == rc) {
        /* set the id to response */
        msg->response->session_start_resp->session_id = session->id;
    } else {
        /* set the error code to response */
        sm_session_drop(cm_ctx->sm_ctx, session);
        msg->response->result = rc;
    }

    /* send the response */
    rc = cm_msg_send_session(cm_ctx, session, msg);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Unable to send session_start response (conn=%p).", (void*)conn);
    }

    /* release the message */
    sr__msg__free_unpacked(msg, NULL);

    return rc;
}

/**
 * @brief Processes a session stop request.
 */
static int
cm_session_stop_req_process(cm_ctx_t *cm_ctx, sm_session_t *session, Sr__Msg *msg_in)
{
    Sr__Msg *msg_out = NULL;
    int rc = SR_ERR_OK;
    bool drop_session = false;

    CHECK_NULL_ARG5(cm_ctx, session, msg_in, msg_in->request, msg_in->request->session_stop_req);

    SR_LOG_DBG("Processing session_stop request (session id=%"PRIu32").", session->id);

    /* prepare the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__SESSION_STOP, msg_in->session_id, &msg_out);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Cannot allocate the response for session_stop request (session id=%"PRIu32").", session->id);
        return SR_ERR_NOMEM;
    }

    if (SR_ERR_OK == rc) {
        /* validate provided session id */
        if (session->id != msg_in->request->session_stop_req->session_id) {
            SR_LOG_ERR("Stopping of other sessions is not allowed (sess id=%"PRIu32", requested id=%"PRIu32").",
                    session->id, msg_in->request->session_stop_req->session_id);
            msg_out->response->error_msg = strdup("Stopping of other sessions is not allowed");
            rc = SR_ERR_UNSUPPORTED;
        }
    }

    /* stop session in Request Processor */
    if ((SR_ERR_OK == rc) && (NULL != session->cm_data)) {
        rc = rp_session_stop(cm_ctx->rp_ctx, session->cm_data->rp_session);
    }

    if (SR_ERR_OK == rc) {
        /* set the id to response */
        msg_out->response->session_stop_resp->session_id = session->id;
        drop_session = true;
    } else {
        /* set the error code to response */
        msg_out->response->result = rc;
    }

    /* send the response */
    rc = cm_msg_send_session(cm_ctx, session, msg_out);
    if (SR_ERR_OK != rc) {
        SR_LOG_WRN("Unable to send session_stop response via session id=%"PRIu32".", session->id);
    }

    /* release the message */
    sr__msg__free_unpacked(msg_out, NULL);

    /* drop session in SM - must be called AFTER sending */
    if (drop_session && (SR_ERR_OK == rc)) {
        rc = sm_session_drop(cm_ctx->sm_ctx, session);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Unable to drop the session in Session Manager (session id=%"PRIu32").", session->id);
        }
    }

    return rc;
}

/**
 * Process a request from client.
 */
static int
cm_req_process(cm_ctx_t *cm_ctx, sm_connection_t *conn, sm_session_t *session, Sr__Msg *msg)
{
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(cm_ctx, conn, msg, msg->request);
    /* session can be NULL by session_start */
    if ((SR__OPERATION__SESSION_START != msg->request->operation) &&
            ((NULL == session || NULL == session->cm_data))) {
        return SR_ERR_INVAL_ARG;
    }

    rc = sr_pb_msg_validate(msg, SR__MSG__MSG_TYPE__REQUEST, msg->request->operation);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Invalid request received (conn=%p).", (void*)conn);
        rc = SR_ERR_INVAL_ARG;
        goto cleanup;
    }

    if (CM_AF_UNIX_CLIENT != conn->type) {
        SR_LOG_ERR("Request received from non-client connection (conn=%p).", (void*)conn);
        rc = SR_ERR_INVAL_ARG;
        goto cleanup;
    }

    switch (msg->request->operation) {
        case SR__OPERATION__SESSION_START:
            rc = cm_session_start_req_process(cm_ctx, conn, msg);
            sr__msg__free_unpacked(msg, NULL);
            break;
        case SR__OPERATION__SESSION_STOP:
            rc = cm_session_stop_req_process(cm_ctx, session, msg);
            sr__msg__free_unpacked(msg, NULL);
            break;
        default:
            if (session->cm_data->rp_req_cnt > 0) {
                /* there are some outstanding requests in RP, put the message into queue */
                rc = sr_cbuff_enqueue(session->cm_data->request_queue, &msg);
                if (SR_ERR_OK != rc) {
                    goto cleanup;
                }
            } else {
                /* no outstanding requests in RP, we can forward the message to request Processor */
                session->cm_data->rp_req_cnt += 1;
                rc = rp_msg_process(cm_ctx->rp_ctx, session->cm_data->rp_session, msg);
                if (SR_ERR_OK != rc) {
                    session->cm_data->rp_req_cnt -= 1;
                    /* do not cleanup the message (already done in RP) */
                }
            }
            break;
    }

    return rc;

cleanup:
    sr__msg__free_unpacked(msg, NULL);
    return rc;
}

/**
 * Process a response from client.
 */
static int
cm_resp_process(cm_ctx_t *cm_ctx, sm_connection_t *conn, sm_session_t *session, Sr__Msg *msg)
{
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(cm_ctx, conn, msg, msg->response);
    CHECK_NULL_ARG2(session, session->cm_data);

    rc = sr_pb_msg_validate(msg, SR__MSG__MSG_TYPE__RESPONSE, msg->response->operation);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Invalid response received (conn=%p).", (void*)conn);
        rc = SR_ERR_INVAL_ARG;
        goto cleanup;
    }

    if (CM_AF_UNIX_SERVER != conn->type) {
        SR_LOG_ERR("Response received from non-server connection (conn=%p).", (void*)conn);
        rc = SR_ERR_INVAL_ARG;
        goto cleanup;
    }

    if (session->cm_data->rp_resp_expected > 0) {
        /* the response is expected, forward it to Request Processor */
        rc = rp_msg_process(cm_ctx->rp_ctx, session->cm_data->rp_session, msg);
    } else {
        /* the response is unexpected */
        SR_LOG_ERR("Unexpected response received to session id=%"PRIu32".", session->id);
        rc = SR_ERR_INVAL_ARG;
        goto cleanup;
    }

    return rc;

cleanup:
    sr__msg__free_unpacked(msg, NULL);
    return rc;
}

/**
 * Process a message received on connection.
 */
static int
cm_conn_msg_process(cm_ctx_t *cm_ctx, sm_connection_t *conn, uint8_t *msg_data, size_t msg_size)
{
    Sr__Msg *msg = NULL;
    sm_session_t *session = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(cm_ctx, conn, msg_data);

    /* unpack the message */
    msg = sr__msg__unpack(NULL, msg_size, msg_data);
    if (NULL == msg) {
        SR_LOG_ERR("Unable to unpack the message (conn=%p).", (void*)conn);
        return SR_ERR_INTERNAL;
    }

    /* NULL check according to message type */
    if (((SR__MSG__MSG_TYPE__REQUEST == msg->type) && (NULL == msg->request)) ||
            ((SR__MSG__MSG_TYPE__RESPONSE == msg->type) && (NULL == msg->response))) {
        SR_LOG_ERR("Message with NULL payload received (conn=%p).", (void*)conn);
        rc = SR_ERR_INVAL_ARG;
        goto cleanup;
    }

    /* find matching session (except for session_start request) */
    if ((SR__MSG__MSG_TYPE__REQUEST != msg->type) || (SR__OPERATION__SESSION_START != msg->request->operation)) {
        rc = sm_session_find_id(cm_ctx->sm_ctx, msg->session_id, &session);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Unable to find session context for session id=%"PRIu32" (conn=%p).",
                    msg->session_id, (void*)conn);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }
        if (conn != session->connection) {
            SR_LOG_ERR("Session mismatched with connection (session id=%"PRIu32", conn=%p).",
                    msg->session_id, (void*)conn);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }
    }

    switch (msg->type) {
        case SR__MSG__MSG_TYPE__REQUEST:
            rc = cm_req_process(cm_ctx, conn, session, msg);
            break;
        case SR__MSG__MSG_TYPE__RESPONSE:
            rc = cm_resp_process(cm_ctx, conn, session, msg);
            break;
        default:
            SR_LOG_ERR("Unexpected message type received (session id=%"PRIu32").", session->id);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
    }

    return rc;

cleanup:
    sr__msg__free_unpacked(msg, NULL);
    return rc;
}

/**
 * Process the content of input buffer of a connection.
 */
static int
cm_conn_in_buff_process(cm_ctx_t *cm_ctx, sm_connection_t *conn)
{
    cm_buffer_t *buff = NULL;
    size_t buff_pos = 0, buff_size = 0;
    size_t msg_size = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(cm_ctx, conn, conn->cm_data);

    buff = &conn->cm_data->in_buff;
    buff_size = buff->pos;
    buff_pos = 0;

    if (buff_size <= SR_MSG_PREAM_SIZE) {
        return SR_ERR_OK; /* nothing to process so far */
    }

    while ((buff_size - buff_pos) > SR_MSG_PREAM_SIZE) {
        msg_size = sr_buff_to_uint32(buff->data + buff_pos);
        if ((msg_size <= 0) || (msg_size > SR_MAX_MSG_SIZE)) {
            /* invalid message size */
            SR_LOG_ERR("Invalid message size in the message preamble (%zu).", msg_size);
            return SR_ERR_MALFORMED_MSG;
        } else if ((buff_size - buff_pos) >= msg_size) {
            /* the message is completely retrieved, parse it */
            SR_LOG_DBG("New message of size %zu bytes received.", msg_size);
            rc = cm_conn_msg_process(cm_ctx, conn,
                    (buff->data + buff_pos + SR_MSG_PREAM_SIZE), msg_size);
            buff_pos += SR_MSG_PREAM_SIZE + msg_size;
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR_MSG("Error by processing of the message.");
                break;
            }
        } else {
            /* the message is not completely retrieved, end processing */
            SR_LOG_DBG("Partial message of size %zu, received %zu.", msg_size,
                    (buff_size - SR_MSG_PREAM_SIZE - buff_pos));
            break;
        }
    }

    if ((0 != buff_pos) && (buff_size - buff_pos) > 0) {
        /* move unprocessed data to the front of the buffer */
        memmove(buff->data, (buff->data + buff_pos), (buff_size - buff_pos));
        buff->pos = buff_size - buff_pos;
    } else {
        /* no more unprocessed data left in the buffer */
        buff->pos = 0;
    }

    return rc;
}

/**
 * @brief Callback called by the event loop watcher when the file descriptor of
 * a connection is readable (some data has arrived).
 */
static void
cm_conn_read_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    sm_connection_t *conn = NULL;
    cm_ctx_t *cm_ctx = NULL;
    cm_buffer_t *buff = NULL;
    int bytes = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG_VOID2(w, w->data);
    conn = (sm_connection_t*)w->data;

    CHECK_NULL_ARG_VOID3(conn, conn->cm_data, conn->cm_data->cm_ctx);
    cm_ctx = conn->cm_data->cm_ctx;
    buff = &conn->cm_data->in_buff;

    SR_LOG_DBG("fd %d readable", conn->fd);

    do {
        /* expand input buffer if needed */
        rc = cm_conn_buffer_expand(conn, buff, CM_IN_BUFF_MIN_SPACE);
        if (SR_ERR_OK != rc) {
            conn->close_requested = true;
            break;
        }
        /* receive data */
        bytes = recv(conn->fd, (buff->data + buff->pos), (buff->size - buff->pos), 0);
        if (bytes > 0) {
            /* recieved "bytes" bytes of data */
            SR_LOG_DBG("%d bytes of data recieved on fd %d : %s", bytes, conn->fd, buff->data);
            buff->pos += bytes;
        } else if (0 == bytes) {
            /* connection closed by the other side */
            SR_LOG_DBG("Peer on fd %d disconnected.", conn->fd);
            conn->close_requested = true;
            break;
        } else {
            if ((EWOULDBLOCK == errno) || (EAGAIN == errno)) {
                /* no more data to be read */
                SR_LOG_DBG("fd %d would block", conn->fd);
                break;
            } else {
                /* error by reading - close the connection due to an error */
                SR_LOG_ERR("Error by reading data on fd %d: %s.", conn->fd, strerror(errno));
                conn->close_requested = true;
                break;
            }
        }
    } while (bytes > 0); /* recv returns -1 when there is no more data to be read */

    /* process the content of input buffer */
    if (SR_ERR_OK == rc) {
        rc = cm_conn_in_buff_process(cm_ctx, conn);
        if (SR_ERR_OK != rc) {
            SR_LOG_WRN("Error by processing of the input buffer of fd=%d, closing the connection.", conn->fd);
            conn->close_requested = true;
            rc = SR_ERR_OK; /* connection will be closed, we can continue */
        }
    }

    /* close the connection if requested */
    if ((conn->close_requested) || (SR_ERR_OK != rc)) {
        cm_conn_close(cm_ctx, conn);
    }
}

/**
 * @brief Callback called by the event loop watcher when the file descriptor of
 * a connection is writable (without blocking).
 */
static void
cm_conn_write_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    sm_connection_t *conn = NULL;
    cm_ctx_t *cm_ctx = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG_VOID2(w, w->data);
    conn = (sm_connection_t*)w->data;

    CHECK_NULL_ARG_VOID3(conn, conn->cm_data, conn->cm_data->cm_ctx);
    cm_ctx = conn->cm_data->cm_ctx;

    SR_LOG_DBG("fd %d writeable", conn->fd);

    ev_io_stop(cm_ctx->event_loop, &conn->cm_data->write_watcher);

    /* flush the output buffer */
    rc = cm_conn_out_buff_flush(cm_ctx, conn);

    /* close the connection if requested */
    if ((conn->close_requested) || (SR_ERR_OK != rc)) {
        cm_conn_close(cm_ctx, conn);
    }
}

/**
 * @brief Initializes read and write watchers for the file descriptor of provided connection.
 */
static int
cm_conn_watcher_init(cm_ctx_t *cm_ctx, sm_connection_t *conn)
{
    CHECK_NULL_ARG2(cm_ctx, conn);

    conn->cm_data = calloc(1, sizeof(*(conn->cm_data)));
    if (NULL == conn->cm_data) {
        SR_LOG_ERR_MSG("Cannot allocate CM connection data context.");
        return SR_ERR_NOMEM;
    }

    conn->cm_data->cm_ctx = cm_ctx;

    ev_io_init(&conn->cm_data->read_watcher, cm_conn_read_cb, conn->fd, EV_READ);
    conn->cm_data->read_watcher.data = (void*)conn;
    ev_io_start(cm_ctx->event_loop, &conn->cm_data->read_watcher);

    ev_io_init(&conn->cm_data->write_watcher, cm_conn_write_cb, conn->fd, EV_WRITE);
    conn->cm_data->write_watcher.data = (void*)conn;
    /* do not start write watcher - will be started when needed */

    return SR_ERR_OK;
}

/**
 * @brief Callback called by the event loop watcher when a new connection is detected
 * on the server socket. Accepts new connections to the server and starts
 * monitoring the new client file descriptors.
 */
static void
cm_server_watcher_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    cm_ctx_t *cm_ctx = NULL;
    sm_connection_t *connection = NULL;
    int clnt_fd = -1;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG_VOID2(w, w->data);
    cm_ctx = (cm_ctx_t*)w->data;

    do {
        clnt_fd = accept(cm_ctx->listen_socket_fd, NULL, NULL);
        if (-1 != clnt_fd) {
            /* accepted the new connection */
            SR_LOG_DBG("New client connection on fd %d", clnt_fd);
            /* set to nonblocking mode */
            rc = cm_fd_set_nonblock(clnt_fd);
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR("Cannot set fd=%d to nonblocking mode.", clnt_fd);
                close(clnt_fd);
                continue;
            }
            /* start connection in session manager */
            rc = sm_connection_start(cm_ctx->sm_ctx, CM_AF_UNIX_CLIENT, clnt_fd, &connection);
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR("Cannot start connection in Session manager (fd=%d).", clnt_fd);
                close(clnt_fd);
                continue;
            }
            /* check uid in case of local (library) mode */
            if (CM_MODE_LOCAL == cm_ctx->mode) {
                if (connection->uid != geteuid()) {
                    SR_LOG_ERR("Peer's uid=%d does not match with local uid=%d "
                            "(required by local mode).", connection->uid, geteuid());
                    sm_connection_stop(cm_ctx->sm_ctx, connection);
                    close(clnt_fd);
                    continue;
                }
            }
            /* start watching this fd */
            rc = cm_conn_watcher_init(cm_ctx, connection);
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR("Cannot initialize watcher for fd=%d.", clnt_fd);
                close(clnt_fd);
                continue;
            }
        } else {
            if ((EWOULDBLOCK == errno) || (EAGAIN == errno)) {
                /* no more connections to accept */
                break;
            } else {
                /* error by accept - only log the error and skip it */
                SR_LOG_ERR("Unexpected error by accepting new connection: %s", strerror(errno));
                continue;
            }
        }
    } while (-1 != clnt_fd); /* accept returns -1 when there are no more connections to accept */
}

/**
 * @brief Event loop of Connection Manager. Monitors all connections for events
 * and calls proper callback handlers for each event. This function call blocks
 * until stop is requested via async stop request.
 */
void
cm_event_loop(cm_ctx_t *cm_ctx)
{
    CHECK_NULL_ARG_VOID(cm_ctx);

    SR_LOG_DBG_MSG("Starting CM event loop.");

    ev_run(cm_ctx->event_loop, 0);

    SR_LOG_DBG_MSG("CM event loop finished.");
}

/**
 * @brief Starts the event loop in a new thread (applicable only for library mode).
 */
static void *
cm_event_loop_threaded(void *cm_ctx_p)
{
    if (NULL == cm_ctx_p) {
        return NULL;
    }

    cm_ctx_t *cm_ctx = (cm_ctx_t*)cm_ctx_p;

    cm_event_loop(cm_ctx);

    return NULL;
}

/**
 * @brief Callback called by the event loop watcher when an async request to stop the loop is received.
 */
static void
cm_stop_cb(struct ev_loop *loop, ev_async *w, int revents)
{
    cm_ctx_t *cm_ctx = NULL;

    CHECK_NULL_ARG_VOID3(loop, w, w->data);
    cm_ctx = (cm_ctx_t*)w->data;

    SR_LOG_DBG_MSG("Event loop stop requested.");

    ev_break(cm_ctx->event_loop, EVBREAK_ALL);
}

int
cm_init(const cm_connection_mode_t mode, const char *socket_path, cm_ctx_t **cm_ctx_p)
{
    cm_ctx_t *ctx = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(socket_path, cm_ctx_p);

    SR_LOG_DBG_MSG("Connection Manager init started.");

    ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        SR_LOG_ERR_MSG("Cannot allocate memory for Connection Manager.");
        rc = SR_ERR_NOMEM;
        goto cleanup;
    }
    ctx->mode = mode;

    /* initialize Session Manager */
    rc = sm_init(cm_session_data_cleanup, cm_connection_data_cleanup, &ctx->sm_ctx);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Cannot initialize Session Manager.");
        goto cleanup;
    }

    /* initialize unix-domain server */
    rc = cm_server_init(ctx, socket_path);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Cannot initialize server socket.");
        goto cleanup;
    }

    /* initialize Request Processor */
    rc = rp_init(ctx, &ctx->rp_ctx);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Cannot initialize Request Processor.");
        goto cleanup;
    }

    /* initialize event loop */
    /* According to our measurements, EPOLL backend is significantly slower for
     * fewer file descriptors, so we are disabling it for now. */
    ctx->event_loop = ev_default_loop((EVBACKEND_ALL ^ EVBACKEND_EPOLL) | EVFLAG_NOENV);

    /* initialize event watcher for unix-domain server socket */
    ev_io_init(&ctx->server_watcher, cm_server_watcher_cb, ctx->listen_socket_fd, EV_READ);
    ctx->server_watcher.data = (void*)ctx;
    ev_io_start(ctx->event_loop, &ctx->server_watcher);

    /* initialize event watcher for async stop requests */
    ev_async_init(&ctx->stop_watcher, cm_stop_cb);
    ctx->stop_watcher.data = (void*)ctx;
    ev_async_start(ctx->event_loop, &ctx->stop_watcher);

    SR_LOG_DBG_MSG("Connection Manager initialized successfully.");

    *cm_ctx_p = ctx;
    return SR_ERR_OK;

cleanup:
    cm_cleanup(ctx);
    return rc;
}

void
cm_cleanup(cm_ctx_t *cm_ctx)
{
    size_t i = 0;
    sm_session_t *session = NULL;
    int rc = SR_ERR_OK;

    if (NULL != cm_ctx) {
        /* stop all sessions in RP */
        while (SR_ERR_OK == rc) {
            rc = sm_session_get_index(cm_ctx->sm_ctx, i++, &session);
            if ((NULL != session) && (NULL != session->cm_data)) {
                rp_session_stop(cm_ctx->rp_ctx, session->cm_data->rp_session);
                session = NULL;
            }
        }
        rp_cleanup(cm_ctx->rp_ctx);
        sm_cleanup(cm_ctx->sm_ctx);

        ev_loop_destroy(cm_ctx->event_loop);
        cm_server_cleanup(cm_ctx);
        free(cm_ctx);
    }
}

int
cm_start(cm_ctx_t *cm_ctx)
{
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG(cm_ctx);

    if (CM_MODE_DAEMON == cm_ctx->mode) {
        /* run the event loop in this thread */
        cm_event_loop(cm_ctx);
    } else {
        /* run the event loop in a new thread */
        rc = pthread_create(&cm_ctx->event_loop_thread, NULL,
                cm_event_loop_threaded, cm_ctx);
        if (0 != rc) {
            SR_LOG_ERR("Error by creating a new thread: %s", strerror(errno));
        }
    }

    return rc;
}

int
cm_stop(cm_ctx_t *cm_ctx)
{
    CHECK_NULL_ARG(cm_ctx);

    /* send async event to the event loop */
    ev_async_send(cm_ctx->event_loop, &cm_ctx->stop_watcher);

    if (CM_MODE_LOCAL == cm_ctx->mode) {
        /* block until cleanup is finished and the thread with event loop exits */
        pthread_join(cm_ctx->event_loop_thread, NULL);
    }

    return SR_ERR_OK;
}

int
cm_msg_send(cm_ctx_t *cm_ctx, Sr__Msg *msg)
{
    sm_session_t *session = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG_NORET2(rc, cm_ctx, msg);

    if (SR_ERR_OK != rc) {
        if (NULL != msg) {
            sr__msg__free_unpacked(msg, NULL);
        }
        return rc;
    }

    /* find the session */
    rc = sm_session_find_id(cm_ctx->sm_ctx, msg->session_id, &session);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Unable to find the session matching with id specified in the message "
                "(id=%"PRIu32").", msg->session_id);
        return rc;
    }

    if ((NULL == session) || (NULL == session->cm_data)) {
        SR_LOG_ERR("invalid session context - NULL value detected (id=%"PRIu32").", msg->session_id);
        return rc;
    }

    /* update counters of session-related requests in RP */
    if (SR__MSG__MSG_TYPE__RESPONSE == msg->type) {
        if (session->cm_data->rp_req_cnt > 0) {
            session->cm_data->rp_req_cnt -= 1;
        }
    } else if (SR__MSG__MSG_TYPE__REQUEST == msg->type) {
        session->cm_data->rp_resp_expected += 1;
    }

    /* send the message */
    rc = cm_msg_send_session(cm_ctx, session, msg);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Unable to send the message over session (id=%"PRIu32").", msg->session_id);
        return rc;
    }

    /* release the message */
    sr__msg__free_unpacked(msg, NULL);

    /* if there are no outstanding session-related requests in RP and there are some requests waiting, process them */
    if (0 == session->cm_data->rp_req_cnt) {
        if (sr_cbuff_dequeue(session->cm_data->request_queue, &msg)) {
            session->cm_data->rp_req_cnt += 1;
            rc = rp_msg_process(cm_ctx->rp_ctx, session->cm_data->rp_session, msg);
            if (SR_ERR_OK != rc) {
                session->cm_data->rp_req_cnt -= 1;
            }
        }
    }

    return SR_ERR_OK;
}
