/**
 * @file notification_processor.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Sysrepo Notification Processor implementation.
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
#include <inttypes.h>
#include <pthread.h>

#include "sr_common.h"
#include "rp_internal.h"
#include "notification_processor.h"

/**
 * @brief Information about a notification destination.
 */
typedef struct np_dst_info_s {
    const char *dst_address;        /**< Destination address. */
    char **subscribed_modules;      /**< Array of module names which the destination has subscriptions for. */
    size_t subscribed_modules_cnt;  /**< Number of the modules with subscriptions. */
} np_dst_info_t;

/**
 * @brief Notification Processor context.
 */
typedef struct np_ctx_s {
    rp_ctx_t *rp_ctx;                     /**< Request Processor context. */
    np_subscription_t **subscriptions;    /**< List of active non-persistent subscriptions. */
    size_t subscription_cnt;              /**< Number of active non-persistent subscriptions. */
    sr_btree_t *dst_info_btree;           /**< Binary tree used for fast destination info lookup. */
    pthread_rwlock_t lock;                /**< Read-write lock for the context. */
} np_ctx_t;

/**
 * @brief Compares two notification destination information structures by
 * associated destination addresses (used by lookups in binary tree).
 */
static int
np_dst_info_cmp(const void *a, const void *b)
{
    assert(a);
    assert(b);
    np_dst_info_t *dst_info_a = (np_dst_info_t*)a;
    np_dst_info_t *dst_info_b = (np_dst_info_t*)b;

    int res = 0;

    assert(dst_info_a->dst_address);
    assert(dst_info_b->dst_address);

    res = strcmp(dst_info_a->dst_address, dst_info_b->dst_address);
    if (0 == res) {
        return 0;
    } else if (res < 0) {
        return -1;
    } else {
        return 1;
    }
}

/**
 * @brief Cleans up a notification destination information structure.
 * @note Called automatically when a node from the binary tree is removed
 * (which is also when the tree itself is being destroyed).
 */
static void
np_dst_info_cleanup(void *dst_info_p)
{
    np_dst_info_t *dst_info = NULL;

    if (NULL != dst_info_p) {
        dst_info = (np_dst_info_t *)dst_info_p;
        for (size_t i = 0; i < dst_info->subscribed_modules_cnt; i++) {
            free(dst_info->subscribed_modules[i]);
        }
        free(dst_info->subscribed_modules);
        free((void*)dst_info->dst_address);
        free(dst_info);
    }
}

/**
 * @brief Adds information about notification destination into NP context.
 */
static int
np_dst_info_insert(np_ctx_t *np_ctx, const char *dst_address, const char *module_name)
{
    np_dst_info_t info_lookup = { 0, }, *info = NULL, *new_info = NULL;
    char **tmp = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(np_ctx, dst_address, module_name);

    pthread_rwlock_rdlock(&np_ctx->lock);

    /* find info entry matching with the destination */
    info_lookup.dst_address = dst_address;
    info = sr_btree_search(np_ctx->dst_info_btree, &info_lookup);

    if (NULL != info) {
        /* info entry found */
        for (size_t i = 0; i < info->subscribed_modules_cnt; i++) {
            if (0 == strcmp(info->subscribed_modules[i], module_name)) {
                /* module name already exists within the info entry, no update needed */
                pthread_rwlock_unlock(&np_ctx->lock);
                return SR_ERR_OK;
            }
        }
    }

    /* info update is required */
    pthread_rwlock_unlock(&np_ctx->lock);
    pthread_rwlock_wrlock(&np_ctx->lock);

    if (NULL == info) {
        /* info entry not found, create new one */
        new_info = calloc(1, sizeof(*new_info));
        CHECK_NULL_NOMEM_GOTO(new_info, rc, cleanup);

        new_info->dst_address = strdup(dst_address);
        CHECK_NULL_NOMEM_GOTO(new_info->dst_address, rc, cleanup);

        rc = sr_btree_insert(np_ctx->dst_info_btree, new_info);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Unable to insert new info entry into btree.");
        info = new_info;
    }

    /* add the module into info entry */
    tmp = realloc(info->subscribed_modules, (info->subscribed_modules_cnt + 1) * sizeof(*tmp));
    CHECK_NULL_NOMEM_GOTO(tmp, rc, cleanup);
    info->subscribed_modules = tmp;

    info->subscribed_modules[info->subscribed_modules_cnt] = strdup(module_name);
    CHECK_NULL_NOMEM_GOTO(info->subscribed_modules[info->subscribed_modules_cnt], rc, cleanup);
    info->subscribed_modules_cnt++;

    pthread_rwlock_unlock(&np_ctx->lock);
    return SR_ERR_OK;

cleanup:
    if (NULL != new_info) {
        sr_btree_delete(np_ctx->dst_info_btree, new_info);
        free((char*)new_info->dst_address);
        free((char*)new_info->subscribed_modules);
        free(new_info);
    }
    pthread_rwlock_unlock(&np_ctx->lock);
    return rc;
}

