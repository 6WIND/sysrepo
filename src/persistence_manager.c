/**
 * @file persistence_manager.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Sysrepo's Persistence Manager implementation.
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
#include <inttypes.h>
#include <pthread.h>
#include <fcntl.h>
#include <libyang/libyang.h>

#include "sr_common.h"
#include "access_control.h"
#include "persistence_manager.h"

#define PM_SCHEMA_FILE "sysrepo-persistent-data.yin"  /**< Schema of module's persistent data. */

#define PM_XPATH_MODULE           "/sysrepo-persistent-data:module[name='%s']"

#define PM_XPATH_FEATURES         PM_XPATH_MODULE "/enabled-features/feature-name"
#define PM_XPATH_FEATURES_BY_NAME PM_XPATH_MODULE "/enabled-features/feature-name[text()='%s']"

#define PM_XPATH_SUBSCRIPTIONS             PM_XPATH_MODULE "/subscriptions/subscription[type='%s'][destination-address='%s'][destination-id='%"PRIu32"']"
#define PM_XPATH_SUBSCRIPTIONS_BY_TYPE     PM_XPATH_MODULE "/subscriptions/subscription[type='%s']"
#define PM_XPATH_SUBSCRIPTIONS_BY_DST_ID   PM_XPATH_MODULE "/subscriptions/subscription[destination-address='%s'][destination-id='%"PRIu32"']"
#define PM_XPATH_SUBSCRIPTIONS_BY_DST_ADDR PM_XPATH_MODULE "/subscriptions/subscription[destination-address='%s']"


/**
 * @brief Persistence Manager context.
 */
typedef struct pm_ctx_s {
    ac_ctx_t *ac_ctx;                 /**< Access Control module context. */
    struct ly_ctx *ly_ctx;            /**< libyang context used locally in PM. */
    const struct lys_module *schema;  /**< Schema tree of sysrepo-persistent-data YANG. */
    const char *data_search_dir;      /**< Directory containing the data files. */
} pm_ctx_t;

/**
 * @brief Saves the data tree into the file specified by file descriptor.
 */
static int
pm_save_data_tree(pm_ctx_t *pm_ctx, int fd, struct lyd_node *data_tree)
{
    int ret = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(pm_ctx, data_tree);

    /* empty file content */
    ret = ftruncate(fd, 0);
    if (0 != ret) {
        SR_LOG_ERR("File truncate failed: %s", strerror(errno));
        rc = SR_ERR_INTERNAL;
    }

    /* print data tree to file */
    rc = lyd_print_fd(fd, data_tree, LYD_XML, LYP_WITHSIBLINGS | LYP_FORMAT);
    if (0 != rc) {
        SR_LOG_ERR("Saving persist data tree failed: %s", ly_errmsg());
        rc = SR_ERR_INTERNAL;
    } else {
        SR_LOG_DBG_MSG("Persist data tree successfully saved.");
        rc = SR_ERR_OK;
    }

    /* flush in-core data to the disc */
    ret = fdatasync(fd);
    if (0 != ret) {
        SR_LOG_ERR("File synchronization failed: %s", strerror(errno));
        rc = SR_ERR_INTERNAL;
    }

    return rc;
}

/**
 * @brief Loads the data tree of persistent data file tied to specified YANG module.
 */
