/**
 * @file rp_get.c
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

#include "rp_dt_get.h"
#include "rp_dt_xpath.h"
#include <libyang/libyang.h>
#include "sr_common.h"
#include "xpath_processor.h"
#include "sysrepo.h"

/**
 * Functions copies the bits into string
 * @param [in] leaf - data tree node from the bits will be copied
 * @param [out] dest - space separated set bit field
 */
static int
rp_dt_copy_bits(const struct lyd_node_leaf_list *leaf, char **dest)
{
    CHECK_NULL_ARG3(leaf, dest, leaf->schema);

    struct lys_node_leaf *sch = (struct lys_node_leaf *) leaf->schema;
    char *bits_str = NULL;
    int bits_count = sch->type.info.bits.count;
    struct lys_type_bit **bits = leaf->value.bit;

    size_t length = 1; /* terminating NULL byte*/
    for (int i = 0; i < bits_count; i++) {
        if (NULL != bits[i] && NULL != bits[i]->name) {
            length += strlen(bits[i]->name);
            length++; /*space after bit*/
        }
    }
    bits_str = calloc(length, sizeof(*bits_str));
    if (NULL == bits_str) {
        SR_LOG_ERR_MSG("Memory allocation failed");
        return SR_ERR_NOMEM;
    }
    size_t offset = 0;
    for (int i = 0; i < bits_count; i++) {
        if (NULL != bits[i] && NULL != bits[i]->name) {
            strcpy(bits_str + offset, bits[i]->name);
            offset += strlen(bits[i]->name);
            bits_str[offset] = ' ';
            offset++;
        }
    }
    if (0 != offset) {
        bits_str[offset - 1] = '\0';
    }

    *dest = bits_str;
    return SR_ERR_OK;
}