/**
 * @brief Removes information about notification destination from NP context.
 */
static int
np_dst_info_remove(np_ctx_t *np_ctx, const char *dst_address, const char *module_name)
{
    np_dst_info_t info_lookup = { 0, }, *info = NULL;

    CHECK_NULL_ARG2(np_ctx, dst_address);

    info_lookup.dst_address = dst_address;

    if (NULL == module_name) {
        /* remove whole destination info entry */
        sr_btree_delete(np_ctx->dst_info_btree, &info_lookup);
        return SR_ERR_OK;
    }

    /* remove specified module name */
    info = sr_btree_search(np_ctx->dst_info_btree, &info_lookup);
    if (NULL != info) {
        if (1 == info->subscribed_modules_cnt) {
            /* last module - remove whole destination info entry */
            sr_btree_delete(np_ctx->dst_info_btree, &info_lookup);
        } else {
            /* not last module - remove only the matching module name */
            for (size_t i = 0; i < info->subscribed_modules_cnt; i++) {
                if (0 == strcmp(info->subscribed_modules[i], module_name)) {
                    /* remove this module from info entry */
                    free((void*)info->subscribed_modules[i]);
                    if (i < (info->subscribed_modules_cnt - 1)) {
                        memmove(info->subscribed_modules + i,
                                info->subscribed_modules + i + 1,
                                (info->subscribed_modules_cnt - i - 1) * sizeof(*info->subscribed_modules));
                    }
                    info->subscribed_modules_cnt--;
                    break;
                }
            }
        }
    }

    return SR_ERR_OK;
}

int
np_init(rp_ctx_t *rp_ctx, np_ctx_t **np_ctx_p)
{
    np_ctx_t *ctx = NULL;
    int rc = 0, ret = 0;

    CHECK_NULL_ARG2(rp_ctx, np_ctx_p);

    /* allocate the context */
    ctx = calloc(1, sizeof(*ctx));
    CHECK_NULL_NOMEM_RETURN(ctx);

    ctx->rp_ctx = rp_ctx;

    /* init binary tree for fast destination info lookup */
    rc = sr_btree_init(np_dst_info_cmp, np_dst_info_cleanup, &ctx->dst_info_btree);
    CHECK_RC_MSG_GOTO(rc, cleanup, "Cannot allocate binary tree for destination info lookup.");

    /* initialize subscriptions lock */
    ret = pthread_rwlock_init(&ctx->lock, NULL);
    CHECK_ZERO_MSG_GOTO(ret, rc, SR_ERR_INTERNAL, cleanup, "Subscriptions lock initialization failed.");

    SR_LOG_DBG_MSG("Notification Processor initialized successfully.");

    *np_ctx_p = ctx;
    return SR_ERR_OK;

cleanup:
    np_cleanup(ctx);
    return rc;
}

void
np_cleanup(np_ctx_t *np_ctx)
{
    SR_LOG_DBG_MSG("Notification Processor cleanup requested.");

    if (NULL != np_ctx) {
        for (size_t i = 0; i < np_ctx->subscription_cnt; i++) {
            free((void*)np_ctx->subscriptions[i]->dst_address);
            free((void*)np_ctx->subscriptions[i]->xpath);
            free(np_ctx->subscriptions[i]);
        }
        free(np_ctx->subscriptions);
        sr_btree_cleanup(np_ctx->dst_info_btree);
        pthread_rwlock_destroy(&np_ctx->lock);
        free(np_ctx);
    }
}

