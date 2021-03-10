/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdlib.h>

#include "files.h"
#include "log.h"
#include "tpm2.h"
#include "tpm2_tool.h"
#include "tpm2_nv_util.h"
#include "tpm2_options.h"


typedef struct tpm_nvextend_ctx tpm_nvextend_ctx;
/*
 * Since the first session is the authorization session for NVIndex, it is
 * provided by the auth interface.
 */
#define MAX_AUX_SESSIONS 2
struct tpm_nvextend_ctx {
    struct {
        const char *ctx_path;
        const char *auth_str;
        tpm2_loaded_object object;
    } auth_hierarchy;

    const char *input_path;
    TPM2_HANDLE nv_index;

    char *cp_hash_path;

    uint8_t aux_session_cnt;
    tpm2_session *aux_session[MAX_AUX_SESSIONS];
    const char *aux_session_path[MAX_AUX_SESSIONS];
};

static tpm_nvextend_ctx ctx;

static bool on_arg(int argc, char **argv) {
    /* If the user doesn't specify an authorization hierarchy use the index
     * passed to -x/--index for the authorization index.
     */
    if (!ctx.auth_hierarchy.ctx_path) {
        ctx.auth_hierarchy.ctx_path = argv[0];
    }
    return on_arg_nv_index(argc, argv, &ctx.nv_index);
}

static bool on_option(char key, char *value) {

    switch (key) {

    case 'C':
        ctx.auth_hierarchy.ctx_path = value;
        break;
    case 'P':
        ctx.auth_hierarchy.auth_str = value;
        break;
    case 'i':
        ctx.input_path = value;
        break;
    case 0:
        ctx.cp_hash_path = value;
        break;
    case 'S':
        ctx.aux_session_path[ctx.aux_session_cnt] = value;
        if (ctx.aux_session_cnt < MAX_AUX_SESSIONS) {
            ctx.aux_session_cnt++;
        } else {
            return false;
        }
        break;
    }

    return true;
}

static bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] = {
        { "hierarchy", required_argument, NULL, 'C' },
        { "auth",      required_argument, NULL, 'P' },
        { "input",     required_argument, NULL, 'i' },
        { "cphash",    required_argument, NULL,  0  },
        { "session",   required_argument, NULL, 'S' },
    };

    *opts = tpm2_options_new("S:C:P:i:", ARRAY_LEN(topts), topts, on_option,
            on_arg, 0);

    return *opts != NULL;
}

static tool_rc tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {

    UNUSED(flags);

    ctx.input_path = strcmp(ctx.input_path, "-") ? ctx.input_path : NULL;

    TPM2B_MAX_NV_BUFFER data = { .size = sizeof(data.buffer) };

    bool result = files_load_bytes_from_buffer_or_file_or_stdin(NULL,
            ctx.input_path, &data.size, data.buffer);
    if (!result) {
        return tool_rc_general_error;
    }

    tool_rc rc = tpm2_util_object_load_auth(ectx, ctx.auth_hierarchy.ctx_path,
            ctx.auth_hierarchy.auth_str, &ctx.auth_hierarchy.object, false,
            TPM2_HANDLE_FLAGS_NV | TPM2_HANDLE_FLAGS_O | TPM2_HANDLE_FLAGS_P);
    if (rc != tool_rc_success) {
        LOG_ERR("Invalid handle authorization");
        return rc;
    }

    if (!ctx.cp_hash_path) {
        ESYS_TR aux_session_handle[MAX_AUX_SESSIONS] = {ESYS_TR_NONE, ESYS_TR_NONE};
        tool_rc tmp_rc = tpm2_util_aux_sessions_setup(ectx, ctx.aux_session_cnt,
            ctx.aux_session_path, aux_session_handle, ctx.aux_session);
        if (tmp_rc != tool_rc_success) {
            return tmp_rc;
        }
        return tpm2_nvextend(ectx, &ctx.auth_hierarchy.object, ctx.nv_index,
            &data, NULL, aux_session_handle[0], aux_session_handle[1]);
    }

    TPM2B_DIGEST cp_hash = { .size = 0 };
    rc = tpm2_nvextend(ectx, &ctx.auth_hierarchy.object, ctx.nv_index, &data,
        &cp_hash, ESYS_TR_NONE, ESYS_TR_NONE);
    if (rc != tool_rc_success) {
        return rc;
    }

    result = files_save_digest(&cp_hash, ctx.cp_hash_path);
    if (!result) {
        rc = tool_rc_general_error;
    }

    return rc;
}

static tool_rc tpm2_tool_onstop(ESYS_CONTEXT *ectx) {

    UNUSED(ectx);

    tool_rc rc = tool_rc_success;
    if (!ctx.cp_hash_path) {
        rc = tpm2_session_close(&ctx.auth_hierarchy.object.session);
    }

    uint8_t session_idx = 0;
    tool_rc tmp_rc = tool_rc_success;
    for(session_idx = 0; session_idx < ctx.aux_session_cnt; session_idx++) {
        if (ctx.aux_session_path[session_idx]) {
            tmp_rc = tpm2_session_close(&ctx.aux_session[session_idx]);
        }
    }

    return rc != tool_rc_success ? rc :
           tmp_rc != tool_rc_success ? tmp_rc :
           tool_rc_success;
}

// Register this tool with tpm2_tool.c
TPM2_TOOL_REGISTER("nvextend", tpm2_tool_onstart, tpm2_tool_onrun, tpm2_tool_onstop, NULL)