static int
rp_dt_copy_value(const struct lyd_node_leaf_list *leaf, LY_DATA_TYPE type, sr_val_t *value)
{
    CHECK_NULL_ARG2(leaf, value);
    int rc = SR_ERR_OK;
    struct lys_node_leaf *leaf_schema = NULL;
    if (NULL == leaf->schema || NULL == leaf->schema->name) {
        SR_LOG_ERR_MSG("Missing schema information");
        return SR_ERR_INTERNAL;
    }

    switch (type) {
    case LY_TYPE_BINARY:
        if (NULL == leaf->value.binary) {
            SR_LOG_ERR("Binary data in leaf '%s' is NULL", leaf->schema->name);
            return SR_ERR_INTERNAL;
        }
        value->data.binary_val = strdup(leaf->value.binary);
        if (NULL == value->data.binary_val) {
            SR_LOG_ERR("Copy value failed for leaf '%s' of type 'binary'", leaf->schema->name);
            return SR_ERR_INTERNAL;
        }
        return SR_ERR_OK;
    case LY_TYPE_BITS:
        if (NULL == leaf->value.bit) {
            SR_LOG_ERR("Missing schema information for node '%s'", leaf->schema->name);
        }
        rc = rp_dt_copy_bits(leaf, &(value->data.bits_val));
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Copy value failed for leaf '%s' of type 'bits'", leaf->schema->name);
        }
        return rc;
    case LY_TYPE_BOOL:
        value->data.bool_val = leaf->value.bln;
        return SR_ERR_OK;
    case LY_TYPE_DEC64:
        value->data.decimal64_val = (double) leaf->value.dec64;
        leaf_schema = (struct lys_node_leaf *) leaf->schema;
        for (size_t i = 0; i < leaf_schema->type.info.dec64.dig; i++) {
            /* shift decimal point*/
            value->data.decimal64_val *= 0.1;
        }
        return SR_ERR_OK;
    case LY_TYPE_EMPTY:
        return SR_ERR_OK;
    case LY_TYPE_ENUM:
        if (NULL == leaf->value.enm || NULL == leaf->value.enm->name) {
            SR_LOG_ERR("Missing schema information for node '%s'", leaf->schema->name);
            return SR_ERR_INTERNAL;
        }
        value->data.enum_val = strdup(leaf->value.enm->name);
        if (NULL == value->data.enum_val) {
            SR_LOG_ERR("Copy value failed for leaf '%s' of type 'enum'", leaf->schema->name);
            return SR_ERR_INTERNAL;
        }
        return SR_ERR_OK;
    case LY_TYPE_IDENT:
        if (NULL == leaf->value.ident->name) {
            SR_LOG_ERR("Identity ref in leaf '%s' is NULL", leaf->schema->name);
            return SR_ERR_INTERNAL;
        }
        value->data.identityref_val = strdup(leaf->value.ident->name);
        if (NULL == value->data.identityref_val) {
            SR_LOG_ERR("Copy value failed for leaf '%s' of type 'identityref'", leaf->schema->name);
            return SR_ERR_INTERNAL;
        }
        return SR_ERR_OK;
    case LY_TYPE_INST:
        /* NOT IMPLEMENTED yet*/
        if (NULL != leaf->schema && NULL != leaf->schema->name) {
            SR_LOG_ERR("Copy value failed for leaf '%s'", leaf->schema->name);
        }
        return SR_ERR_INTERNAL;
    case LY_TYPE_STRING:
        value->data.string_val = strdup(leaf->value.string);
        if (NULL == value->data.string_val) {
            SR_LOG_ERR_MSG("String duplication failed");
            return SR_ERR_NOMEM;
        }
        return SR_ERR_OK;
    case LY_TYPE_UNION:
        /* Copy of selected union type should be called instead */
        SR_LOG_ERR("Can not copy value of union '%s'", leaf->schema->name);
        return SR_ERR_INTERNAL;
    case LY_TYPE_INT8:
        value->data.int8_val = leaf->value.int8;
        return SR_ERR_OK;
    case LY_TYPE_UINT8:
        value->data.uint8_val = leaf->value.uint8;
        return SR_ERR_OK;
    case LY_TYPE_INT16:
        value->data.int16_val = leaf->value.int16;
        return SR_ERR_OK;
    case LY_TYPE_UINT16:
        value->data.uint16_val = leaf->value.uint16;
        return SR_ERR_OK;
    case LY_TYPE_INT32:
        value->data.int32_val = leaf->value.int32;
        return SR_ERR_OK;
    case LY_TYPE_UINT32:
        value->data.uint32_val = leaf->value.uint32;
        return SR_ERR_OK;
    case LY_TYPE_INT64:
        value->data.int64_val = leaf->value.int64;
        return SR_ERR_OK;
    case LY_TYPE_UINT64:
        value->data.uint64_val = leaf->value.uint64;
        return SR_ERR_OK;
    default:
        SR_LOG_ERR("Copy value failed for leaf '%s'", leaf->schema->name);
        return SR_ERR_INTERNAL;
    }
}

/**
 * @brief Fills sr_val_t from lyd_node structure. It fills xpath and copies the value.
 * @param [in] node
 * @param [out] value
 * @return err_code
 */
static int
rp_dt_get_value_from_node(struct lyd_node *node, sr_val_t **value)
{
    CHECK_NULL_ARG3(node, value, node->schema);

    int rc = SR_ERR_OK;
    char *xpath = NULL;
    struct lyd_node_leaf_list *data_leaf = NULL;
    struct lys_node_container *sch_cont = NULL;
    rc = rp_dt_create_xpath_for_node(node, &xpath);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Create xpath for node failed");
        return rc;
    }

    sr_val_t *val = calloc(1, sizeof(*val));
    if (NULL == val) {
        SR_LOG_ERR_MSG("Memory allocation failed.");
        free(xpath);
        return SR_ERR_NOMEM;
    }
    val->xpath = xpath;

    switch (node->schema->nodetype) {
    case LYS_LEAF:
        data_leaf = (struct lyd_node_leaf_list *) node;

        val->type = sr_libyang_type_to_sysrepo(data_leaf->value_type);

        if (SR_ERR_OK != rp_dt_copy_value(data_leaf, data_leaf->value_type, val)) {
            SR_LOG_ERR_MSG("Copying of value failed");
            free(val->xpath);
            free(val);
            return SR_ERR_INTERNAL;
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

        if (SR_ERR_OK != rp_dt_copy_value(data_leaf, data_leaf->value_type, val)) {
            SR_LOG_ERR_MSG("Copying of value failed");
            free(val->xpath);
            free(val);
            return SR_ERR_INTERNAL;
        }
        break;
    default:
        SR_LOG_WRN_MSG("Get value is not implemented for this node type");
        free(val->xpath);
        free(val);
        return SR_ERR_INTERNAL;
    }
    *value = val;
    return SR_ERR_OK;
}

