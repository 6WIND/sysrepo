/**
 * @defgroup rp_get Request processor data tree helpers for get functionality 
 * @{
 * @brief 
 * @file rp_dt_get.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 *
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

#ifndef RP_DT_GET_H
#define RP_DT_GET_H

#include "xpath_processor.h"
#include "rp_dt_lookup.h"

/**
 * @brief Retrieves all nodes corresponding to location_id using ::rp_dt_get_nodes and copy all values
 * using ::rp_dt_get_values_from_nodes.
 * @param [in] dm_ctx
 * @param [in] data_tree
 * @param [in] loc_id
 * @param [in] check_enable
 * @param [out] values
 * @param [out] count
 * @return Error code (SR_ERR_OK on success), SR_ERR_NOT_FOUND
 */
int rp_dt_get_values(const dm_ctx_t *dm_ctx, struct lyd_node *data_tree, const xp_loc_id_t *loc_id, bool check_enable, sr_val_t ***values, size_t *count);

/**
 * @brief Returns the value for the specified location_id for leaf, container and list.
 * If the provided location id identifies the whole moduel SR_ERR_INVAL_ARG is returned.
 * @param [in] dm_ctx
 * @param [in] data_tree
 * @param [in] loc_id
 * @param [in] check_enable
 * @param [out] value
 * @return Error code (SR_ERR_OK on success)
 */
int rp_dt_get_value(const dm_ctx_t *dm_ctx, struct lyd_node *data_tree, const xp_loc_id_t *loc_id, bool checke_enable, sr_val_t **value);

/**
 * @brief Returns the value for the specified xpath. Internally converts xpath to location_id and call ::rp_dt_get_value.
 * The xpath is validated.
 * @param [in] dm_ctx
 * @param [in] dm_session
 * @param [in] xpath
 * @param [out] value
 * @return Error code (SR_ERR_OK on success), SR_ERR_NOT_FOUND, SR_ERR_UNKNOWN_MODEL, SR_ERR_BAD_ELEMENT
 */
int rp_dt_get_value_wrapper(dm_ctx_t *dm_ctx, dm_session_t *dm_session, const char *xpath, sr_val_t **value);

/**
 * @brief Returns the values for the specified xpath. Internally converts xpath to location_id and 
 * calls ::rp_dt_get_values.
 * @param [in] dm_ctx
 * @param [in] dm_session
 * @param [in] xpath
 * @param [out] values
 * @param [out] count
 * @return Error code (SR_ERR_OK on success), SR_ERR_NOT_FOUND, SR_ERR_UNKNOWN_MODEL, SR_ERR_BAD_ELEMENT
 */
int rp_dt_get_values_wrapper(dm_ctx_t *dm_ctx, dm_session_t *dm_session, const char *xpath, sr_val_t ***values, size_t *count);

/**
 * @brief Returns the values for the specified xpath. Internally converts xpath to location_id and calls ::rp_dt_get_nodes_with_opts 
 * and ::rp_dt_get_values_from_nodes. The selection of returned valued can be specified by recursive, limit and offset.
 * @param [in] dm_ctx
 * @param [in] dm_session
 * @param [in] get_items_ctx
 * @param [in] xpath
 * @param [in] recursive - include all nodes under the selected xpath
 * @param [in] offset - return the values with index and above
 * @param [in] limit - the maximum count of values that can be returned
 * @param [out] values
 * @param [out] count
 */
int rp_dt_get_values_wrapper_with_opts(dm_ctx_t *dm_ctx, dm_session_t *dm_session, rp_dt_get_items_ctx_t *get_items_ctx, const char *xpath,
                                       bool recursive, size_t offset, size_t limit, sr_val_t ***values, size_t *count);

#endif /* RP_DT_GET_H */

/**
 * @}
 */