static int
pm_load_data_tree(pm_ctx_t *pm_ctx, const ac_ucred_t *user_cred, const char *module_name,  const char *data_filename,
        bool read_only, int *fd_p, struct lyd_node **data_tree)
{
    int fd = -1;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(pm_ctx, module_name, data_filename, data_tree);

    /* open the file as the proper user */
    if (NULL != user_cred) {
        ac_set_user_identity(pm_ctx->ac_ctx, user_cred);
    }

    fd = open(data_filename, (read_only ? O_RDONLY : O_RDWR));

    if (NULL != user_cred) {
        ac_unset_user_identity(pm_ctx->ac_ctx);
    }

    if (-1 == fd) {
        /* error by open */
        if (ENOENT == errno) {
            SR_LOG_DBG("Persist data file '%s' does not exist.", data_filename);
            if (read_only) {
                rc = SR_ERR_DATA_MISSING;
            } else {
                /* create new persist file */
                ac_set_user_identity(pm_ctx->ac_ctx, user_cred);
                fd = open(data_filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
                ac_unset_user_identity(pm_ctx->ac_ctx);
                if (-1 == fd) {
                    SR_LOG_ERR("Unable to create new persist data file '%s': %s", data_filename, strerror(errno));
                    rc = SR_ERR_INTERNAL;
                }
            }
        } else if (EACCES == errno) {
            SR_LOG_ERR("Insufficient permissions to access persist data file '%s'.", data_filename);
            rc = SR_ERR_UNAUTHORIZED;
        } else {
            SR_LOG_ERR("Unable to open persist data file '%s': %s.", data_filename, strerror(errno));
            rc = SR_ERR_INTERNAL;
        }
        CHECK_RC_LOG_RETURN(rc, "Persist data tree load for '%s' has failed.", module_name);
    }

    /* lock & load the data tree */
    sr_lock_fd(fd, (read_only ? false : true), true);

    *data_tree = lyd_parse_fd(pm_ctx->ly_ctx, fd, LYD_XML, LYD_OPT_STRICT | LYD_OPT_CONFIG);
    if (NULL == *data_tree && LY_SUCCESS != ly_errno) {
        SR_LOG_ERR("Parsing persist data from file '%s' failed: %s", data_filename, ly_errmsg());
        rc = SR_ERR_INTERNAL;
    } else {
        SR_LOG_DBG("Persist data successfully loaded from file '%s'.", data_filename);
    }

    if (read_only || NULL == fd_p) {
        /* unlock and close fd in case of read_only has been requested */
        sr_unlock_fd(fd);
        close(fd);
    } else {
        /* return open fd to locked file otherwise */
        *fd_p = fd;
    }

    return rc;
}

/**
 * @brief Logging callback called from libyang for each log entry.
 */
static void
pm_ly_log_cb(LY_LOG_LEVEL level, const char *msg, const char *path)
{
    if (LY_LLERR == level) {
        SR_LOG_DBG("libyang error: %s", msg);
    }
}

/**
 * @brief Saves/deletes provided data on provided xpath location within the
 * persistent data file of a module.
 */
static int
pm_save_persistent_data(pm_ctx_t *pm_ctx, const ac_ucred_t *user_cred, const char *module_name,
        bool add, const char *xpath, const char *value)
{
    char *data_filename = NULL;
    struct lyd_node *data_tree = NULL, *new_node = NULL;
    struct ly_set *node_set = NULL;
    int fd = -1;
    int rc = SR_ERR_OK, ret = 0;

    CHECK_NULL_ARG3(pm_ctx, module_name, xpath);

    /* get persist file path */
    rc = sr_get_persist_data_file_name(pm_ctx->data_search_dir, module_name, &data_filename);
    CHECK_RC_LOG_RETURN(rc, "Unable to compose persist data file name for '%s'.", module_name);

    /* load the data tree from persist file */
    rc = pm_load_data_tree(pm_ctx, user_cred, module_name, data_filename, false, &fd, &data_tree);
    CHECK_RC_LOG_RETURN(rc, "Unable to load persist data tree for module '%s'.", module_name);

    if (NULL == data_tree && !add) {
        SR_LOG_ERR("Persist data tree for module '%s' is empty.", module_name);
        rc = SR_ERR_DATA_MISSING;
        goto cleanup;
    }

    if (add) {
        /* add persistent data */
        new_node = lyd_new_path(data_tree, pm_ctx->ly_ctx, xpath, value, 0);
        if (NULL == data_tree) {
            /* if the new data tree has been just created */
            data_tree = new_node;
        }
        if (NULL == new_node) {
            SR_LOG_ERR("Unable to add new persistent data (module=%s, xpath=%s): %s.", module_name, xpath, ly_errmsg());
            rc = SR_ERR_DATA_EXISTS;
            goto cleanup;
        }
    } else {
        /* delete persistent data */
        node_set = lyd_get_node(data_tree, xpath);
        if (NULL == node_set || LY_SUCCESS != ly_errno) {
            SR_LOG_ERR("Unable to find requested persistent data (module=%s, xpath=%s): %s.", module_name, xpath, ly_errmsg());
            rc = SR_ERR_INTERNAL;
            goto cleanup;
        }
        for (size_t i = 0; i < node_set->number; i++) {
            ret = lyd_unlink(node_set->set.d[i]);
            if (0 != ret) {
                SR_LOG_ERR("Unable to delete persistent data (module=%s, xpath=%s): %s.", module_name, xpath, ly_errmsg());
                rc = SR_ERR_INTERNAL;
                goto cleanup;
            } else {
                lyd_free(node_set->set.d[i]);
            }
        }
    }

    /* save the changes */
    rc = pm_save_data_tree(pm_ctx, fd, data_tree);

cleanup:
    if (NULL != node_set) {
        ly_set_free(node_set);
    }
    if (NULL != data_tree) {
        lyd_free_withsiblings(data_tree);
    }
    free(data_filename);

    if (-1 != fd) {
        sr_unlock_fd(fd);
        close(fd);
    }

    return rc;
}

int
pm_init(ac_ctx_t *ac_ctx, const char *schema_search_dir, const char *data_search_dir, pm_ctx_t **pm_ctx)
{
    pm_ctx_t *ctx = NULL;
    char *schema_filename = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(ac_ctx, schema_search_dir, data_search_dir, pm_ctx);

    /* allocate and initialize the context */
    ctx = calloc(1, sizeof(*ctx));
    CHECK_NULL_NOMEM_GOTO(ctx, rc, cleanup);

    ctx->ac_ctx = ac_ctx;
    ctx->data_search_dir = strdup(data_search_dir);
    CHECK_NULL_NOMEM_GOTO(ctx->data_search_dir, rc, cleanup);

    /* initialize libyang */
    ctx->ly_ctx = ly_ctx_new(schema_search_dir);
    if (NULL == ctx->ly_ctx) {
        SR_LOG_ERR("libyang initialization failed: %s", ly_errmsg());
        rc = SR_ERR_INIT_FAILED;
        goto cleanup;
    }

    ly_set_log_clb(pm_ly_log_cb, 0);

    rc = sr_str_join(schema_search_dir, PM_SCHEMA_FILE, &schema_filename);
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }

    /* load persist files schema to context */
    ctx->schema = lys_parse_path(ctx->ly_ctx, schema_filename, LYS_IN_YIN);
    free(schema_filename);
    if (NULL == ctx->schema) {
        SR_LOG_WRN("Unable to parse the schema file '%s': %s", PM_SCHEMA_FILE, ly_errmsg());
        rc = SR_ERR_INTERNAL;
        goto cleanup;
    }

    *pm_ctx = ctx;
    return SR_ERR_OK;

cleanup:
    pm_cleanup(ctx);
    return rc;
}