/**
 * Fills the values from the array of nodes
 */
static int
rp_dt_get_values_from_nodes(struct lyd_node **nodes, size_t count, sr_val_t ***values)
{
    CHECK_NULL_ARG2(nodes, values);
    int rc = SR_ERR_OK;
    sr_val_t **vals = NULL;
    vals = calloc(count, sizeof(*vals));
    if (NULL == vals) {
        SR_LOG_ERR_MSG("Memory allocation failed");
        return SR_ERR_NOMEM;
    }

    for (size_t i = 0; i < count; i++) {
        rc = rp_dt_get_value_from_node(nodes[i], &vals[i]);
        if (SR_ERR_OK != rc) {
            const char *name = "";
            if (NULL != nodes[i] && NULL != nodes[i]->schema && NULL != nodes[i]->schema->name) {
                name = nodes[i]->schema->name;
            }
            SR_LOG_ERR("Getting value from node %s failed", name);
            for (size_t j = 0; j < i; j++) {
                sr_free_val(vals[j]);
            }
            free(vals);
            return SR_ERR_INTERNAL;
        }
    }
    *values = vals;

    return rc;
}

int
rp_dt_get_value(const dm_ctx_t *dm_ctx, struct lyd_node *data_tree, const xp_loc_id_t *loc_id, sr_val_t **value)
{
    CHECK_NULL_ARG4(dm_ctx, data_tree, loc_id, value);
    int rc = 0;
    struct lyd_node *node = NULL;
    rc = rp_dt_get_node(dm_ctx, data_tree, loc_id, &node);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Node not found for xpath %s", loc_id->xpath);
        return rc;
    }
    rc = rp_dt_get_value_from_node(node, value);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get value from node failed for xpath %s", loc_id->xpath);
    }
    return rc;
}

int
rp_dt_get_values(const dm_ctx_t *dm_ctx, struct lyd_node *data_tree, const xp_loc_id_t *loc_id, sr_val_t ***values, size_t *count)
{
    CHECK_NULL_ARG5(dm_ctx, data_tree, loc_id, values, count);

    int rc = SR_ERR_OK;
    struct lyd_node **nodes = NULL;
    rc = rp_dt_get_nodes(dm_ctx, data_tree, loc_id, &nodes, count);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get nodes for xpath %s failed", loc_id->xpath);
        return rc;
    }

    rc = rp_dt_get_values_from_nodes(nodes, *count, values);
    free(nodes);

    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying values from nodes failed for xpath '%s'", loc_id->xpath);
        return rc;
    }

    return SR_ERR_OK;
}

