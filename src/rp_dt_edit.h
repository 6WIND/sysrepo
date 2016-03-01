/**
 * @defgroup rp_edit Request processor's data tree edit helpers
 * @{
 * @brief Function that can create, modify delete nodes or move lists.
 * @file rp_dt_edit.h
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

#ifndef RP_DT_EDIT_H
#define RP_DT_EDIT_H

#include "request_processor.h"
#include "data_manager.h"

/**
 * @brief Validates the xpath and then deletes item(s) identified by xpath.
 * List key can not be deleted. (if attempted SR_ERR_INVAL_ARG is returned)
 * @param [in] dm_ctx
 * @param [in] session
 * @param [in] loc_id
 * @param [in] options If the nodes can not be delete because of the option SR_ERR_DATA_MISSING or SR_ERR_DATA_EXISTS is returned
 * @return Error code (SR_ERR_OK on success) SR_ERR_DATA_MISSING, SR_ERR_DATA_EXISTS, SR_ERR_UNKNOWN_MODEL, SR_ERR_BAD_ELEMENT
 */
int rp_dt_delete_item(dm_ctx_t *dm_ctx, dm_session_t *session, const xp_loc_id_t *loc_id, const sr_edit_flag_t options);

/**
 * @brief Function validates the xpath and then creates presence container, list instance, leaf, leaf-list item. If the xpath identifies leaf-list value is appended to the end
 * of the leaf-list. Value of the list key can not be set or changed. To create a list use
 * xpath including all list keys.
 * @param [in] dm_ctx
 * @param [in] session
 * @param [in] xpath
 * @param [in] options If the node can not be created because of the option SR_ERR_DATA_EXISTS or SR_ERR_DATA_MISSING is returned
 * @param [in] value the value to be set (xpath inside the structure is ignored), in case of presence container or list instance is ignored can be NULL
 * @return Error code (SR_ERR_OK on success) SR_ERR_DATA_MISSING, SR_ERR_DATA_EXISTS, SR_ERR_UNKNOWN_MODEL, SR_ERR_BAD_ELEMENT
 */
int rp_dt_set_item(dm_ctx_t *dm_ctx, dm_session_t *session, const xp_loc_id_t *loc_id, const sr_edit_flag_t options, const sr_val_t *value);

/**
 * @brief Move the list instance into selected direction. If the list instance doesn't exists or the list is not user-ordered SR_ERR_INVAL_ARG is returned.
 * If the list is at left-most/right-most position move UP/DOWN does nothing.
 * @param [in] dm_ctx
 * @param [in] session
 * @param [in] xpath
 * @param [in] direction
 * @return Error code (SR_ERR_OK on success) SR_ERR_UNKNOWN_MODEL, SR_ERR_BAD_ELEMENT
 */
int rp_dt_move_list(dm_ctx_t *dm_ctx, dm_session_t *session, const xp_loc_id_t *loc_id, sr_move_direction_t direction);

/**
 * @brief Wraps ::rp_dt_move_list call, in case of success logs the operation to the session's operation list.
 * @param [in] rp_ctx
 * @param [in] session
 * @param [in] xpath
 * @param [in] direction
 * @return Error code (SR_ERR_OK on success)
 */
int rp_dt_move_list_wrapper(rp_ctx_t *rp_ctx, rp_session_t *session, const char *xpath, sr_move_direction_t direction);

/**
 * @brief Wraps ::rp_dt_set_item call. In case of success logs the operation to the session's operation list.
 * @param [in] rp_ctx
 * @param [in] session
 * @param [in] xpath
 * @param [in] val
 * @param [in] opt
 * @return Error code (SR_ERR_OK on success)
 */
int rp_dt_set_item_wrapper(rp_ctx_t *rp_ctx, rp_session_t *session, const char *xpath, sr_val_t *val, sr_edit_options_t opt);

/**
 * @brief Wraps ::rp_dt_delete_item call. In in case of success logs the operation to the session's operation list.
 * @param [in] rp_ctx
 * @param [in] session
 * @param [in] xpath
 * @param [in] opts
 * @return Error code (SR_ERR_OK on success)
 */
int rp_dt_delete_item_wrapper(rp_ctx_t *rp_ctx, rp_session_t *session, const char *xpath, sr_edit_options_t opts);

/**
 * @brief Perform the list of provided operations on the session. Stops
 * on the first error.
 * @param [in] ctx
 * @param [in] session
 * @param [in] operations
 * @param [in] count
 * @param [in] matched_ts - set of model's name where the current modify timestamp
 * matches the timestamp of the session copy. Operation for this models skipped.
 * @return Error code (SR_ERR_OK on success)
 */
int rp_dt_replay_operations(dm_ctx_t *ctx, dm_session_t *session, dm_sess_op_t *operations, size_t count
#ifdef HAVE_STAT_ST_MTIM
, struct ly_set *matched_ts
#endif
);

#endif /* RP_DT_EDIT_H */

/**
 * @}
 */