void
pm_cleanup(pm_ctx_t *pm_ctx)
{
    if (NULL != pm_ctx) {
        if (NULL != pm_ctx->ly_ctx) {
            ly_ctx_destroy(pm_ctx->ly_ctx, NULL);
        }
        free((void*)pm_ctx->data_search_dir);
        free(pm_ctx);
    }
}

int
pm_save_feature_state(pm_ctx_t *pm_ctx, const ac_ucred_t *user_cred, const char *module_name,
        const char *feature_name, bool enable)
{
    char xpath[PATH_MAX] = { 0, };
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(pm_ctx, user_cred, module_name, feature_name);

    if (enable) {
        /* enable the feature */
        snprintf(xpath, PATH_MAX, PM_XPATH_FEATURES, module_name);

        rc = pm_save_persistent_data(pm_ctx, user_cred, module_name, true, xpath, feature_name);

        if (SR_ERR_OK == rc) {
            SR_LOG_DBG("Feature '%s' successfully enabled in '%s' persist data tree.", feature_name, module_name);
        }
    } else {
        /* disable the feature */
        snprintf(xpath, PATH_MAX, PM_XPATH_FEATURES_BY_NAME, module_name, feature_name);

        rc = pm_save_persistent_data(pm_ctx, user_cred, module_name, false, xpath, NULL);

        if (SR_ERR_OK == rc) {
            SR_LOG_DBG("Feature '%s' successfully disabled in '%s' persist file.", feature_name, module_name);
        }
    }

    return rc;
}

