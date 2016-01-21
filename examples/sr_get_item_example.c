/**
 * @file sr_get_item_example.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Example usage of sr_get_item function
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
#include "sysrepo.h"

int
main(int argc, char **argv)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_val_t *value = NULL;
    int rc = SR_ERR_OK;
    
    /* connect to sysrepo */
    rc = sr_connect("app1", true, &conn);
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }

    /* turn on debug logging on stderr */
    sr_logger_set_level(SR_LL_DBG, SR_LL_INF);

    /* start session */
    rc = sr_session_start(conn, NULL, SR_DS_CANDIDATE, &sess);
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }

    /* read one value*/
    rc = sr_get_item(sess, "/ietf-interfaces:interfaces/interface[name='eth0']/enabled", &value);
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }
    printf("\nValue on xpath: %s = %s\n", value->xpath, (value->data.bool_val ? "true" : "false"));
    sr_free_val_t(value);

cleanup:
    if (NULL != sess) {
        sr_session_stop(sess);
    }
    if (NULL != conn) {
        sr_disconnect(conn);
    }
    return rc;
}