int
rp_dt_get_value_wrapper(dm_ctx_t *dm_ctx, dm_session_t *dm_session, const char *xpath, sr_val_t **value)
{
    CHECK_NULL_ARG4(dm_ctx, dm_session, xpath, value);

    int rc = SR_ERR_INVAL_ARG;
    xp_loc_id_t *l = NULL;
    struct lyd_node *data_tree = NULL;
    char *data_tree_name = NULL;
    rc = xp_char_to_loc_id(xpath, &l);

    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Converting xpath '%s' to loc_id failed.", xpath);
        return rc;
    }

    if (!XP_HAS_NODE_NS(l, 0)) {
        SR_LOG_ERR("Provided xpath '%s' doesn't contain namespace on the root node", xpath);
        goto cleanup;
    }

    data_tree_name = XP_CPY_NODE_NS(l, 0);
    if (NULL == data_tree_name) {
        SR_LOG_ERR("Copying module name failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = dm_get_datatree(dm_ctx, dm_session, data_tree_name, &data_tree);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Getting data tree failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = rp_dt_get_value(dm_ctx, data_tree, l, value);
    if (SR_ERR_NOT_FOUND == rc) {
        rc = rp_dt_validate_node_xpath(dm_ctx, l, NULL, NULL);
        rc = rc == SR_ERR_OK ? SR_ERR_NOT_FOUND : rc;
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get value failed for xpath '%s'", xpath);
    }

cleanup:
    xp_free_loc_id(l);
    free(data_tree_name);
    return rc;
}

int
rp_dt_get_values_wrapper(dm_ctx_t *dm_ctx, dm_session_t *dm_session, const char *xpath, sr_val_t ***values, size_t *count)
{
    CHECK_NULL_ARG5(dm_ctx, dm_session, xpath, values, count);

    int rc = SR_ERR_INVAL_ARG;
    xp_loc_id_t *l = NULL;
    struct lyd_node *data_tree = NULL;
    char *data_tree_name = NULL;
    rc = xp_char_to_loc_id(xpath, &l);

    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Converting xpath '%s' to loc_id failed.", xpath);
        return rc;
    }

    if (!XP_HAS_NODE_NS(l, 0)) {
        SR_LOG_ERR("Provided xpath '%s' doesn't containt namespace on the root node", xpath);
        goto cleanup;
    }

    data_tree_name = XP_CPY_NODE_NS(l, 0);
    if (NULL == data_tree_name) {
        SR_LOG_ERR("Copying module name failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = dm_get_datatree(dm_ctx, dm_session, data_tree_name, &data_tree);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Getting data tree failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = rp_dt_get_values(dm_ctx, data_tree, l, values, count);
    if (SR_ERR_NOT_FOUND == rc) {
        rc = rp_dt_validate_node_xpath(dm_ctx, l, NULL, NULL);
        rc = rc == SR_ERR_OK ? SR_ERR_NOT_FOUND : rc;
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get values failed for xpath '%s'", xpath);
    }

cleanup:
    xp_free_loc_id(l);
    free(data_tree_name);
    return rc;
}

int
rp_dt_get_values_wrapper_with_opts(dm_ctx_t *dm_ctx, dm_session_t *dm_session, rp_dt_get_items_ctx_t *get_items_ctx, const char *xpath,
        bool recursive, size_t offset, size_t limit, sr_val_t ***values, size_t *count)
{

    int rc = SR_ERR_INVAL_ARG;
    xp_loc_id_t *l = NULL;
    struct lyd_node *data_tree = NULL;
    struct lyd_node **nodes = NULL;
    char *data_tree_name = NULL;
    rc = xp_char_to_loc_id(xpath, &l);

    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Converting xpath '%s' to loc_id failed.", xpath);
        return rc;
    }

    if (!XP_HAS_NODE_NS(l, 0)) {
        SR_LOG_ERR("Provided xpath's root doesn't contain a namespace '%s' ", xpath);
        goto cleanup;
    }

    data_tree_name = XP_CPY_NODE_NS(l, 0);
    if (NULL == data_tree_name) {
        SR_LOG_ERR("Copying module name failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = dm_get_datatree(dm_ctx, dm_session, data_tree_name, &data_tree);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Getting data tree failed for xpath '%s'", xpath);
        goto cleanup;
    }

    rc = rp_dt_get_nodes_with_opts(dm_ctx, dm_session, get_items_ctx, data_tree, l, recursive, offset, limit, &nodes, count);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get nodes for xpath %s failed", l->xpath);
        goto cleanup;
    }

    rc = rp_dt_get_values_from_nodes(nodes, *count, values);
    if (SR_ERR_NOT_FOUND == rc) {
        rc = rp_dt_validate_node_xpath(dm_ctx, l, NULL, NULL);
        rc = rc == SR_ERR_OK ? SR_ERR_NOT_FOUND : rc;
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Copying values from nodes failed for xpath '%s'", l->xpath);
        goto cleanup;
    }

cleanup:
    free(nodes);
    xp_free_loc_id(l);
    free(data_tree_name);
    return rc;

}