int
pm_get_features(pm_ctx_t *pm_ctx, const char *module_name, char ***features_p, size_t *feature_cnt_p)
{
    char *data_filename = NULL;
    char xpath[PATH_MAX] = { 0, };
    struct lyd_node *data_tree = NULL;
    struct ly_set *node_set = NULL;
    char **features = NULL;
    const char *feature_name = NULL;
    size_t feature_cnt = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(pm_ctx, module_name, features_p, feature_cnt_p);

    /* get persist file path */
    rc = sr_get_persist_data_file_name(pm_ctx->data_search_dir, module_name, &data_filename);
    CHECK_RC_LOG_RETURN(rc, "Unable to compose persist data file name for '%s'.", module_name);

    /* load the data tree from persist file */
    rc = pm_load_data_tree(pm_ctx, NULL, module_name, data_filename, true, NULL, &data_tree);
    CHECK_RC_LOG_GOTO(rc, cleanup, "Unable to load persist data tree for module '%s'.", module_name);

    if (NULL == data_tree) {
        /* empty data file */
        *features_p = NULL;
        *feature_cnt_p = 0;
        goto cleanup;
    }

    snprintf(xpath, PATH_MAX, PM_XPATH_FEATURES, module_name);
    node_set = lyd_get_node(data_tree, xpath);

    if (NULL != node_set && node_set->number > 0) {
        features = calloc(node_set->number, sizeof(*features));
        CHECK_NULL_NOMEM_GOTO(features, rc, cleanup);

        for (size_t i = 0; i < node_set->number; i++) {
            feature_name = ((struct lyd_node_leaf_list *)node_set->set.d[i])->value_str;
            if (NULL != feature_name) {
                features[feature_cnt] = strdup(feature_name);
                CHECK_NULL_NOMEM_GOTO(features[feature_cnt], rc, cleanup);
                feature_cnt++;
            }
        }
    }

    SR_LOG_DBG("Returning %zu features enabled in '%s' persist file.", feature_cnt, module_name);

    *features_p = features;
    *feature_cnt_p = feature_cnt;

cleanup:
    if (NULL != node_set) {
        ly_set_free(node_set);
    }
    if (NULL != data_tree) {
        lyd_free_withsiblings(data_tree);
    }
    free(data_filename);

    if (SR_ERR_OK != rc) {
        for (size_t i = 0; i < feature_cnt; i++) {
            free((void*)features[i]);
        }
        free(features);
    }
    return rc;
}

int
pm_save_subscribtion_state(pm_ctx_t *pm_ctx, const ac_ucred_t *user_cred, const char *module_name,
        const np_subscription_t *subscription, const bool subscribe)
{
    char xpath[PATH_MAX] = { 0, };
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(pm_ctx, user_cred, module_name, subscription);

    snprintf(xpath, PATH_MAX, PM_XPATH_SUBSCRIPTIONS, module_name,
            sr_event_gpb_to_str(subscription->event_type), subscription->dst_address, subscription->dst_id);

    if (subscribe) {
        /* add the subscription */
        rc = pm_save_persistent_data(pm_ctx, user_cred, module_name, true, xpath, NULL);

        if (SR_ERR_OK == rc) {
            SR_LOG_DBG("Subscription entry successfully added into '%s' persist data tree.", module_name);
        }
    } else {
        /* remove the subscription */
        rc = pm_save_persistent_data(pm_ctx, user_cred, module_name, false, xpath, NULL);

        if (SR_ERR_OK == rc) {
            SR_LOG_DBG("Subscription entry successfully removed from '%s' persist file.", module_name);
        }
    }

    return rc;
}