int
np_notification_subscribe(np_ctx_t *np_ctx, const ac_ucred_t *user_cred, Sr__NotificationEvent event_type,
        const char *dst_address, uint32_t dst_id, const char *module_name, const char *xpath)
{
    np_subscription_t *subscription = NULL;
    np_subscription_t **subscriptions_tmp = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(np_ctx, np_ctx->rp_ctx, dst_address);

    SR_LOG_DBG("Notification subscribe: event=%d, dst_address='%s', dst_id=%"PRIu32".", event_type, dst_address, dst_id);

    /* prepare new subscription entry */
    subscription = calloc(1, sizeof(*subscription));
    CHECK_NULL_NOMEM_RETURN(subscription);

    subscription->event_type = event_type;
    if (NULL != xpath) {
        subscription->xpath = strdup(xpath);
        CHECK_NULL_NOMEM_GOTO(subscription->xpath, rc, cleanup);
    }

    subscription->dst_id = dst_id;
    subscription->dst_address = strdup(dst_address);
    CHECK_NULL_NOMEM_GOTO(subscription->dst_address, rc, cleanup);

    /* save the new subscription */
    if (SR__NOTIFICATION_EVENT__MODULE_CHANGE_EV == event_type) {
        /* add the subscription to module's persistent data */
        rc = np_dst_info_insert(np_ctx, dst_address, module_name);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Unable to update notification destination info.");

        rc = pm_save_subscribtion_state(np_ctx->rp_ctx->pm_ctx, user_cred, module_name, subscription, true);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Unable to save the subscription into persistent data file.");

        goto cleanup; /* subscription not needed anymore */
    } else {
        /* add the subscription to in-memory subscription list */
        pthread_rwlock_wrlock(&np_ctx->lock);
        subscriptions_tmp = realloc(np_ctx->subscriptions, (np_ctx->subscription_cnt + 1) * sizeof(*subscriptions_tmp));
        CHECK_NULL_NOMEM_ERROR(subscriptions_tmp, rc);

        if (SR_ERR_OK == rc) {
            np_ctx->subscriptions = subscriptions_tmp;
            np_ctx->subscriptions[np_ctx->subscription_cnt] = subscription;
            np_ctx->subscription_cnt += 1;
            pthread_rwlock_unlock(&np_ctx->lock);
        } else {
            pthread_rwlock_unlock(&np_ctx->lock);
            goto cleanup;
        }
    }

    return SR_ERR_OK;

cleanup:
    if (NULL != subscription) {
        if (SR_ERR_OK != rc) {
            pthread_rwlock_wrlock(&np_ctx->lock);
            np_dst_info_remove(np_ctx, dst_address, module_name);
            pthread_rwlock_unlock(&np_ctx->lock);
        }
        free((void*)subscription->dst_address);
        free((void*)subscription->xpath);
        free(subscription);
    }
    return rc;
}

