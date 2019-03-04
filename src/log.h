/**
 * @file log.h
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief header for logging routines
 *
 * @copyright
 * Copyright (c) 2019 CESNET, z.s.p.o.
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
#ifndef _LOG_H
#define _LOG_H

#include "common.h"

#define SR_ERRINFO_INT(err_info) sr_errinfo_new(err_info, SR_ERR_INTERNAL, NULL, "Internal error (%s:%d).", __FILE__, __LINE__)
#define SR_ERRINFO_MEM(err_info) sr_errinfo_new(err_info, SR_ERR_NOMEM, NULL, NULL)
#define SR_ERRINFO_RWLOCK(err_info, wr, func, ret) sr_errinfo_new(err_info, (ret == ETIMEDOUT) ? SR_ERR_TIME_OUT : SR_ERR_INTERNAL, \
        NULL, "%s locking a rwlock failed (%s: %s).", wr ? "Write" : "Read", func, strerror(ret))
#define SR_ERRINFO_LOCK(err_info, func, ret) sr_errinfo_new(err_info, (ret == ETIMEDOUT) ? SR_ERR_TIME_OUT : SR_ERR_INTERNAL, \
        NULL, "Locking a mutex failed (%s: %s).", func, strerror(ret))
#define SR_ERRINFO_COND(err_info, func, ret) sr_errinfo_new(err_info, (ret == ETIMEDOUT) ? SR_ERR_TIME_OUT : SR_ERR_INTERNAL, \
        NULL, "Waiting on a conditional variable failed (%s: %s).", func, strerror(ret))
#define SR_ERRINFO_SYSERRNO(err_info, func) sr_errinfo_new(err_info, SR_ERR_SYS, NULL, "%s() failed (%s).", func, strerror(errno))
#define SR_ERRINFO_VALID(err_info) sr_errinfo_new(err_info, SR_ERR_VALIDATION_FAILED, NULL, "Validation failed.")

#define SR_LOG_WRN(format, ...) sr_log(SR_LL_WRN, format, ##__VA_ARGS__)
#define SR_LOG_INF(format, ...) sr_log(SR_LL_INF, format, ##__VA_ARGS__)
#define SR_LOG_DBG(format, ...) sr_log(SR_LL_DBG, format, ##__VA_ARGS__)

#define SR_LOG_WRNMSG(format) sr_log(SR_LL_WRN, format)
#define SR_LOG_INFMSG(format) sr_log(SR_LL_INF, format)
#define SR_LOG_DBGMSG(format) sr_log(SR_LL_DBG, format)

#define SR_CHECK_MEM_GOTO(cond, err_info, go) if (cond) { SR_ERRINFO_MEM(&(err_info)); goto go; }
#define SR_CHECK_MEM_RET(cond, err_info) if (cond) { SR_ERRINFO_MEM(&(err_info)); return err_info; }
#define SR_CHECK_INT_GOTO(cond, err_info, go) if (cond) { SR_ERRINFO_INT(&(err_info)); goto go; }
#define SR_CHECK_INT_RET(cond, err_info) if (cond) { SR_ERRINFO_INT(&(err_info)); return err_info; }

#define SR_CHECK_ARG_APIRET(cond, session, err_info) if (cond) { sr_errinfo_new(&(err_info), SR_ERR_INVAL_ARG, NULL, \
        "Invalid arguments for function \"%s\".", __func__); return sr_api_ret(session, err_info); }

sr_error_t sr_api_ret(sr_session_ctx_t *session, sr_error_info_t *err_info);

void sr_log_msg(sr_log_level_t ll, const char *msg, const char *path);

void sr_errinfo_new(sr_error_info_t **err_info, sr_error_t err_code, const char *xpath, const char *format, ...);

void sr_errinfo_new_ly(sr_error_info_t **err_info, struct ly_ctx *ly_ctx);

void sr_log_wrn_ly(struct ly_ctx *ly_ctx);

void sr_errinfo_free(sr_error_info_t **err_info);

void sr_errinfo_merge(sr_error_info_t **err_info, sr_error_info_t *err_info2);

void sr_log(sr_log_level_t ll, const char *format, ...);

#endif