/*
 * shadPS4 SPIR-V to DXIL bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i386/shadps4-gpu-dxil.h"

#ifdef CONFIG_SHADPS4_SPIRV_TO_DXIL
#include "hw/i386/shadps4-spirv-to-dxil.h"

static void shadps4_dxil_log(void *opaque, const char *message)
{
    GString *log = opaque;

    if (!message || !*message) {
        return;
    }
    if (log->len) {
        g_string_append_c(log, '\n');
    }
    g_string_append(log, message);
}
#endif

bool shadps4_dxil_available(uint64_t *version)
{
#ifdef CONFIG_SHADPS4_SPIRV_TO_DXIL
    uint64_t detected = spirv_to_dxil_get_version();

    if (version) {
        *version = detected;
    }
    return true;
#else
    if (version) {
        *version = 0;
    }
    return false;
#endif
}

bool shadps4_dxil_compile(const uint32_t *spirv, size_t word_count,
                          ShadPS4DxilStage stage,
                          ShadPS4DxilBinary *binary, char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!binary) {
        return false;
    }
    shadps4_dxil_binary_clear(binary);
    if (!spirv || word_count < 5 || spirv[0] != UINT32_C(0x07230203) ||
        stage < SHADPS4_DXIL_STAGE_VERTEX ||
        stage > SHADPS4_DXIL_STAGE_COMPUTE) {
        if (error) {
            *error = g_strdup("invalid SPIR-V module or shader stage");
        }
        return false;
    }

#ifdef CONFIG_SHADPS4_SPIRV_TO_DXIL
    struct dxil_spirv_runtime_conf conf = {
        .runtime_data_cbv = {
            .register_space = 31,
            .base_shader_register = 0,
        },
        .push_constant_cbv = {
            .register_space = 30,
            .base_shader_register = 0,
        },
        .first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO,
        .workgroup_id_mode = DXIL_SPIRV_SYSVAL_TYPE_NATIVE,
        .declared_read_only_images_as_srvs = true,
        .inferred_read_only_images_as_srvs = true,
        .shader_model_max = SHADER_MODEL_6_2,
    };
    struct dxil_spirv_debug_options debug = { 0 };
    struct dxil_spirv_object object = { 0 };
    GString *messages = g_string_new(NULL);
    struct dxil_spirv_logger logger = {
        .priv = messages,
        .log = shadps4_dxil_log,
    };
    bool compiled;

    compiled = spirv_to_dxil(spirv, word_count, NULL, 0,
                             (dxil_spirv_shader_stage)stage, "main",
                             DXIL_VALIDATOR_1_4, &debug, &conf, &logger,
                             &object);
    if (!compiled || !object.binary.buffer || !object.binary.size) {
        if (error) {
            *error = g_string_free(messages, false);
            if (!**error) {
                g_free(*error);
                *error = g_strdup("SPIR-V to DXIL translation failed");
            }
        } else {
            g_string_free(messages, true);
        }
        spirv_to_dxil_free(&object);
        return false;
    }

    binary->data = g_memdup2(object.binary.buffer, object.binary.size);
    binary->size = object.binary.size;
    binary->requires_runtime_data = object.metadata.requires_runtime_data;
    binary->needs_draw_sysvals = object.metadata.needs_draw_sysvals;
    spirv_to_dxil_free(&object);
    g_string_free(messages, true);
    return true;
#else
    if (error) {
        *error = g_strdup("SPIR-V to DXIL translator is not linked");
    }
    return false;
#endif
}

bool shadps4_dxil_self_test(char **error)
{
    static const uint32_t compute_spirv[] = {
        0x07230203, 0x00010000, 0, 5, 0,
        0x00020011, 1,
        0x0003000e, 0, 1,
        0x0005000f, 5, 3, 0x6e69616d, 0,
        0x00060010, 3, 17, 1, 1, 1,
        0x00020013, 1,
        0x00030021, 2, 1,
        0x00050036, 1, 3, 0, 2,
        0x000200f8, 4,
        0x000100fd,
        0x00010038,
    };
    ShadPS4DxilBinary binary = { 0 };
    bool valid;

    if (!shadps4_dxil_compile(compute_spirv, ARRAY_SIZE(compute_spirv),
                              SHADPS4_DXIL_STAGE_COMPUTE, &binary, error)) {
        return false;
    }
    valid = binary.size >= 4 && !memcmp(binary.data, "DXBC", 4);
    if (!valid && error) {
        *error = g_strdup("translator returned an invalid DXIL container");
    }
    shadps4_dxil_binary_clear(&binary);
    return valid;
}

void shadps4_dxil_binary_clear(ShadPS4DxilBinary *binary)
{
    if (!binary) {
        return;
    }
    g_free(binary->data);
    memset(binary, 0, sizeof(*binary));
}