int
np_notification_unsubscribe(np_ctx_t *np_ctx,  const ac_ucred_t *user_cred, Sr__NotificationEvent event_type,
        const char *dst_address, uint32_t dst_id, const char *module_name)
{
    np_subscription_t *subscription = NULL, subscription_lookup = { 0, };
    size_t i = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(np_ctx, dst_address);

    SR_LOG_DBG("Notification unsubscribe: dst_address='%s', dst_id=%"PRIu32".", dst_address, dst_id);

    if (SR__NOTIFICATION_EVENT__MODULE_CHANGE_EV == event_type) {
        /* remove the subscription to module's persistent data */
        subscription_lookup.dst_address = dst_address;
        subscription_lookup.dst_id = dst_id;
        subscription_lookup.event_type = event_type;
        rc = pm_save_subscribtion_state(np_ctx->rp_ctx->pm_ctx, user_cred, module_name, &subscription_lookup, false);
        if (SR_ERR_OK == rc) {
            pthread_rwlock_wrlock(&np_ctx->lock);
            rc = np_dst_info_remove(np_ctx, dst_address, module_name);
            pthread_rwlock_unlock(&np_ctx->lock);
        }
    } else {
        /* remove the subscription from in-memory subscription list */

        /* find matching subscription */
        for (i = 0; i < np_ctx->subscription_cnt; i++) {
            if ((np_ctx->subscriptions[i]->dst_id == dst_id) &&
                    (0 == strcmp(np_ctx->subscriptions[i]->dst_address, dst_address))) {
                subscription = np_ctx->subscriptions[i];
                break;
            }
        }
        if (NULL == subscription) {
            SR_LOG_ERR("Subscription matching with dst_address='%s' and dst_id=%"PRIu32" not found.", dst_address, dst_id);
            return SR_ERR_INVAL_ARG;
        }

        /* remove the subscription from array */
        pthread_rwlock_wrlock(&np_ctx->lock);
        if (np_ctx->subscription_cnt > (i + 1)) {
            memmove(np_ctx->subscriptions + i, np_ctx->subscriptions + i + 1,
                    (np_ctx->subscription_cnt - i - 1) * sizeof(*np_ctx->subscriptions));
        }
        np_ctx->subscription_cnt -= 1;
        pthread_rwlock_unlock(&np_ctx->lock);

        /* release the subscription */
        free((void*)subscription->dst_address);
        free((void*)subscription->xpath);
        free(subscription);
    }

    return rc;
}

int
np_unsubscribe_destination(np_ctx_t *np_ctx, const char *dst_address)
{
    np_dst_info_t info_lookup = { 0, }, *info = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(np_ctx, dst_address);

    pthread_rwlock_wrlock(&np_ctx->lock);

    info_lookup.dst_address = dst_address;
    info = sr_btree_search(np_ctx->dst_info_btree, &info_lookup);
    if (NULL != info) {
        for (size_t i = 0; i < info->subscribed_modules_cnt; i++) {
            SR_LOG_DBG("Removing subscriptions for destination '%s' from '%s'.", dst_address,
                    info->subscribed_modules[i]);
            rc = pm_delete_subscriptions_for_destination(np_ctx->rp_ctx->pm_ctx,
                    info->subscribed_modules[i], dst_address);
            CHECK_RC_LOG_RETURN(rc, "Unable to remove subscriptions for destination '%s' from '%s'.", dst_address,
                    info->subscribed_modules[i]);
        }
        np_dst_info_remove(np_ctx, dst_address, NULL);
    }

    pthread_rwlock_unlock(&np_ctx->lock);

    return rc;
}

int
np_module_install_notify(np_ctx_t *np_ctx, const char *module_name, const char *revision, bool installed)
{
    Sr__Msg *notif = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(np_ctx, module_name);

    SR_LOG_DBG("Sending module-install notifications, module_name='%s', revision='%s', installed=%d.",
            module_name, revision, installed);

    pthread_rwlock_rdlock(&np_ctx->lock);

    for (size_t i = 0; i < np_ctx->subscription_cnt; i++) {
        if (SR__NOTIFICATION_EVENT__MODULE_INSTALL_EV == np_ctx->subscriptions[i]->event_type) {
            /* allocate the notification */
            rc = sr_pb_notif_alloc(SR__NOTIFICATION_EVENT__MODULE_INSTALL_EV,
                    np_ctx->subscriptions[i]->dst_address, np_ctx->subscriptions[i]->dst_id, &notif);
            /* fill-in notification details */
            if (SR_ERR_OK == rc) {
                notif->notification->module_install_notif->installed = installed;
                notif->notification->module_install_notif->module_name = strdup(module_name);
                CHECK_NULL_NOMEM_ERROR(notif->notification->module_install_notif->module_name, rc);
            }
            if (SR_ERR_OK == rc && NULL != revision) {
                notif->notification->module_install_notif->revision = strdup(revision);
                CHECK_NULL_NOMEM_ERROR(notif->notification->module_install_notif->revision, rc);
            }
            /* send the notification */
            if (SR_ERR_OK == rc) {
                SR_LOG_DBG("Sending a module-install notification to the destination address='%s', id=%"PRIu32".",
                        np_ctx->subscriptions[i]->dst_address, np_ctx->subscriptions[i]->dst_id);
                rc = cm_msg_send(np_ctx->rp_ctx->cm_ctx, notif);
            } else {
                sr__msg__free_unpacked(notif, NULL);
                break;
            }
        }
    }

    pthread_rwlock_unlock(&np_ctx->lock);

    return rc;
}

