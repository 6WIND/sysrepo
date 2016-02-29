/**
 * @file rp_dt_context_helper.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief RP datatree context helper functions API.
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

#ifndef RP_DT_CONTEXT_HELPER_H_
#define RP_DT_CONTEXT_HELPER_H_

/**
 * @brief
 */
void test_rp_ctx_create(rp_ctx_t **rp_ctx_p);

/**
 * @brief
 */
void test_rp_ctx_cleanup(rp_ctx_t *ctx);

/**
 * @brief
 */
void test_rp_sesssion_create(rp_ctx_t *rp_ctx, sr_datastore_t datastore, rp_session_t **rp_session_p);

/**
 * @brief
 */
void test_rp_session_cleanup(rp_ctx_t *ctx, rp_session_t *session);

#endif /* RP_DT_CONTEXT_HELPER_H_ */