int
pm_remove_subscriptions_for_destination(pm_ctx_t *pm_ctx, const char *module_name, const char *dst_address)
{
    char xpath[PATH_MAX] = { 0, };
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(pm_ctx, module_name, dst_address);

    snprintf(xpath, PATH_MAX, PM_XPATH_SUBSCRIPTIONS_BY_DST_ADDR, module_name, dst_address);

    /* remove the subscriptions */
    rc = pm_save_persistent_data(pm_ctx, NULL, module_name, false, xpath, NULL);

    if (SR_ERR_OK == rc) {
        SR_LOG_DBG("Subscription entries for destination '%s' successfully removed from '%s' persist file.",
                dst_address, module_name);
    }

    return rc;
}

int
pm_get_subscriptions(pm_ctx_t *pm_ctx, const char *module_name, Sr__NotificationEvent event_type,
        np_subscription_t **subscriptions_p, size_t *subscription_cnt_p)
{
    char *data_filename = NULL;
    char xpath[PATH_MAX] = { 0, };
    struct lyd_node *data_tree = NULL, *node = NULL;
    struct ly_set *node_set = NULL;
    struct lyd_node_leaf_list *node_ll = NULL;
    np_subscription_t *subscriptions = NULL;
    size_t subscription_cnt = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG4(pm_ctx, module_name, subscriptions_p, subscription_cnt_p);

    /* get persist file path */
    rc = sr_get_persist_data_file_name(pm_ctx->data_search_dir, module_name, &data_filename);
    CHECK_RC_LOG_RETURN(rc, "Unable to compose persist data file name for '%s'.", module_name);

    /* load the data tree from persist file */
    rc = pm_load_data_tree(pm_ctx, NULL, module_name, data_filename, true, NULL, &data_tree);
    CHECK_RC_LOG_GOTO(rc, cleanup, "Unable to load persist data tree for module '%s'.", module_name);

    if (NULL == data_tree) {
        /* empty data file */
        *subscriptions_p = NULL;
        *subscription_cnt_p = 0;
        goto cleanup;
    }

    snprintf(xpath, PATH_MAX, PM_XPATH_SUBSCRIPTIONS_BY_TYPE, module_name, sr_event_gpb_to_str(event_type));
    node_set = lyd_get_node(data_tree, xpath);

    if (NULL != node_set && node_set->number > 0) {
        subscriptions = calloc(node_set->number, sizeof(*subscriptions));
        CHECK_NULL_NOMEM_GOTO(subscriptions, rc, cleanup);

        for (size_t i = 0; i < node_set->number; i++) {
            node = node_set->set.d[i]->child;
            while (NULL != node) {
                if (NULL != node->schema->name) {
                    node_ll = (struct lyd_node_leaf_list*)node;
                    if (NULL != node_ll->value_str && 0 == strcmp(node->schema->name, "type")) {
                        subscriptions[subscription_cnt].event_type = sr_event_str_to_gpb(node_ll->value_str);
                    }
                    if (NULL != node_ll->value_str && 0 == strcmp(node->schema->name, "destination-address")) {
                        subscriptions[subscription_cnt].dst_address = strdup(node_ll->value_str);
                        CHECK_NULL_NOMEM_GOTO(subscriptions[subscription_cnt].dst_address, rc, cleanup);
                    }
                    if (NULL != node_ll->value_str && 0 == strcmp(node->schema->name, "destination-id")) {
                        subscriptions[subscription_cnt].dst_id = atoi(node_ll->value_str);
                    }
                    node = node->next;
                }
            }
            subscription_cnt++;
        }
    }

    SR_LOG_DBG("Returning %zu subscriptions found in '%s' persist file.", subscription_cnt, module_name);

    *subscriptions_p = subscriptions;
    *subscription_cnt_p = subscription_cnt;

cleanup:
    if (NULL != node_set) {
        ly_set_free(node_set);
    }
    if (NULL != data_tree) {
        lyd_free_withsiblings(data_tree);
    }
    free(data_filename);

    if (SR_ERR_OK != rc) {
        for (size_t i = 0; i < subscription_cnt; i++) {
            free((void*)subscriptions[i].dst_address);
        }
        free(subscriptions);
    }

    return rc;
}