int
np_feature_enable_notify(np_ctx_t *np_ctx, const char *module_name, const char *feature_name, bool enabled)
{
    Sr__Msg *notif = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(np_ctx, module_name, feature_name);

    SR_LOG_DBG("Sending feature-enable notifications, module_name='%s', feature_name='%s', enabled=%d.",
                module_name, feature_name, enabled);

    pthread_rwlock_rdlock(&np_ctx->lock);

    for (size_t i = 0; i < np_ctx->subscription_cnt; i++) {
        if (SR__NOTIFICATION_EVENT__FEATURE_ENABLE_EV == np_ctx->subscriptions[i]->event_type) {
            /* allocate the notification */
            rc = sr_pb_notif_alloc(SR__NOTIFICATION_EVENT__FEATURE_ENABLE_EV,
                    np_ctx->subscriptions[i]->dst_address, np_ctx->subscriptions[i]->dst_id, &notif);
            /* fill-in notification details */
            if (SR_ERR_OK == rc) {
                notif->notification->feature_enable_notif->enabled = enabled;
                notif->notification->feature_enable_notif->module_name = strdup(module_name);
                CHECK_NULL_NOMEM_ERROR(notif->notification->feature_enable_notif->module_name, rc);
            }
            if (SR_ERR_OK == rc) {
                notif->notification->feature_enable_notif->feature_name = strdup(feature_name);
                CHECK_NULL_NOMEM_ERROR(notif->notification->feature_enable_notif->feature_name, rc);
            }
            /* send the notification */
            if (SR_ERR_OK == rc) {
                SR_LOG_DBG("Sending a feature-enable notification to the destination address='%s', id=%"PRIu32".",
                        np_ctx->subscriptions[i]->dst_address, np_ctx->subscriptions[i]->dst_id);
                rc = cm_msg_send(np_ctx->rp_ctx->cm_ctx, notif);
            } else {
                sr__msg__free_unpacked(notif, NULL);
                break;
            }
        }
    }

    pthread_rwlock_unlock(&np_ctx->lock);

    return rc;
}

int
np_module_change_notify(np_ctx_t *np_ctx, const char *module_name)
{
    np_subscription_t *subscriptions = NULL;
    size_t subscription_cnt = 0;
    Sr__Msg *notif = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(np_ctx, module_name);

    SR_LOG_DBG("Sending module-change notifications, module_name='%s'.", module_name);

    rc = pm_get_subscriptions(np_ctx->rp_ctx->pm_ctx, module_name, SR__NOTIFICATION_EVENT__MODULE_CHANGE_EV,
            &subscriptions, &subscription_cnt);

    for (size_t i = 0; i < subscription_cnt; i++) {
        /* allocate the notification */
        rc = sr_pb_notif_alloc(SR__NOTIFICATION_EVENT__MODULE_CHANGE_EV,
                subscriptions[i].dst_address, subscriptions[i].dst_id, &notif);
        /* fill-in notification details */
        if (SR_ERR_OK == rc) {
            notif->notification->module_change_notif->module_name = strdup(module_name);
            CHECK_NULL_NOMEM_ERROR(notif->notification->module_change_notif->module_name, rc);
        }
        /* save notification destination info */
        if (SR_ERR_OK == rc) {
            rc = np_dst_info_insert(np_ctx, subscriptions[i].dst_address, module_name);
        }
        /* send the notification */
        if (SR_ERR_OK == rc) {
            SR_LOG_DBG("Sending a module-change notification to the destination address='%s', id=%"PRIu32".",
                    subscriptions[i].dst_address, subscriptions[i].dst_id);
            rc = cm_msg_send(np_ctx->rp_ctx->cm_ctx, notif);
        } else {
            sr__msg__free_unpacked(notif, NULL);
            break;
        }
    }

    for (size_t i = 0; i < subscription_cnt; i++) {
        free((void*)subscriptions[i].dst_address);
    }
    free(subscriptions);

    return rc;
}
