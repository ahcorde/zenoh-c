//
// Copyright (c) 2023 ZettaScale Technology
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
// which is available at https://www.apache.org/licenses/LICENSE-2.0.
//
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
//
// Contributors:
//   ZettaScale Zenoh Team, <zenoh@zettascale.tech>
//

#include "z_int_helpers.h"

#ifdef VALID_PLATFORM

#include "zenoh.h"

const char *const SEM_NAME = "/z_int_test_queryable_sync_sem";
sem_t *sem;

const char *const keyexpr = "test/key";
const char *const values[] = {"test_value_1", "test_value_2", "test_value_3"};
const size_t values_count = sizeof(values) / sizeof(values[0]);

const char *const K_VAR = "k_var";
const char *const K_CONST = "k_const";
const char *const V_CONST = "v const";

typedef struct attachement_context_t {
    const char* keys[2];
    const char* values[2];
    size_t num_items;
    size_t iteration_index;
} attachement_context_t;

void create_attachement_it(z_owned_bytes_t* kv_pair, void* context) {
    attachement_context_t *ctx = (attachement_context_t*)(context);
    z_owned_bytes_t k, v;
    if (ctx->iteration_index >= ctx->num_items) {
        z_null(kv_pair);
        return;
    } else {
        z_bytes_encode_from_string(&k, ctx->keys[ctx->iteration_index]);
        z_bytes_encode_from_string(&v, ctx->values[ctx->iteration_index]);
    }

    z_bytes_encode_pair(kv_pair, z_move(k), z_move(v));
    ctx->iteration_index++;
};

z_error_t check_attachement_it(const z_loaned_bytes_t* kv_pair, void* context) {
    attachement_context_t *ctx = (attachement_context_t*)(context);
    if (ctx->iteration_index >= ctx->num_items) {
        perror("Attachment contains more items than expected\n");
        return -1;
    }

    z_owned_bytes_t k, v;
    z_owned_str_t k_str, v_str;
    z_bytes_decode_into_pair(kv_pair, &k, &v);

    z_bytes_decode_into_string(z_loan(k), &k_str);
    z_bytes_decode_into_string(z_loan(v), &v_str);

    if (strncmp(ctx->keys[ctx->iteration_index], z_str_data(z_loan(k_str)), z_str_len(z_loan(k_str))) != 0) {
        perror("Incorrect attachment key\n");
        return -1;
    }
    if (strncmp(ctx->values[ctx->iteration_index], z_str_data(z_loan(v_str)), z_str_len(z_loan(v_str))) != 0) {
        perror("Incorrect attachment value\n");
        return -1;
    }

    z_drop(&k_str);
    z_drop(&v_str);
    z_drop(&k);
    z_drop(&v);
    ctx->iteration_index++;
    return 0;
};


void query_handler(const z_loaned_query_t *query, void *context) {
    static int value_num = 0;

    z_view_str_t params;
    z_query_parameters(query, &params);
    const z_loaned_value_t* payload_value = z_query_value(query);

    const z_loaned_bytes_t* attachment = z_query_attachment(query);
    if (attachment == NULL) {
        perror("Missing attachment!");
        exit(-1);
    }

    attachement_context_t in_attachment_context = (attachement_context_t){
        .keys = {K_CONST, K_VAR}, .values = {V_CONST, values[value_num]}, .num_items = 2, .iteration_index = 0 
    };
    z_error_t res = z_bytes_decode_into_iter(attachment, check_attachement_it, (void*)&in_attachment_context);
    if (in_attachment_context.iteration_index != in_attachment_context.num_items || res != 0) {
        perror("Failed to validate attachment");
        exit(-1);
    }

    z_query_reply_options_t options;
    z_query_reply_options_default(&options);
   
    z_owned_bytes_t reply_attachment;
    attachement_context_t out_attachment_context = (attachement_context_t){
        .keys = {K_CONST}, .values = {V_CONST}, .num_items = 1, .iteration_index = 0 
    };
    z_bytes_encode_from_iter(&reply_attachment, create_attachement_it, (void*)&out_attachment_context);

    options.attachment = &reply_attachment;

    z_owned_bytes_t payload;
    z_bytes_encode_from_string(&payload, values[value_num]);

    z_view_keyexpr_t reply_ke;
    z_view_keyexpr_from_string(&reply_ke, (const char *)context);
    z_query_reply(query, z_loan(reply_ke), z_move(payload), &options);

    if (++value_num == values_count) {
        exit(0);
    }
}

