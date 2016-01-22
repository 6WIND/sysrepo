/**
 * @file dm_test.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Data Manager unit tests.
 *
 * @copyright
 * Copyright 2015 Cisco Systems, Inc.
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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <stdio.h>
#include "data_manager.h"
#include "test_data.h"
#include "sr_common.h"

void dm_create_cleanup(void **state){
   int rc;
   dm_ctx_t *ctx;
   rc = dm_init(TEST_DATA_DIR, &ctx);
   assert_int_equal(SR_ERR_OK,rc);

   dm_cleanup(ctx);

}

void dm_get_data_tree(void **state)
{
    int rc;
    dm_ctx_t *ctx;
    dm_session_t *ses_ctx;
    struct lyd_node *data_tree;

    rc = dm_init(TEST_DATA_DIR, &ctx);
    assert_int_equal(SR_ERR_OK, rc);

    dm_session_start(ctx, &ses_ctx);
    /* Load from file */
    assert_int_equal(SR_ERR_OK, dm_get_datatree(ctx, ses_ctx ,"example-module", &data_tree));
    /* Get from avl tree */
    assert_int_equal(SR_ERR_OK, dm_get_datatree(ctx, ses_ctx ,"example-module", &data_tree));
    /* Module without data*/
    assert_int_equal(SR_ERR_NOT_FOUND, dm_get_datatree(ctx, ses_ctx ,"small-module", &data_tree));
    /* Not existing module should return an error*/
    assert_int_equal(SR_ERR_UNKNOWN_MODEL, dm_get_datatree(ctx, ses_ctx ,"not-existing-module", &data_tree));

    dm_session_stop(ctx, ses_ctx);

    dm_cleanup(ctx);

}

int main(){

    const struct CMUnitTest tests[] = {
            cmocka_unit_test(dm_create_cleanup),
            cmocka_unit_test(dm_get_data_tree),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}


