/**
 * @file daemon.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Sysrepo daemon source file.
 *
 * @copyright
 * Copyright 2016 Cisco Systems, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>

#include "sr_common.h"
#include "connection_manager.h"

#define SR_CHILD_INIT_TIMEOUT 2  /**< Timeout to initialize the child process (in seconds) */

int pidfile_fd = -1; /**< File descriptor of sysrepo deamon's PID file */

/**
 * @brief Signal handler used to deliver initialization result from child to
 * parent process, so that parent can exit with appropriate exit code.
 */
static void
srd_child_status_handler(int signum)
{
    switch(signum) {
        case SIGUSR1:
            /* child process has initialized successfully */
            exit(EXIT_SUCCESS);
            break;
        case SIGALRM:
            /* child process has not initialized within SR_CHILD_INIT_TIMEOUT seconds */
            exit(EXIT_FAILURE);
            break;
        case SIGCHLD:
            /* child process has terminated */
            exit(EXIT_FAILURE);
            break;
    }
}

/**
 * @brief Daemonize the process - fork() and instruct the child to behave as a proper daemon.
 */
static pid_t
srd_daemonize(void)
{
    pid_t pid, sid;
    int ret = 0;
    char str[NAME_MAX] = { 0 };

    /* register handlers for signals that we expect to receive from child process */
    signal(SIGCHLD, srd_child_status_handler);
    signal(SIGUSR1, srd_child_status_handler);
    signal(SIGALRM, srd_child_status_handler);

    /* fork off the parent process. */
    pid = fork();
    if (pid < 0) {
        SR_LOG_ERR("Unable to fork sysrepo daemon: %s.", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        /* this is the parent process, wait for a signal from child */
        alarm(SR_CHILD_INIT_TIMEOUT);
        pause();
        exit(EXIT_FAILURE); /* this should not be executed */
    }

    /* at this point we are executing as the child process */

    /* ignore certain signals */
    signal(SIGUSR1, SIG_IGN);
    signal(SIGALRM, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);  /* keyboard stop */
    signal(SIGTTIN, SIG_IGN);  /* background read from tty */
    signal(SIGTTOU, SIG_IGN);  /* background write to tty */
    signal(SIGHUP, SIG_IGN);   /* hangup */
    signal(SIGPIPE, SIG_IGN);  /* broken pipe */

    /* create a new session containing a single (new) process group */
    sid = setsid();
    if (sid < 0) {
        SR_LOG_ERR("Unable to create new session: %s.", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* change the current working directory. */
   if ((chdir(SR_DEAMON_WORK_DIR)) < 0) {
       SR_LOG_ERR("Unable to change directory to '%s': %s.", SR_DEAMON_WORK_DIR, strerror(errno));
       exit(EXIT_FAILURE);
   }

   /* redirect standard files to /dev/null */
    freopen( "/dev/null", "r", stdin);
    freopen( "/dev/null", "w", stdout);
    freopen( "/dev/null", "w", stderr);

    /* set file creation mask */
    umask(S_IWGRP | S_IWOTH);

    /* maintain only single instance of sysrepo daemon */

    /* open PID file */
    pidfile_fd = open(SR_DAEMON_PID_FILE, O_RDWR | O_CREAT, 0640);
    if (pidfile_fd < 0) {
        SR_LOG_ERR("Unable to open sysrepo PID file '%s': %s.", SR_DAEMON_PID_FILE, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* acquire lock on the PID file */
    if (lockf(pidfile_fd, F_TLOCK, 0) < 0) {
        if (EACCES == errno || EAGAIN == errno) {
            SR_LOG_ERR_MSG("Another instance of sysrepo daemon is running, unable to start.");
        } else {
            SR_LOG_ERR("Unable to lock sysrepo PID file '%s': %s.", SR_DAEMON_PID_FILE, strerror(errno));
        }
        exit(EXIT_FAILURE);
    }

    /* write PID into the PID file */
    snprintf(str, NAME_MAX, "%d\n", getpid());
    ret = write(pidfile_fd, str, strlen(str));
    if (-1 == ret) {
        SR_LOG_ERR("Unable to write into sysrepo PID file '%s': %s.", SR_DAEMON_PID_FILE, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* do not close nor unlock the PID file, keep it open while the daemon is alive */

    return getppid(); /* return PID of the parent */
}

/**
 * @brief Callback to be called when a signal requesting daemon termination has been received.
 */
static void
srd_sigterm_cb(cm_ctx_t *cm_ctx, int signum)
{
    if (NULL != cm_ctx) {
        SR_LOG_INF_MSG("Sysrepo daemon termination requested.");

        /* stop the event loop in the Connection Manager */
        cm_stop(cm_ctx);

        /* close and delete the PID file */
        if (-1 != pidfile_fd) {
            close(pidfile_fd);
            pidfile_fd = -1;
        }
        unlink(SR_DAEMON_PID_FILE);
    }
}

/**
 * @brief Main routine of the sysrepo daemon.
 */
int
main(int argc, char* argv[])
{
    pid_t parent;
    int rc = SR_ERR_OK;
    cm_ctx_t *sr_cm_ctx = NULL;

    sr_logger_init("sysrepod");
    sr_log_stderr(SR_LL_NONE);
    sr_log_syslog(SR_LL_INF);

    SR_LOG_INF_MSG("Sysrepo daemon initialization started.");

    /* deamonize the process */
    parent = srd_daemonize();

    /* initialize local Connection Manager */
    rc = cm_init(CM_MODE_DAEMON, SR_DAEMON_SOCKET, &sr_cm_ctx);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Unable to initialize Connection Manager: %s.", sr_strerror(rc));
        exit(EXIT_FAILURE);
    }

    /* install SIGTERM & SIGINT signal watchers */
    rc = cm_watch_signal(sr_cm_ctx, SIGTERM, srd_sigterm_cb);
    if (SR_ERR_OK == rc) {
        rc = cm_watch_signal(sr_cm_ctx, SIGINT, srd_sigterm_cb);
    }
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Unable to initialize signal watcher: %s.", sr_strerror(rc));
        exit(EXIT_FAILURE);
    }

    /* tell the parent process that we are okay */
    kill(parent, SIGUSR1);

    SR_LOG_INF_MSG("Sysrepo daemon initialized successfully.");

    /* execute the server (the call is blocking in the event loop) */
    rc = cm_start(sr_cm_ctx);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Connection Manager execution returned an error: %s.", sr_strerror(rc));
        cm_cleanup(sr_cm_ctx);
        exit(EXIT_FAILURE);
    }

    /* cleanup */
    cm_cleanup(sr_cm_ctx);

    SR_LOG_INF_MSG("Sysrepo daemon terminated.");

    return 0;
}