int run_queryable() {
    z_owned_config_t config;
    z_config_default(&config);
    z_owned_session_t s;
    if (z_open(&s, z_move(config)) < 0) {
        perror("Unable to open session!");
        return -1;
    }

    z_owned_closure_query_t callback;
    z_closure(&callback, query_handler, NULL, keyexpr);

    z_view_keyexpr_t ke;
    z_view_keyexpr_from_string(&ke, keyexpr);

    z_owned_queryable_t qable;
    if (z_declare_queryable(&qable, z_loan(s), z_loan(ke), z_move(callback), NULL) < 0) {
        printf("Unable to create queryable.\n");
        return -1;
    }

    SEM_POST(sem);
    z_sleep_s(10);

    z_undeclare_queryable(z_move(qable));
    z_close(z_move(s));
    return 0;
}

int run_get() {
    SEM_WAIT(sem);

    z_owned_config_t config;
    z_config_default(&config);
    z_owned_session_t s;
    if (z_open(&s, z_move(config)) < 0) {
        perror("Unable to open session!");
        return -1;
    }

    z_view_keyexpr_t ke;
    z_view_keyexpr_from_string(&ke, keyexpr);

    z_get_options_t opts;
    z_get_options_default(&opts);


    for (int val_num = 0; val_num < values_count; ++val_num) {
        attachement_context_t out_attachment_context = (attachement_context_t){
            .keys = {K_CONST, K_VAR}, .values = {V_CONST, values[val_num]}, .num_items = 2, .iteration_index = 0 
        };

        z_owned_reply_channel_t channel;
        zc_reply_fifo_new(&channel, 16);

        z_owned_bytes_t attachment;
        z_bytes_encode_from_iter(&attachment, create_attachement_it, (void*)&out_attachment_context);

        opts.attachment = &attachment;
        z_get(z_loan(s), z_loan(ke), "", z_move(channel.send), &opts);
        z_owned_reply_t reply;
        for (z_call(z_loan(channel.recv), &reply); z_check(reply); z_call(z_loan(channel.recv), &reply)) {
            assert(z_reply_is_ok(z_loan(reply)));

            const z_loaned_sample_t* sample = z_reply_ok(z_loan(reply));
            z_owned_str_t payload_str;
            z_bytes_decode_into_string(z_sample_payload(sample), &payload_str);
            if (strncmp(values[val_num], z_str_data(z_loan(payload_str)), z_str_len(z_loan(payload_str)))) {
                perror("Unexpected value received");
                z_drop(z_move(payload_str));
                exit(-1);
            }

            const z_loaned_bytes_t* received_attachment = z_sample_attachment(sample);
            if (received_attachment == NULL) {
                perror("Missing attachment!");
                exit(-1);
            }
            attachement_context_t in_attachment_context = (attachement_context_t){
                .keys = {K_CONST}, .values = {V_CONST}, .num_items = 1, .iteration_index = 0 
            };
            z_error_t res = z_bytes_decode_into_iter(received_attachment, check_attachement_it, (void*)&in_attachment_context);
            if (in_attachment_context.iteration_index != in_attachment_context.num_items || res != 0) {
                perror("Failed to validate attachment");
                exit(-1);
            }

            z_drop(z_move(payload_str));
        }
        z_drop(z_move(reply));
        z_drop(z_move(channel));
    }
    z_close(z_move(s));

    return 0;
}

int main() {
    SEM_INIT(sem, SEM_NAME);

    func_ptr_t funcs[] = {run_queryable, run_get};
    assert(run_timeouted_test(funcs, 2, 10) == 0);

    SEM_DROP(sem, SEM_NAME);

    return 0;
}

#else
int main() { return 0; }
#endif  // VALID_PLATFORM
