/**
 * @file rp_dt_get.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief
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

#include <libyang/libyang.h>
#include "sysrepo.h"
#include "sr_common.h"

#include "access_control.h"
#include "rp_internal.h"
#include "rp_dt_get.h"
#include "rp_dt_xpath.h"

/**
 * @brief Fills sr_val_t from lyd_node structure. It fills xpath and copies the value.
 * @param [in] node
 * @param [out] value
 * @return Error code (SR_ERR_OK on success)
 */
static int
rp_dt_get_value_from_node(struct lyd_node *node, sr_val_t *val)
{
    CHECK_NULL_ARG3(node, val, node->schema);

    int rc = SR_ERR_OK;
    char *xpath = NULL;
    struct lyd_node_leaf_list *data_leaf = NULL;
    struct lys_node_container *sch_cont = NULL;

    rc = rp_dt_create_xpath_for_node(node, &xpath);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Create xpath for node failed");
        return rc;
    }
    val->xpath = xpath;

    switch (node->schema->nodetype) {
    case LYS_LEAF:
        data_leaf = (struct lyd_node_leaf_list *) node;
        val->dflt = node->dflt;

        val->type = sr_libyang_type_to_sysrepo(data_leaf->value_type);

        rc = sr_libyang_leaf_copy_value(data_leaf, val);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR_MSG("Copying of value failed");
            goto cleanup;
        }
        break;
    case LYS_CONTAINER:
        sch_cont = (struct lys_node_container *) node->schema;
        val->type = sch_cont->presence == NULL ? SR_CONTAINER_T : SR_CONTAINER_PRESENCE_T;
        break;
    case LYS_LIST:
        val->type = SR_LIST_T;
        break;
    case LYS_LEAFLIST:
        data_leaf = (struct lyd_node_leaf_list *) node;

        val->type = sr_libyang_type_to_sysrepo(data_leaf->value_type);

        rc = sr_libyang_leaf_copy_value(data_leaf, val);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR_MSG("Copying of value failed");
            goto cleanup;
        }
        break;
    default:
        SR_LOG_WRN_MSG("Get value is not implemented for this node type");
        rc = SR_ERR_INTERNAL;
        goto cleanup;
    }
    return SR_ERR_OK;

cleanup:
    sr_free_val_content(val);
    return rc;
}

int
rp_dt_get_values_from_nodes(struct ly_set *nodes, sr_val_t **values, size_t *value_cnt)
{
    CHECK_NULL_ARG2(nodes, values);
    int rc = SR_ERR_OK;
    sr_val_t *vals = NULL;
    size_t cnt = 0;
    struct lyd_node *node = NULL;

    vals = calloc(nodes->number, sizeof(*vals));
    CHECK_NULL_NOMEM_RETURN(vals);

    for (size_t i = 0; i < nodes->number; i++) {
        node = nodes->set.d[i];
        if (NULL == node || NULL == node->schema || LYS_RPC == node->schema->nodetype) {
            /* ignore this node */
            continue;
        }
        rc = rp_dt_get_value_from_node(node, &vals[i]);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Getting value from node %s failed", node->schema->name);
            free(vals);
            return SR_ERR_INTERNAL;
        }
        cnt++;
    }

    *values = vals;
    *value_cnt = cnt;

    return rc;
}

int
rp_dt_get_value(const dm_ctx_t *dm_ctx, struct lyd_node *data_tree, const char *xpath, bool check_enabled, sr_val_t **value)
{
    CHECK_NULL_ARG4(dm_ctx, data_tree, xpath, value);
    int rc = SR_ERR_OK;
    sr_val_t *val = NULL;
    struct lyd_node *node = NULL;

    rc = rp_dt_find_node(dm_ctx, data_tree, xpath, check_enabled, &node);
    if (SR_ERR_OK != rc) {
        if (SR_ERR_NOT_FOUND != rc) {
            SR_LOG_ERR("Find node failed (%d) xpath %s", rc, xpath);
        }
        return rc;
    }

    val = calloc(1, sizeof(*val));
    CHECK_NULL_NOMEM_RETURN(val);

    rc = rp_dt_get_value_from_node(node, val);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get value from node failed for xpath %s", xpath);
        free(val);
    } else {
        *value = val;
    }

    return rc;
}

int
rp_dt_get_values(const dm_ctx_t *dm_ctx, struct lyd_node *data_tree, const char *xpath, bool check_enable, sr_val_t **values, size_t *count)
{
    CHECK_NULL_ARG5(dm_ctx, data_tree, xpath, values, count);

    int rc = SR_ERR_OK;

    struct ly_set *nodes = NULL;
    rc = rp_dt_find_nodes(dm_ctx, data_tree, xpath, check_enable, &nodes);
    if (SR_ERR_OK != rc) {
        if (SR_ERR_NOT_FOUND != rc) {
            SR_LOG_ERR("Get nodes for xpath %s failed (%d)", xpath, rc);
        }
        return rc;
    }

    rc = rp_dt_get_values_from_nodes(nodes, values, count);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying values from nodes failed for xpath '%s'", xpath);
    }

    ly_set_free(nodes);
    return SR_ERR_OK;
}

int
rp_dt_get_value_wrapper(rp_ctx_t *rp_ctx, rp_session_t *rp_session, const char *xpath, sr_val_t **value)
{
    CHECK_NULL_ARG4(rp_ctx, rp_ctx->dm_ctx, rp_session, rp_session->dm_session);
    CHECK_NULL_ARG2(xpath, value);
    SR_LOG_INF("Get item request %s datastore, xpath: %s", sr_ds_to_str(rp_session->datastore), xpath);

    int rc = SR_ERR_INVAL_ARG;
    struct lyd_node *data_tree = NULL;
    char *data_tree_name = NULL;

    rc = ac_check_node_permissions(rp_session->ac_session, xpath, AC_OPER_READ);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Access control check failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = sr_copy_first_ns(xpath, &data_tree_name);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying module name failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = dm_get_datatree(rp_ctx->dm_ctx, rp_session->dm_session, data_tree_name, &data_tree);
    if (SR_ERR_OK != rc) {
        if (SR_ERR_NOT_FOUND != rc) {
            SR_LOG_ERR("Getting data tree failed (%d) for xpath '%s'", rc, xpath);
        }
        goto cleanup;
    }

    rc = rp_dt_get_value(rp_ctx->dm_ctx, data_tree, xpath, dm_is_running_ds_session(rp_session->dm_session), value);
cleanup:
    if (SR_ERR_NOT_FOUND == rc) {
#if 0
        rc = rp_dt_validate_node_xpath(rp_ctx->dm_ctx, rp_session->dm_session, xpath, NULL, NULL);
        rc = rc == SR_ERR_OK ? SR_ERR_NOT_FOUND : rc;
#endif
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get value failed for xpath '%s'", xpath);
    }

    free(data_tree_name);
    return rc;
}

int
rp_dt_get_values_wrapper(rp_ctx_t *rp_ctx, rp_session_t *rp_session, const char *xpath, sr_val_t **values, size_t *count)
{
    CHECK_NULL_ARG4(rp_ctx, rp_ctx->dm_ctx, rp_session, rp_session->dm_session);
    CHECK_NULL_ARG3(xpath, values, count);
    SR_LOG_INF("Get items request %s datastore, xpath: %s", sr_ds_to_str(rp_session->datastore), xpath);

    int rc = SR_ERR_INVAL_ARG;
    struct lyd_node *data_tree = NULL;
    char *data_tree_name = NULL;

    rc = sr_copy_first_ns(xpath, &data_tree_name);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying module name failed for xpath '%s'", xpath);
        goto cleanup;
    }
    rc = ac_check_node_permissions(rp_session->ac_session, xpath, AC_OPER_READ);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Access control check failed for xpath '%s'", xpath);
        goto cleanup;
    }
    rc = dm_get_datatree(rp_ctx->dm_ctx, rp_session->dm_session, data_tree_name, &data_tree);
    if (SR_ERR_OK != rc) {
        if (SR_ERR_NOT_FOUND != rc) {
            SR_LOG_ERR("Getting data tree failed (%d) for xpath '%s'", rc, xpath);
        }
        goto cleanup;
    }

    rc = rp_dt_get_values(rp_ctx->dm_ctx, data_tree, xpath, dm_is_running_ds_session(rp_session->dm_session), values, count);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get values failed for xpath '%s'", xpath);
    }

cleanup:
    if (SR_ERR_NOT_FOUND == rc || (SR_ERR_OK == rc && 0 == count)) {
#if 0
        rc = rp_dt_validate_node_xpath(rp_ctx->dm_ctx, rp_session->dm_session, xpath, NULL, NULL);
        rc = rc == SR_ERR_OK ? SR_ERR_NOT_FOUND : rc;
#endif
    }
    free(data_tree_name);
    return rc;
}

int
rp_dt_get_values_wrapper_with_opts(rp_ctx_t *rp_ctx, rp_session_t *rp_session, rp_dt_get_items_ctx_t *get_items_ctx, const char *xpath,
        size_t offset, size_t limit, sr_val_t **values, size_t *count)
{
    CHECK_NULL_ARG5(rp_ctx, rp_ctx->dm_ctx, rp_session, rp_session->dm_session, get_items_ctx);
    CHECK_NULL_ARG3(xpath, values, count);
    SR_LOG_INF("Get items request %s datastore, xpath: %s, offset: %zu, limit: %zu", sr_ds_to_str(rp_session->datastore), xpath, offset, limit);

    int rc = SR_ERR_INVAL_ARG;
    struct lyd_node *data_tree = NULL;
    struct ly_set *nodes = NULL;
    char *data_tree_name = NULL;

    rc = sr_copy_first_ns(xpath, &data_tree_name);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying module name failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = ac_check_node_permissions(rp_session->ac_session, xpath, AC_OPER_READ);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Access control check failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = dm_get_datatree(rp_ctx->dm_ctx, rp_session->dm_session, data_tree_name, &data_tree);
    if (SR_ERR_OK != rc) {
        if (SR_ERR_NOT_FOUND != rc) {
            SR_LOG_ERR("Getting data tree failed (%d) for xpath '%s'", rc, xpath);
        }
        goto cleanup;
    }

    rc = rp_dt_find_nodes_with_opts(rp_ctx->dm_ctx, rp_session->dm_session, get_items_ctx, data_tree, xpath, offset, limit, &nodes);
    if (SR_ERR_OK != rc) {
        if (SR_ERR_NOT_FOUND != rc) {
            SR_LOG_ERR("Get nodes for xpath %s failed (%d)", xpath, rc);
        }
        goto cleanup;
    }

    rc = rp_dt_get_values_from_nodes(nodes, values, count);
cleanup:
    if (SR_ERR_NOT_FOUND == rc) {
#if 0
        rc = rp_dt_validate_node_xpath(rp_ctx->dm_ctx, rp_session->dm_session, xpath, NULL, NULL);
        rc = rc == SR_ERR_OK ? SR_ERR_NOT_FOUND : rc;
#endif
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying values from nodes failed for xpath '%s'", xpath);
    }

    ly_set_free(nodes);
    free(data_tree_name);
    return rc;

}
