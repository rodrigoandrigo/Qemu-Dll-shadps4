/*
 * shadPS4 D3D12 GPU backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#define QEMU_HOST_INTERNAL
#include "qemu/qemu-host.h"
#define CINTERFACE
#define COBJMACROS
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <math.h>
#include "hw/i386/shadps4-gpu-d3d12.h"
#include "hw/i386/shadps4-gpu-dxil.h"
#include "shadps4-gcn-bridge.h"

#define SHADPS4_D3D12_VIDEO_SURFACES 16
#define SHADPS4_D3D12_MAX_SURFACES 64
#define SHADPS4_D3D12_MAX_DEPTH_SURFACES 16
#define SHADPS4_PM4_TYPE3 3
#define SHADPS4_PM4_SET_BASE 0x11
#define SHADPS4_PM4_DRAW_INDIRECT 0x24
#define SHADPS4_PM4_DRAW_INDEX_INDIRECT 0x25
#define SHADPS4_PM4_INDEX_BASE 0x26
#define SHADPS4_PM4_DRAW_INDEX2 0x27
#define SHADPS4_PM4_INDEX_TYPE 0x2a
#define SHADPS4_PM4_DRAW_INDEX_AUTO 0x2d
#define SHADPS4_PM4_NUM_INSTANCES 0x2f
#define SHADPS4_PM4_DRAW_INDEX_OFFSET2 0x35
#define SHADPS4_PM4_DRAW_INDEX_INDIRECT_MULTI 0x38
#define SHADPS4_PM4_DISPATCH_DIRECT 0x15
#define SHADPS4_PM4_DISPATCH_INDIRECT 0x16
#define SHADPS4_PM4_DRAW_INDEX_INDIRECT_COUNT_MULTI 0x9d
#define SHADPS4_PM4_WAIT_REG_MEM 0x3c
#define SHADPS4_PM4_PFP_SYNC_ME 0x42
#define SHADPS4_PM4_SURFACE_SYNC 0x43
#define SHADPS4_PM4_EVENT_WRITE 0x46
#define SHADPS4_PM4_EVENT_WRITE_EOP 0x47
#define SHADPS4_PM4_RELEASE_MEM 0x49
#define SHADPS4_PM4_ACQUIRE_MEM 0x58
#define SHADPS4_PM4_SET_CONTEXT_REG 0x69
#define SHADPS4_PM4_SET_SH_REG 0x76
#define SHADPS4_PM4_SET_UCONFIG_REG 0x79
#define SHADPS4_CONTEXT_CB_COLOR0_BASE 0x318
#define SHADPS4_CONTEXT_CB_COLOR_STRIDE 0xf
#define SHADPS4_CONTEXT_CB_COUNT 8
#define SHADPS4_SHADER_PS_PGM_LO 0x008
#define SHADPS4_SHADER_VS_PGM_LO 0x048
#define SHADPS4_SHADER_GS_PGM_LO 0x088
#define SHADPS4_SHADER_ES_PGM_LO 0x0c8
#define SHADPS4_SHADER_HS_PGM_LO 0x108
#define SHADPS4_SHADER_LS_PGM_LO 0x148
#define SHADPS4_SHADER_CS_PGM_LO 0x20c
#define SHADPS4_CONTEXT_SHADER_STAGES 0x2d5
#define SHADPS4_CONTEXT_HOST_PRIMITIVE 0x3ff
#define SHADPS4_UCONFIG_PRIMITIVE_TYPE 0x242
#define SHADPS4_SHADER_SCAN_BYTES (64 * 1024)
#define SHADPS4_SHADER_MAX_BYTES (16 * 1024 * 1024)
#define SHADPS4_SHADER_CACHE_SIZE 256
#define SHADPS4_PIPELINE_CACHE_SIZE 128
#define SHADPS4_DESCRIPTOR_PAGES 64
#define SHADPS4_MAX_WRITEBACKS 256
#define SHADPS4_MAX_TRANSIENTS 128
#define SHADPS4_D3D12_FENCE_TIMEOUT_MS 10000
#define SHADPS4_SHADER_CS_SETTINGS 0x212
#define SHADPS4_SHADER_CS_USER_DATA 0x240

typedef struct ShadPS4ShaderBinaryInfo {
    uint64_t address;
    uint64_t hash;
    uint32_t length;
    bool is_srt;
    bool valid;
} ShadPS4ShaderBinaryInfo;

typedef struct ShadPS4D3D12Shader {
    uint64_t key;
    uint64_t variant_key;
    uint64_t hash;
    uint64_t last_use;
    ShadPS4GcnStage stage;
    ShadPS4DxilBinary dxil;
    uint32_t unified_bindings;
    uint32_t buffer_bindings;
    uint32_t user_data_bindings;
    ShadPS4GcnResource *resources;
    size_t resource_count;
    uint32_t *flat_user_data;
    size_t flat_user_data_words;
    struct ShadPS4D3D12BoundResource *bound_resources;
    uint32_t user_data_binding_start;
    uint32_t user_data_value_count;
    uint32_t user_data_values[16];
    uint32_t vertex_input_count;
    ShadPS4GcnVertexInput vertex_inputs[SHADPS4_GCN_MAX_VERTEX_INPUTS];
    bool valid;
} ShadPS4D3D12Shader;

typedef struct ShadPS4D3D12BoundResource {
    ID3D12Resource *resource;
    ID3D12Resource *upload;
    ID3D12Resource *readback;
    uint8_t *mapped;
    uint64_t size;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprints;
    UINT *footprint_rows;
    uint32_t subresource_count;
    uint64_t upload_size;
    D3D12_RESOURCE_STATES state;
} ShadPS4D3D12BoundResource;

static void shadps4_d3d12_bound_clear(ShadPS4D3D12BoundResource *bound);

typedef struct ShadPS4D3D12Writeback {
    ShadPS4D3D12BoundResource *bound;
    uint64_t guest_address;
    uint64_t size;
    ShadPS4GcnResource image;
    bool is_image;
} ShadPS4D3D12Writeback;

typedef struct ShadPS4D3D12Pipeline {
    uint64_t key;
    uint64_t last_use;
    ID3D12PipelineState *pso;
} ShadPS4D3D12Pipeline;

typedef struct ShadPS4D3D12Surface {
    ID3D12Resource *texture;
    ID3D12Resource *readback;
    HANDLE shared_handle;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    D3D12_RESOURCE_STATES state;
    uint64_t readback_size;
    uint64_t guest_address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t tiling_mode;
    uint32_t samples;
    DXGI_FORMAT dxgi_format;
    uint64_t last_use;
    bool video_out;
    bool rendered;
} ShadPS4D3D12Surface;

typedef struct ShadPS4D3D12DepthSurface {
    ID3D12Resource *texture;
    D3D12_RESOURCE_STATES state;
    uint64_t guest_address;
    uint32_t width;
    uint32_t height;
    uint32_t samples;
    DXGI_FORMAT format;
    DXGI_FORMAT srv_format;
    bool has_stencil;
    bool initialized;
} ShadPS4D3D12DepthSurface;

struct ShadPS4D3D12State {
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    HANDLE shared_fence_handle;
    LUID adapter_luid;
    ID3D12RootSignature *root_signature;
    ID3D12Resource *constant_upload;
    uint8_t *constant_map;
    ID3D12DescriptorHeap *rtv_heap;
    ID3D12DescriptorHeap *dsv_heap;
    ID3D12DescriptorHeap *resource_heap;
    ID3D12DescriptorHeap *sampler_heaps[SHADPS4_DESCRIPTOR_PAGES];
    UINT rtv_stride;
    UINT dsv_stride;
    UINT resource_stride;
    UINT sampler_stride;
    ShadPS4D3D12Surface surfaces[SHADPS4_D3D12_MAX_SURFACES];
    ShadPS4D3D12DepthSurface depth_surfaces[
        SHADPS4_D3D12_MAX_DEPTH_SURFACES];
    ShadPS4D3D12Shader shaders[SHADPS4_SHADER_CACHE_SIZE];
    ShadPS4D3D12Pipeline graphics_pipelines[SHADPS4_PIPELINE_CACHE_SIZE];
    ShadPS4D3D12Pipeline compute_pipelines[SHADPS4_PIPELINE_CACHE_SIZE];
    HANDLE fence_event;
    uint64_t fence_value;
    uint64_t submit_count;
    uint64_t draw_count;
    uint64_t dispatch_count;
    uint64_t unmatched_target_count;
    uint64_t unsupported_opcode_count;
    uint64_t draw_failure_count;
    uint64_t invalid_shader_count;
    uint64_t shader_compile_count;
    uint64_t shader_cache_hit_count;
    uint64_t shader_cache_clock;
    uint64_t pipeline_cache_clock;
    uint64_t surface_clock;
    uint32_t descriptor_page;
    CPUState *writeback_cpu;
    ShadPS4D3D12Writeback writebacks[SHADPS4_MAX_WRITEBACKS];
    uint32_t writeback_count;
    ID3D12Resource *transients[SHADPS4_MAX_TRANSIENTS];
    uint32_t transient_count;
    uint64_t reported_opcodes[4];
    uint32_t reported_no_draw_dwords[4];
    uint32_t reported_no_draw_count;
    bool traced_first_draw;
    bool traced_first_draw_state;
    bool traced_first_shader;
    bool ready;
};

static bool shadps4_d3d12_is_tail_padding(const uint32_t *commands,
                                          uint32_t offset,
                                          uint32_t command_dwords)
{
    for (uint32_t i = offset; i < command_dwords; i++) {
        uint32_t word = le32_to_cpu(commands[i]);

        if (word != 0 && word != UINT32_C(0x80000000)) {
            return false;
        }
    }
    return true;
}

static uint64_t shadps4_d3d12_shader_address(const uint32_t *registers,
                                              uint32_t register_count,
                                              uint32_t pgm_lo)
{
    uint64_t encoded;

    if (!registers || pgm_lo + 1 >= register_count) {
        return 0;
    }
    encoded = registers[pgm_lo] |
              ((uint64_t)(registers[pgm_lo + 1] & 0xff) << 32);
    return encoded << 8;
}

static bool shadps4_d3d12_queue_writeback(ShadPS4D3D12State *d3d12,
    ShadPS4D3D12BoundResource *bound, uint64_t guest_address, uint64_t size);

static bool shadps4_d3d12_read_shader_info(CPUState *cs, uint64_t address,
                                           ShadPS4ShaderBinaryInfo *info)
{
    static const uint8_t signature[] = { 'O', 'r', 'b', 'S', 'h', 'd', 'r' };
    g_autofree uint8_t *data = NULL;

    memset(info, 0, sizeof(*info));
    info->address = address;
    if (!cs || !address || address > UINT64_MAX - SHADPS4_SHADER_SCAN_BYTES) {
        return false;
    }
    data = g_malloc(SHADPS4_SHADER_SCAN_BYTES);
    if (cpu_memory_rw_debug(cs, address, data, SHADPS4_SHADER_SCAN_BYTES,
                            false) != 0) {
        return false;
    }
    for (uint32_t offset = 0;
         offset + 28 <= SHADPS4_SHADER_SCAN_BYTES; offset += 4) {
        uint32_t attributes;

        if (memcmp(data + offset, signature, sizeof(signature))) {
            continue;
        }
        memcpy(&attributes, data + offset + 8, sizeof(attributes));
        attributes = le32_to_cpu(attributes);
        info->length = attributes >> 8;
        memcpy(&info->hash, data + offset + 16, sizeof(info->hash));
        info->hash = le64_to_cpu(info->hash);
        info->is_srt = data[offset + 14] & 1;
        info->valid = info->length != 0 &&
                      info->length <= SHADPS4_SHADER_MAX_BYTES &&
                      !(info->length & 3);
        return info->valid;
    }
    return false;
}

static bool shadps4_d3d12_read_guest(void *opaque, uint64_t address,
                                     void *data, size_t size)
{
    CPUState *cs = opaque;

    return size <= INT_MAX &&
           cpu_memory_rw_debug(cs, address, data, size, false) == 0;
}

static bool shadps4_d3d12_shader_self_test(uint64_t *gcn_version,
                                           char **error)
{
#ifdef CONFIG_SHADPS4_GCN_BRIDGE
    static const uint32_t code[] = {
        UINT32_C(0xbeeb03ff), 0, UINT32_C(0xbf810000),
    };
    uint32_t user_data[16] = { 0 };
    ShadPS4GcnInput input = {
        .code = code,
        .code_words = ARRAY_SIZE(code),
        .user_data = user_data,
        .user_data_words = ARRAY_SIZE(user_data),
        .hash = UINT64_C(0x51454d552d47434e),
        .stage = SHADPS4_GCN_STAGE_COMPUTE,
        .logical_stage = SHADPS4_GCN_LOGICAL_COMPUTE,
        .runtime = {
            .num_allocated_vgprs = 4,
            .workgroup_size = { 1, 1, 1 },
        },
    };
    ShadPS4GcnOutput output = { 0 };
    ShadPS4DxilBinary dxil = { 0 };
    char *gcn_error = NULL;
    bool valid;

    if (gcn_version) {
        *gcn_version = shadps4_gcn_bridge_version();
    }
    if (!shadps4_gcn_compile(&input, &output, &gcn_error)) {
        if (error) {
            *error = g_strdup(gcn_error ?: "GCN self-test failed");
        }
        shadps4_gcn_error_free(gcn_error);
        return false;
    }
    valid = shadps4_dxil_compile(output.spirv, output.spirv_words,
                                 SHADPS4_DXIL_STAGE_COMPUTE, &dxil, error);
    shadps4_dxil_binary_clear(&dxil);
    shadps4_gcn_output_free(&output);
    shadps4_gcn_error_free(gcn_error);
    return valid;
#else
    if (gcn_version) {
        *gcn_version = 0;
    }
    if (error) {
        *error = g_strdup("GCN bridge is not linked");
    }
    return false;
#endif
}

static void shadps4_d3d12_fill_graphics_runtime(
    ShadPS4GcnRuntime *runtime, ShadPS4GcnStage stage,
    const uint32_t *context, uint32_t context_count)
{
    if (!context || context_count <= 0x207) {
        return;
    }
    if (stage == SHADPS4_GCN_STAGE_VERTEX ||
        stage == SHADPS4_GCN_STAGE_GEOMETRY) {
        uint32_t control = context[0x207];
        uint32_t output = 0;

        runtime->vertex_clip_disable = (context[0x204] >> 16) & 1;
        if ((control >> 21) & 1) {
            uint32_t *map = &runtime->vertex_outputs[output++ * 4];
            map[0] = (control >> 16) & 1 ? 1 : 0;
            map[1] = (control >> 17) & 1 ? 2 :
                     (control >> 25) & 1 ? 4 : 0;
            map[2] = (control >> 20) & 1 ? 3 :
                     (control >> 18) & 1 ? 5 : 0;
            map[3] = (control >> 19) & 1 ? 6 : 0;
        }
        for (uint32_t half = 0; half < 2; half++) {
            if (!((control >> (22 + half)) & 1) || output >= 3) {
                continue;
            }
            uint32_t *map = &runtime->vertex_outputs[output++ * 4];
            for (uint32_t component = 0; component < 4; component++) {
                uint32_t distance = half * 4 + component;

                map[component] = (control >> distance) & 1 ?
                    15 + distance : (control >> (8 + distance)) & 1 ?
                    7 + distance : 0;
            }
        }
        runtime->vertex_num_outputs = output;
    } else if (stage == SHADPS4_GCN_STAGE_FRAGMENT) {
        runtime->fragment_input_enable = context[0x1b3];
        runtime->fragment_input_address = context[0x1b4];
        runtime->fragment_num_inputs = MIN(context[0x1b6] & 0x3f, 32);
        for (uint32_t i = 0; i < runtime->fragment_num_inputs; i++) {
            runtime->fragment_inputs[i] = context[0x191 + i];
        }
        runtime->fragment_z_export_format = context[0x1c4] & 0xf;
        runtime->fragment_mrtz_mask = (context[0x203] & 1) |
            (((context[0x203] >> 1) & 3) ? 2 : 0) |
            (((context[0x203] >> 8) & 1) << 2) |
            (((context[0x203] >> 7) & 1) << 3);
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t info = context[SHADPS4_CONTEXT_CB_COLOR0_BASE +
                                    i * SHADPS4_CONTEXT_CB_COLOR_STRIDE + 4];
            uint32_t number = (info >> 8) & 7;

            runtime->fragment_color_data_format[i] = (info >> 2) & 0x1f;
            runtime->fragment_color_number_format[i] = number == 6 ? 9 : number;
            runtime->fragment_color_export_format[i] =
                (context[0x1c5] >> (i * 4)) & 0xf;
        }
    }
    if (context_count > 0x2db &&
        (stage == SHADPS4_GCN_STAGE_LOCAL ||
         stage == SHADPS4_GCN_STAGE_HULL ||
         stage == SHADPS4_GCN_STAGE_VERTEX ||
         stage == SHADPS4_GCN_STAGE_EXPORT)) {
        uint32_t config = context[0x2d6];
        uint32_t tess = context[0x2db];

        runtime->tess_input_control_points = (config >> 8) & 0x3f;
        runtime->tess_output_control_points = (config >> 14) & 0x3f;
        runtime->tess_type = tess & 3;
        runtime->tess_partitioning = (tess >> 2) & 7;
        runtime->tess_topology = (tess >> 5) & 7;
    }
    if (stage == SHADPS4_GCN_STAGE_GEOMETRY && context_count > 0x2e4) {
        uint32_t instances = context[0x2e4];
        uint32_t out_primitive = context[0x29b];

        runtime->geometry_output_vertices = context[0x2ce] & 0x7ff;
        runtime->geometry_invocations = (instances & 3) ?
            MAX((instances >> 2) & 0x3f, 1) : 1;
        runtime->geometry_input_primitive =
            context_count > SHADPS4_CONTEXT_HOST_PRIMITIVE ?
            context[SHADPS4_CONTEXT_HOST_PRIMITIVE] : 4;
        runtime->geometry_input_vertex_size = context[0x2ab];
        runtime->geometry_output_vertex_size = context[0x2d7];
        runtime->geometry_mode = context[0x290] & 7;
        for (uint32_t i = 0; i < 4; i++) {
            runtime->geometry_output_primitive[i] =
                (out_primitive >> (i * 6)) & 0x3f;
        }
    }
}

static uint64_t shadps4_d3d12_hash_bytes(uint64_t hash, const void *data,
                                         size_t size)
{
    const uint8_t *bytes = data;

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void shadps4_d3d12_shader_clear(ShadPS4D3D12Shader *shader)
{
    for (size_t i = 0; i < shader->resource_count; i++) {
        ShadPS4D3D12BoundResource *bound = &shader->bound_resources[i];

        if (bound->upload) {
            if (bound->mapped) {
                ID3D12Resource_Unmap(bound->upload, 0, NULL);
            }
            ID3D12Resource_Release(bound->upload);
        } else if (bound->resource && bound->mapped) {
            ID3D12Resource_Unmap(bound->resource, 0, NULL);
        }
        if (bound->resource) {
            ID3D12Resource_Release(bound->resource);
        }
        if (bound->readback) {
            ID3D12Resource_Release(bound->readback);
        }
        g_free(bound->footprints);
        g_free(bound->footprint_rows);
    }
    shadps4_dxil_binary_clear(&shader->dxil);
    g_free(shader->resources);
    g_free(shader->flat_user_data);
    g_free(shader->bound_resources);
    memset(shader, 0, sizeof(*shader));
}

static void shadps4_d3d12_shader_replace_metadata(
    ShadPS4D3D12Shader *shader, const ShadPS4GcnOutput *output)
{
    for (size_t i = 0; i < shader->resource_count; i++) {
        shadps4_d3d12_bound_clear(&shader->bound_resources[i]);
    }
    g_free(shader->resources);
    g_free(shader->flat_user_data);
    g_free(shader->bound_resources);
    shader->unified_bindings = output->unified_bindings;
    shader->buffer_bindings = output->buffer_bindings;
    shader->user_data_bindings = output->user_data_bindings;
    shader->resource_count = output->resource_count;
    shader->resources = g_memdup2(output->resources,
        output->resource_count * sizeof(*output->resources));
    shader->bound_resources = g_new0(ShadPS4D3D12BoundResource,
                                     output->resource_count);
    shader->flat_user_data_words = output->flat_user_data_words;
    shader->flat_user_data = g_memdup2(output->flat_user_data,
        output->flat_user_data_words * sizeof(*output->flat_user_data));
    shader->user_data_binding_start = output->user_data_binding_start;
    shader->user_data_value_count = output->user_data_value_count;
    memcpy(shader->user_data_values, output->user_data_values,
           sizeof(shader->user_data_values));
    shader->vertex_input_count = output->vertex_input_count;
    memcpy(shader->vertex_inputs, output->vertex_inputs,
           sizeof(shader->vertex_inputs));
}

static ShadPS4D3D12Shader *shadps4_d3d12_compile_shader(
    ShadPS4D3D12State *d3d12, CPUState *cs, const uint32_t *registers,
    uint32_t register_count, const uint32_t *context,
    uint32_t context_count, uint32_t pgm_lo, ShadPS4GcnStage stage,
    ShadPS4GcnLogicalStage logical_stage, ShadPS4DxilStage dxil_stage,
    uint32_t initial_unified_bindings, uint32_t initial_buffer_bindings,
    uint32_t initial_user_data_bindings, const char *stage_name)
{
    ShadPS4ShaderBinaryInfo info;
    ShadPS4GcnRuntime runtime = { 0 };
    ShadPS4GcnInput input = { 0 };
    ShadPS4GcnOutput output = { 0 };
    ShadPS4D3D12Shader *slot = NULL;
    g_autofree uint32_t *code = NULL;
    g_autofree uint32_t *geometry_copy_code = NULL;
    const uint32_t *user_data;
    uint32_t settings_lo;
    uint32_t settings_hi;
    uint32_t user_data_offset;
    uint64_t variant_key = UINT64_C(1469598103934665603);
    char *gcn_error = NULL;
    char *dxil_error = NULL;
    uint64_t address = shadps4_d3d12_shader_address(
        registers, register_count, pgm_lo);
    bool trace_shader = !d3d12->traced_first_shader;

    if (!address) {
        return NULL;
    }
    if (trace_shader) {
        d3d12->traced_first_shader = true;
        info_report("shadPS4 D3D12 first shader: stage=%s guest=%#" PRIx64
                    " pgm_lo=%#x", stage_name, address, pgm_lo);
    }
    if (!shadps4_d3d12_read_shader_info(cs, address, &info)) {
        d3d12->invalid_shader_count++;
        if (!(d3d12->invalid_shader_count &
              (d3d12->invalid_shader_count - 1))) {
            warn_report("shadPS4 D3D12: invalid %s shader at guest=%#" PRIx64
                        "; failures=%" PRIu64,
                        stage_name, address, d3d12->invalid_shader_count);
        }
        return NULL;
    }
    if (trace_shader) {
        info_report("shadPS4 D3D12 first shader: metadata length=%u"
                    " hash=%#" PRIx64 " srt=%u",
                    info.length, info.hash, info.is_srt);
    }

    if (stage == SHADPS4_GCN_STAGE_COMPUTE) {
        if (SHADPS4_SHADER_CS_USER_DATA + 16 > register_count ||
            SHADPS4_SHADER_CS_SETTINGS + 1 >= register_count) {
            return NULL;
        }
        user_data_offset = SHADPS4_SHADER_CS_USER_DATA;
        settings_lo = registers[SHADPS4_SHADER_CS_SETTINGS];
        settings_hi = registers[SHADPS4_SHADER_CS_SETTINGS + 1];
        runtime.workgroup_size[0] = registers[0x207] & 0xffff;
        runtime.workgroup_size[1] = registers[0x208] & 0xffff;
        runtime.workgroup_size[2] = registers[0x209] & 0xffff;
        for (uint32_t i = 0; i < 3; i++) {
            runtime.workgroup_size[i] = MAX(runtime.workgroup_size[i], 1);
            runtime.tgid_enable[i] = (settings_hi >> (7 + i)) & 1;
        }
        runtime.shared_memory_size = ((settings_hi >> 15) & 0x1ff) * 512;
    } else {
        if (pgm_lo + 20 > register_count) {
            return NULL;
        }
        user_data_offset = pgm_lo + 4;
        settings_lo = registers[pgm_lo + 2];
        settings_hi = registers[pgm_lo + 3];
    }
    user_data = &registers[user_data_offset];
    runtime.num_user_data = (settings_hi >> 1) & 0x1f;
    runtime.num_input_vgprs = (settings_lo >> 24) & 3;
    runtime.num_allocated_vgprs = ((settings_lo & 0x3f) + 1) * 4;
    shadps4_d3d12_fill_graphics_runtime(
        &runtime, stage, context, context_count);

    variant_key = shadps4_d3d12_hash_bytes(variant_key, &info.hash,
                                           sizeof(info.hash));
    variant_key = shadps4_d3d12_hash_bytes(variant_key, &stage,
                                           sizeof(stage));
    variant_key = shadps4_d3d12_hash_bytes(variant_key,
                                           &initial_unified_bindings,
                                   sizeof(initial_unified_bindings));
    variant_key = shadps4_d3d12_hash_bytes(variant_key,
                                           &initial_buffer_bindings,
                                   sizeof(initial_buffer_bindings));
    variant_key = shadps4_d3d12_hash_bytes(variant_key,
                                           &initial_user_data_bindings,
                                   sizeof(initial_user_data_bindings));
    variant_key = shadps4_d3d12_hash_bytes(variant_key, &runtime,
                                           sizeof(runtime));
    variant_key = shadps4_d3d12_hash_bytes(variant_key, user_data,
                                           16 * sizeof(*user_data));
    for (uint32_t i = 0; i < SHADPS4_SHADER_CACHE_SIZE; i++) {
        ShadPS4D3D12Shader *shader = &d3d12->shaders[i];

        if (shader->valid && shader->variant_key == variant_key &&
            shader->hash == info.hash && shader->stage == stage) {
            shader->last_use = ++d3d12->shader_cache_clock;
            d3d12->shader_cache_hit_count++;
            return shader;
        }
        if (!shader->valid || !slot || shader->last_use < slot->last_use) {
            slot = shader;
        }
    }

#ifdef CONFIG_SHADPS4_GCN_BRIDGE
    code = g_new(uint32_t, info.length / sizeof(uint32_t));
    if (cpu_memory_rw_debug(cs, address, code, info.length, false) != 0) {
        goto failed;
    }
    if (trace_shader && info.length >= 8 * sizeof(*code)) {
        info_report("shadPS4 D3D12 first shader: guest code read;"
                    " words=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]"
                    " compile GCN",
                    le32_to_cpu(code[0]), le32_to_cpu(code[1]),
                    le32_to_cpu(code[2]), le32_to_cpu(code[3]),
                    le32_to_cpu(code[4]), le32_to_cpu(code[5]),
                    le32_to_cpu(code[6]), le32_to_cpu(code[7]));
    } else if (trace_shader) {
        info_report("shadPS4 D3D12 first shader: guest code read; compile GCN");
    }
    if (stage == SHADPS4_GCN_STAGE_GEOMETRY) {
        ShadPS4ShaderBinaryInfo copy_info;
        uint64_t copy_address = shadps4_d3d12_shader_address(
            registers, register_count, SHADPS4_SHADER_VS_PGM_LO);

        if (!copy_address || !shadps4_d3d12_read_shader_info(
                cs, copy_address, &copy_info)) {
            goto failed;
        }
        geometry_copy_code = g_new(uint32_t,
                                   copy_info.length / sizeof(uint32_t));
        if (cpu_memory_rw_debug(cs, copy_address, geometry_copy_code,
                               copy_info.length, false) != 0) {
            goto failed;
        }
        runtime.geometry_copy_code = geometry_copy_code;
        runtime.geometry_copy_code_words = copy_info.length / sizeof(uint32_t);
        runtime.geometry_copy_hash = copy_info.hash;
    }
    input = (ShadPS4GcnInput) {
        .code = code,
        .code_words = info.length / sizeof(uint32_t),
        .user_data = user_data,
        .user_data_words = 16,
        .hash = info.hash,
        .stage = stage,
        .logical_stage = logical_stage,
        .runtime = runtime,
        .initial_unified_bindings = initial_unified_bindings,
        .initial_buffer_bindings = initial_buffer_bindings,
        .initial_user_data_bindings = initial_user_data_bindings,
        .read_memory = shadps4_d3d12_read_guest,
        .read_memory_opaque = cs,
    };
    if (!shadps4_gcn_compile(&input, &output, &gcn_error)) {
        goto failed;
    }
    if (trace_shader) {
        info_report("shadPS4 D3D12 first shader: GCN compiled; SPIR-V=%zu",
                    output.spirv_words);
    }
    uint64_t binary_key = shadps4_d3d12_hash_bytes(
        UINT64_C(1469598103934665603), output.spirv,
        output.spirv_words * sizeof(*output.spirv));

    binary_key = shadps4_d3d12_hash_bytes(binary_key, &dxil_stage,
                                          sizeof(dxil_stage));
    for (uint32_t i = 0; i < SHADPS4_SHADER_CACHE_SIZE; i++) {
        ShadPS4D3D12Shader *shader = &d3d12->shaders[i];

        if (shader->valid && shader->key == binary_key &&
            shader->stage == stage) {
            ShadPS4DxilBinary dxil = {
                .data = g_memdup2(shader->dxil.data, shader->dxil.size),
                .size = shader->dxil.size,
                .requires_runtime_data = shader->dxil.requires_runtime_data,
                .needs_draw_sysvals = shader->dxil.needs_draw_sysvals,
            };

            shadps4_d3d12_shader_clear(slot);
            slot->dxil = dxil;
            slot->key = binary_key;
            slot->variant_key = variant_key;
            slot->hash = info.hash;
            slot->stage = stage;
            shadps4_d3d12_shader_replace_metadata(slot, &output);
            slot->last_use = ++d3d12->shader_cache_clock;
            slot->valid = true;
            d3d12->shader_cache_hit_count++;
            shadps4_gcn_output_free(&output);
            shadps4_gcn_error_free(gcn_error);
            g_free(dxil_error);
            return slot;
        }
    }
    shadps4_d3d12_shader_clear(slot);
    if (!shadps4_dxil_compile(output.spirv, output.spirv_words, dxil_stage,
                              &slot->dxil, &dxil_error)) {
        goto failed;
    }
    slot->key = binary_key;
    slot->variant_key = variant_key;
    slot->hash = info.hash;
    slot->stage = stage;
    shadps4_d3d12_shader_replace_metadata(slot, &output);
    slot->last_use = ++d3d12->shader_cache_clock;
    slot->valid = true;
    d3d12->shader_compile_count++;
    info_report("shadPS4 D3D12: compiled %s GCN shader guest=%#" PRIx64
                " hash=%#" PRIx64 " code=%u DXIL=%zu SRT=%u bindings=%u",
                stage_name, address, info.hash, info.length, slot->dxil.size,
                info.is_srt, output.unified_bindings);
    shadps4_gcn_output_free(&output);
    shadps4_gcn_error_free(gcn_error);
    g_free(dxil_error);
    return slot;

failed:
    d3d12->invalid_shader_count++;
    warn_report("shadPS4 D3D12: failed to compile %s shader hash=%#" PRIx64
                ": %s%s%s", stage_name, info.hash,
                gcn_error ?: "", gcn_error && dxil_error ? "; " : "",
                dxil_error ?: "translation failed");
    shadps4_gcn_output_free(&output);
    shadps4_gcn_error_free(gcn_error);
    g_free(dxil_error);
#else
    (void)logical_stage;
    (void)dxil_stage;
#endif
    return NULL;
}

static bool shadps4_d3d12_is_draw(uint32_t opcode)
{
    switch (opcode) {
    case SHADPS4_PM4_DRAW_INDIRECT:
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT:
    case SHADPS4_PM4_DRAW_INDEX2:
    case SHADPS4_PM4_DRAW_INDEX_AUTO:
    case SHADPS4_PM4_DRAW_INDEX_OFFSET2:
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT_MULTI:
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT_COUNT_MULTI:
        return true;
    default:
        return false;
    }
}

static bool shadps4_d3d12_is_dispatch(uint32_t opcode)
{
    return opcode == SHADPS4_PM4_DISPATCH_DIRECT ||
           opcode == SHADPS4_PM4_DISPATCH_INDIRECT;
}

static void shadps4_d3d12_report(const char *operation, HRESULT hr);
static DXGI_FORMAT shadps4_d3d12_resource_format(
    const ShadPS4GcnResource *resource);

static DXGI_FORMAT shadps4_d3d12_color_target_format(uint32_t info)
{
    uint32_t format = (info >> 2) & 0x1f;
    uint32_t number = (info >> 8) & 7;

    switch (format) {
    case 1:
        return number == 4 ? DXGI_FORMAT_R8_UINT :
               number == 5 ? DXGI_FORMAT_R8_SINT : DXGI_FORMAT_R8_UNORM;
    case 2:
        return number == 7 ? DXGI_FORMAT_R16_FLOAT :
               number == 4 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R16_UNORM;
    case 3:
        return DXGI_FORMAT_R8G8_UNORM;
    case 4:
        return number == 7 ? DXGI_FORMAT_R32_FLOAT :
               number == 5 ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
    case 5:
        return number == 7 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16_UNORM;
    case 8: case 9:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 10:
        return number == 6 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB :
               number == 4 ? DXGI_FORMAT_R8G8B8A8_UINT :
               number == 5 ? DXGI_FORMAT_R8G8B8A8_SINT :
               DXGI_FORMAT_R8G8B8A8_UNORM;
    case 11:
        return number == 7 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32_UINT;
    case 12:
        return number == 7 ? DXGI_FORMAT_R16G16B16A16_FLOAT :
               DXGI_FORMAT_R16G16B16A16_UNORM;
    case 14:
        return number == 7 ? DXGI_FORMAT_R32G32B32A32_FLOAT :
               DXGI_FORMAT_R32G32B32A32_UINT;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static bool shadps4_d3d12_transition_surface(
    ShadPS4D3D12State *d3d12, ShadPS4D3D12Surface *surface,
    D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_BARRIER barrier;

    if (surface->state == state) {
        return true;
    }
    barrier = (D3D12_RESOURCE_BARRIER) {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = surface->texture,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = surface->state,
            .StateAfter = state,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
    surface->state = state;
    return true;
}

static void shadps4_d3d12_transition_depth_surface(
    ShadPS4D3D12State *d3d12, ShadPS4D3D12DepthSurface *surface,
    D3D12_RESOURCE_STATES state)
{
    if (surface->state != state) {
        D3D12_RESOURCE_BARRIER barrier = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = surface->texture,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = surface->state,
                .StateAfter = state,
            },
        };

        ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
        surface->state = state;
    }
}

static int shadps4_d3d12_find_surface_by_address(
    const ShadPS4D3D12State *d3d12, uint64_t address)
{
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_SURFACES; i++) {
        if (d3d12->surfaces[i].texture &&
            (d3d12->surfaces[i].guest_address & ~UINT64_C(0xff)) ==
            (address & ~UINT64_C(0xff))) {
            return i;
        }
    }
    return -1;
}

static int shadps4_d3d12_create_offscreen_surface(
    ShadPS4D3D12State *d3d12, uint64_t address, uint32_t width,
    uint32_t height, DXGI_FORMAT format, uint32_t samples)
{
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = { .Count = samples },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 (samples == 1 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 0),
    };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ShadPS4D3D12Surface *surface;
    HRESULT hr;
    int slot = -1;

    for (uint32_t i = SHADPS4_D3D12_VIDEO_SURFACES;
         i < SHADPS4_D3D12_MAX_SURFACES; i++) {
        if (!d3d12->surfaces[i].texture) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || !address || !width || !height ||
        width > 8192 || height > 8192) {
        return -1;
    }
    surface = &d3d12->surfaces[slot];
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
        &IID_ID3D12Resource, (void **)&surface->texture);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create offscreen render target", hr);
        return -1;
    }
    d3d12->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        d3d12->rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)slot * d3d12->rtv_stride;
    ID3D12Device_CreateRenderTargetView(
        d3d12->device, surface->texture, NULL, rtv);
    surface->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    surface->guest_address = address;
    surface->width = width;
    surface->height = height;
    surface->pitch = width;
    surface->samples = samples;
    surface->dxgi_format = format;
    surface->last_use = ++d3d12->surface_clock;
    return slot;
}

static uint32_t shadps4_d3d12_find_color_targets(
    ShadPS4D3D12State *d3d12, const uint32_t *context_registers,
    uint32_t context_register_count, int targets[8])
{
    uint32_t count = 0;
    uint32_t target_mask = context_register_count > 0x8e ?
                           context_registers[0x8e] : UINT32_MAX;

    for (uint32_t cb = 0; cb < SHADPS4_CONTEXT_CB_COUNT; cb++) {
        uint32_t reg = SHADPS4_CONTEXT_CB_COLOR0_BASE +
                       cb * SHADPS4_CONTEXT_CB_COLOR_STRIDE;
        uint64_t address;
        uint32_t pitch;
        uint32_t height;
        uint32_t samples;
        int target;

        if (reg + 4 >= context_register_count ||
            !context_registers[reg] || !((target_mask >> (cb * 4)) & 0xf)) {
            continue;
        }
        address = (uint64_t)context_registers[reg] << 8;
        target = shadps4_d3d12_find_surface_by_address(d3d12, address);
        if (target < 0) {
            pitch = ((context_registers[reg + 1] & 0x7ff) + 1) << 3;
            height = pitch ?
                ((context_registers[reg + 2] & 0x3fffff) + 1) * 64 /
                pitch : 0;
            samples = 1u << ((context_registers[reg + 5] >> 15) & 3);
            target = shadps4_d3d12_create_offscreen_surface(
                d3d12, address, pitch, height,
                shadps4_d3d12_color_target_format(
                    context_registers[reg + 4]), samples);
        }
        if (target >= 0) {
            d3d12->surfaces[target].last_use = ++d3d12->surface_clock;
            targets[count++] = target;
        }
    }
    return count;
}

static int shadps4_d3d12_find_depth_target(
    ShadPS4D3D12State *d3d12, const uint32_t *context,
    uint32_t context_count)
{
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
    };
    D3D12_CLEAR_VALUE clear_value;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {
        .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    uint32_t zinfo;
    uint32_t zformat;
    uint64_t address;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    bool stencil;
    uint32_t samples;
    int slot = -1;
    HRESULT hr;

    if (!context || context_count <= 0x17) {
        return -1;
    }
    zinfo = context[0x10];
    zformat = zinfo & 3;
    samples = 1u << ((zinfo >> 2) & 3);
    stencil = (context[0x11] & 1) &&
              (context[0x15] || context[0x13]);
    address = (uint64_t)(context[0x14] ?: context[0x12]) << 8;
    if (!address || (zformat != 1 && zformat != 3)) {
        return -1;
    }
    size = context[0x16];
    width = ((size & 0x7ff) + 1) << 3;
    height = (((size >> 11) & 0x7ff) + 1) << 3;
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_DEPTH_SURFACES; i++) {
        ShadPS4D3D12DepthSurface *surface = &d3d12->depth_surfaces[i];

        if (surface->texture && surface->guest_address == address &&
            surface->width == width && surface->height == height) {
            return i;
        }
        if (!surface->texture && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0 || !width || !height || width > 8192 || height > 8192) {
        return -1;
    }
    desc.Width = width;
    desc.Height = height;
    desc.SampleDesc.Count = samples;
    dsv_desc.ViewDimension = samples > 1 ?
        D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Format = zformat == 3 ?
        (stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT) :
        (stencil ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D16_UNORM);
    desc.Format = zformat == 3 ?
        (stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS) :
        (stencil ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R16_TYPELESS);
    clear_value = (D3D12_CLEAR_VALUE) {
        .Format = dsv_desc.Format,
        .DepthStencil = { .Depth = 1.0f, .Stencil = 0 },
    };
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
        &IID_ID3D12Resource,
        (void **)&d3d12->depth_surfaces[slot].texture);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create depth/stencil target", hr);
        return -1;
    }
    d3d12->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        d3d12->dsv_heap, &dsv);
    dsv.ptr += (SIZE_T)slot * d3d12->dsv_stride;
    ID3D12Device_CreateDepthStencilView(
        d3d12->device, d3d12->depth_surfaces[slot].texture, &dsv_desc, dsv);
    d3d12->depth_surfaces[slot].state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    d3d12->depth_surfaces[slot].guest_address = address;
    d3d12->depth_surfaces[slot].width = width;
    d3d12->depth_surfaces[slot].height = height;
    d3d12->depth_surfaces[slot].samples = samples;
    d3d12->depth_surfaces[slot].format = dsv_desc.Format;
    d3d12->depth_surfaces[slot].srv_format = zformat == 3 ?
        (stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT) :
        (stencil ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R16_UNORM);
    d3d12->depth_surfaces[slot].has_stencil = stencil;
    d3d12->depth_surfaces[slot].initialized = false;
    return slot;
}

static void shadps4_d3d12_report_opcode_once(ShadPS4D3D12State *d3d12,
                                              uint32_t opcode)
{
    uint64_t mask = UINT64_C(1) << (opcode & 63);
    uint64_t *word = &d3d12->reported_opcodes[opcode >> 6];

    d3d12->unsupported_opcode_count++;
    if (!(*word & mask)) {
        *word |= mask;
        warn_report("shadPS4 D3D12: PM4 opcode %#x could not be emitted "
                    "by the current packet/resource path", opcode);
    }
}

static bool shadps4_d3d12_is_sync_opcode(uint32_t opcode)
{
    switch (opcode) {
    case SHADPS4_PM4_WAIT_REG_MEM:
    case SHADPS4_PM4_PFP_SYNC_ME:
    case SHADPS4_PM4_SURFACE_SYNC:
    case SHADPS4_PM4_EVENT_WRITE:
    case SHADPS4_PM4_EVENT_WRITE_EOP:
    case SHADPS4_PM4_RELEASE_MEM:
    case SHADPS4_PM4_ACQUIRE_MEM:
        return true;
    default:
        return false;
    }
}

static void shadps4_d3d12_record_sync(ShadPS4D3D12State *d3d12)
{
    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
        .UAV = { .pResource = NULL },
    };

    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
}

static void shadps4_d3d12_report(const char *operation, HRESULT hr)
{
    warn_report("shadPS4 D3D12: %s failed: HRESULT=%#lx",
                operation, (unsigned long)hr);
}

typedef HRESULT (WINAPI *ShadPS4D3D12GetDebugInterface)(REFIID, void **);

static void shadps4_d3d12_enable_debug_layer(void)
{
    HMODULE module;
    ShadPS4D3D12GetDebugInterface get_debug;
    ID3D12Debug *debug = NULL;

    if (!g_getenv("SHADPS4_D3D12_DEBUG")) {
        return;
    }
    module = GetModuleHandleW(L"d3d12.dll");
    get_debug = module ? (ShadPS4D3D12GetDebugInterface)GetProcAddress(
        module, "D3D12GetDebugInterface") : NULL;
    if (!get_debug || FAILED(get_debug(&IID_ID3D12Debug, (void **)&debug))) {
        warn_report("shadPS4 D3D12: debug layer is unavailable");
        return;
    }
    ID3D12Debug_EnableDebugLayer(debug);
    ID3D12Debug_Release(debug);
    info_report("shadPS4 D3D12: debug layer enabled");
}

static void shadps4_d3d12_report_debug_messages(ID3D12Device *device)
{
    ID3D12InfoQueue *queue = NULL;
    UINT64 count;

    if (!g_getenv("SHADPS4_D3D12_DEBUG") ||
        FAILED(ID3D12Device_QueryInterface(
            device, &IID_ID3D12InfoQueue, (void **)&queue))) {
        return;
    }
    count = ID3D12InfoQueue_GetNumStoredMessagesAllowedByRetrievalFilter(queue);
    for (UINT64 i = 0; i < count; i++) {
        SIZE_T size = 0;
        D3D12_MESSAGE *message;

        if (FAILED(ID3D12InfoQueue_GetMessage(queue, i, NULL, &size)) ||
            !size) {
            continue;
        }
        message = g_malloc(size);
        if (SUCCEEDED(ID3D12InfoQueue_GetMessage(
                queue, i, message, &size))) {
            warn_report("shadPS4 D3D12 debug: category=%u severity=%u "
                        "id=%u message='%s'", message->Category,
                        message->Severity, message->ID,
                        message->pDescription ?: "");
        }
        g_free(message);
    }
    ID3D12InfoQueue_ClearStoredMessages(queue);
    ID3D12InfoQueue_Release(queue);
}

static bool shadps4_d3d12_create_root_signature(ShadPS4D3D12State *d3d12)
{
    D3D12_DESCRIPTOR_RANGE ranges[3] = {
        {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 256,
            .BaseShaderRegister = 0,
        }, {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
            .NumDescriptors = 256,
            .BaseShaderRegister = 0,
        }, {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
            .NumDescriptors = 256,
            .BaseShaderRegister = 0,
        },
    };
    D3D12_ROOT_PARAMETER parameters[5] = {
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = { .ShaderRegister = 0, .RegisterSpace = 30 },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        }, {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = { .ShaderRegister = 0, .RegisterSpace = 31 },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        }, {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {
                .NumDescriptorRanges = 1,
                .pDescriptorRanges = &ranges[0],
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        }, {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {
                .NumDescriptorRanges = 1,
                .pDescriptorRanges = &ranges[1],
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        }, {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {
                .NumDescriptorRanges = 1,
                .pDescriptorRanges = &ranges[2],
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
    };
    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = ARRAY_SIZE(parameters),
        .pParameters = parameters,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    ID3DBlob *blob = NULL;
    ID3DBlob *messages = NULL;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages);

    if (FAILED(hr)) {
        warn_report("shadPS4 D3D12: root signature serialization failed: "
                    "HRESULT=%#lx message=%s", (unsigned long)hr,
                    messages ? (const char *)ID3D10Blob_GetBufferPointer(messages)
                             : "none");
        if (messages) {
            ID3D10Blob_Release(messages);
        }
        return false;
    }
    hr = ID3D12Device_CreateRootSignature(
        d3d12->device, 0, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
        (void **)&d3d12->root_signature);
    ID3D10Blob_Release(blob);
    if (messages) {
        ID3D10Blob_Release(messages);
    }
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateRootSignature", hr);
        return false;
    }
    return true;
}

static bool shadps4_d3d12_create_constant_upload(ShadPS4D3D12State *d3d12)
{
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = 512 * SHADPS4_DESCRIPTOR_PAGES,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    D3D12_RANGE read_range = { 0, 0 };
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
        (void **)&d3d12->constant_upload);

    if (FAILED(hr)) {
        shadps4_d3d12_report("Create constant upload buffer", hr);
        return false;
    }
    hr = ID3D12Resource_Map(d3d12->constant_upload, 0, &read_range,
                            (void **)&d3d12->constant_map);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Map constant upload buffer", hr);
        return false;
    }
    memset(d3d12->constant_map, 0, 512 * SHADPS4_DESCRIPTOR_PAGES);
    return true;
}

static D3D12_BLEND shadps4_d3d12_blend_factor(uint32_t factor)
{
    static const D3D12_BLEND factors[] = {
        D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR,
        D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,
        D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA,
        D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_COLOR,
        D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA_SAT,
    };

    if (factor < ARRAY_SIZE(factors)) {
        return factors[factor];
    }
    switch (factor) {
    case 13: case 19: return D3D12_BLEND_BLEND_FACTOR;
    case 14: case 20: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 15: return D3D12_BLEND_SRC1_COLOR;
    case 16: return D3D12_BLEND_INV_SRC1_COLOR;
    case 17: return D3D12_BLEND_SRC1_ALPHA;
    case 18: return D3D12_BLEND_INV_SRC1_ALPHA;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP shadps4_d3d12_blend_op(uint32_t op)
{
    static const D3D12_BLEND_OP ops[] = {
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,
        D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX,
        D3D12_BLEND_OP_REV_SUBTRACT,
    };
    return op < ARRAY_SIZE(ops) ? ops[op] : D3D12_BLEND_OP_ADD;
}

static D3D12_COMPARISON_FUNC shadps4_d3d12_depth_compare(uint32_t compare)
{
    static const D3D12_COMPARISON_FUNC funcs[8] = {
        D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS,
    };
    return funcs[compare & 7];
}

static D3D12_STENCIL_OP shadps4_d3d12_stencil_op(uint32_t op)
{
    static const D3D12_STENCIL_OP ops[10] = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO,
        D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
        D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR_SAT,
        D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT,
        D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR,
    };

    return op < ARRAY_SIZE(ops) ? ops[op] : D3D12_STENCIL_OP_KEEP;
}

static uint32_t shadps4_d3d12_color_slot_for_surface(
    const ShadPS4D3D12Surface *surface, const uint32_t *context,
    uint32_t context_count)
{
    for (uint32_t cb = 0; cb < SHADPS4_CONTEXT_CB_COUNT; cb++) {
        uint32_t reg = SHADPS4_CONTEXT_CB_COLOR0_BASE +
                       cb * SHADPS4_CONTEXT_CB_COLOR_STRIDE;

        if (reg < context_count &&
            ((uint64_t)context[reg] << 8) == surface->guest_address) {
            return cb;
        }
    }
    return 0;
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE shadps4_d3d12_topology_type(
    uint32_t primitive, bool tessellation)
{
    if (tessellation) {
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    }
    if (primitive == 1) {
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }
    if (primitive == 2 || primitive == 3 || primitive == 10 ||
        primitive == 11 || primitive == 18) {
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

static D3D_PRIMITIVE_TOPOLOGY shadps4_d3d12_topology(uint32_t primitive)
{
    switch (primitive) {
    case 1: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 2: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case 10: return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    case 11: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
    case 12: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    case 13: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static bool shadps4_d3d12_valid_dxil(const ShadPS4D3D12Shader *shader)
{
    return shader && shader->dxil.data && shader->dxil.size >= 32 &&
           !memcmp(shader->dxil.data, "DXBC", 4);
}

static ShadPS4D3D12Pipeline *shadps4_d3d12_graphics_pipeline(
    ShadPS4D3D12State *d3d12, ShadPS4D3D12Shader *vs,
    ShadPS4D3D12Shader *hs, ShadPS4D3D12Shader *ds,
    ShadPS4D3D12Shader *gs, ShadPS4D3D12Shader *ps, const uint32_t *context,
    uint32_t context_count, const int *targets, uint32_t target_count,
    int depth_target)
{
    ShadPS4D3D12Pipeline *slot = NULL;
    uint64_t ps_key = ps ? ps->key : 0;
    uint64_t key = vs->key ^ (ps_key + UINT64_C(0x9e3779b97f4a7c15) +
                              (vs->key << 6) + (vs->key >> 2));
    uint32_t blend = context_count > 0x1e0 ? context[0x1e0] : 0;
    uint32_t polygon = context_count > 0x205 ? context[0x205] : 0;
    uint32_t color_mask = context_count > 0x8f ?
        context[0x8e] & context[0x8f] : UINT32_MAX;
    uint32_t depth_control = context_count > 0x200 ? context[0x200] : 0;
    uint32_t stencil_control = context_count > 0x10b ? context[0x10b] : 0;
    uint32_t stencil_ref = context_count > 0x10c ? context[0x10c] : 0;
    uint32_t samples = target_count ? d3d12->surfaces[targets[0]].samples :
        d3d12->depth_surfaces[depth_target].samples;
    uint32_t primitive = context_count > SHADPS4_CONTEXT_HOST_PRIMITIVE ?
        context[SHADPS4_CONTEXT_HOST_PRIMITIVE] : 4;

    if (!shadps4_d3d12_valid_dxil(vs) ||
        (ps && !shadps4_d3d12_valid_dxil(ps)) ||
        (hs && !shadps4_d3d12_valid_dxil(hs)) ||
        (ds && !shadps4_d3d12_valid_dxil(ds)) ||
        (gs && !shadps4_d3d12_valid_dxil(gs))) {
        warn_report("shadPS4 D3D12: refusing graphics pipeline with invalid "
                    "DXIL container: VS=%zu HS=%zu DS=%zu GS=%zu PS=%zu",
                    vs ? vs->dxil.size : 0, hs ? hs->dxil.size : 0,
                    ds ? ds->dxil.size : 0, gs ? gs->dxil.size : 0,
                    ps ? ps->dxil.size : 0);
        return NULL;
    }
    samples = MAX(samples, 1u);
    key = shadps4_d3d12_hash_bytes(key, &blend, sizeof(blend));
    key = shadps4_d3d12_hash_bytes(key, &polygon, sizeof(polygon));
    key = shadps4_d3d12_hash_bytes(key, &color_mask, sizeof(color_mask));
    if (context_count >= 0x1e8) {
        key = shadps4_d3d12_hash_bytes(key, &context[0x1e0],
                                      8 * sizeof(uint32_t));
    }
    key = shadps4_d3d12_hash_bytes(key, &depth_control, sizeof(depth_control));
    key = shadps4_d3d12_hash_bytes(key, &stencil_control,
                                   sizeof(stencil_control));
    key = shadps4_d3d12_hash_bytes(key, &stencil_ref, sizeof(stencil_ref));
    key = shadps4_d3d12_hash_bytes(key, &samples, sizeof(samples));
    key = shadps4_d3d12_hash_bytes(key, &primitive, sizeof(primitive));
    if (hs) {
        key = shadps4_d3d12_hash_bytes(key, &hs->key, sizeof(hs->key));
    }
    if (ds) {
        key = shadps4_d3d12_hash_bytes(key, &ds->key, sizeof(ds->key));
    }
    if (gs) {
        key = shadps4_d3d12_hash_bytes(key, &gs->key, sizeof(gs->key));
    }
    if (depth_target >= 0) {
        key = shadps4_d3d12_hash_bytes(key,
            &d3d12->depth_surfaces[depth_target].format,
            sizeof(d3d12->depth_surfaces[depth_target].format));
    }
    for (uint32_t i = 0; i < target_count; i++) {
        key = shadps4_d3d12_hash_bytes(key,
            &d3d12->surfaces[targets[i]].dxgi_format,
            sizeof(d3d12->surfaces[targets[i]].dxgi_format));
    }

    for (uint32_t i = 0; i < SHADPS4_PIPELINE_CACHE_SIZE; i++) {
        ShadPS4D3D12Pipeline *pipeline = &d3d12->graphics_pipelines[i];

        if (pipeline->pso && pipeline->key == key) {
            pipeline->last_use = ++d3d12->pipeline_cache_clock;
            return pipeline;
        }
        if (!pipeline->pso || !slot || pipeline->last_use < slot->last_use) {
            slot = pipeline;
        }
    }
    if (slot->pso) {
        ID3D12PipelineState_Release(slot->pso);
        memset(slot, 0, sizeof(*slot));
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {
        .pRootSignature = d3d12->root_signature,
        .VS = { vs->dxil.data, vs->dxil.size },
        .HS = { hs ? hs->dxil.data : NULL, hs ? hs->dxil.size : 0 },
        .DS = { ds ? ds->dxil.data : NULL, ds ? ds->dxil.size : 0 },
        .GS = { gs ? gs->dxil.data : NULL, gs ? gs->dxil.size : 0 },
        .PS = { ps ? ps->dxil.data : NULL, ps ? ps->dxil.size : 0 },
        .SampleMask = UINT_MAX,
        .RasterizerState = {
            .FillMode = ((polygon >> 3) & 3) &&
                        ((polygon >> 5) & 7) == 1 ?
                        D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID,
            .CullMode = (polygon & 3) == 1 ? D3D12_CULL_MODE_FRONT :
                        (polygon & 3) == 2 ? D3D12_CULL_MODE_BACK :
                        (polygon & 3) == 3 ? D3D12_CULL_MODE_FRONT :
                        D3D12_CULL_MODE_NONE,
            .FrontCounterClockwise = !((polygon >> 2) & 1),
            .DepthClipEnable = TRUE,
        },
        .DepthStencilState = {
            .DepthEnable = depth_target >= 0 && (depth_control & 2),
            .DepthWriteMask = depth_control & 4 ?
                D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO,
            .DepthFunc = shadps4_d3d12_depth_compare(
                (depth_control >> 4) & 7),
            .StencilEnable = depth_target >= 0 && (depth_control & 1),
            .StencilReadMask = (stencil_ref >> 8) & 0xff,
            .StencilWriteMask = (stencil_ref >> 16) & 0xff,
            .FrontFace = {
                .StencilFailOp = shadps4_d3d12_stencil_op(
                    stencil_control & 0xf),
                .StencilDepthFailOp = shadps4_d3d12_stencil_op(
                    (stencil_control >> 8) & 0xf),
                .StencilPassOp = shadps4_d3d12_stencil_op(
                    (stencil_control >> 4) & 0xf),
                .StencilFunc = shadps4_d3d12_depth_compare(
                    (depth_control >> 8) & 7),
            },
            .BackFace = {
                .StencilFailOp = shadps4_d3d12_stencil_op(
                    (stencil_control >> 12) & 0xf),
                .StencilDepthFailOp = shadps4_d3d12_stencil_op(
                    (stencil_control >> 20) & 0xf),
                .StencilPassOp = shadps4_d3d12_stencil_op(
                    (stencil_control >> 16) & 0xf),
                .StencilFunc = shadps4_d3d12_depth_compare(
                    (depth_control >> 20) & 7),
            },
        },
        .PrimitiveTopologyType = shadps4_d3d12_topology_type(primitive, hs),
        .SampleDesc = { .Count = samples },
    };
    D3D12_INPUT_ELEMENT_DESC input_elements[SHADPS4_GCN_MAX_VERTEX_INPUTS];

    for (uint32_t i = 0; i < vs->vertex_input_count; i++) {
        const ShadPS4GcnVertexInput *input = &vs->vertex_inputs[i];
        ShadPS4GcnResource format_resource = {
            .data_format = input->data_format,
            .number_format = input->number_format,
        };
        DXGI_FORMAT format = shadps4_d3d12_resource_format(&format_resource);

        if (format == DXGI_FORMAT_UNKNOWN || !input->stride) {
            warn_report("shadPS4 D3D12: invalid vertex input %u: "
                        "semantic=%u format=%u/%u stride=%u", i,
                        input->semantic, input->data_format,
                        input->number_format, input->stride);
            return NULL;
        }
        input_elements[i] = (D3D12_INPUT_ELEMENT_DESC) {
            .SemanticName = "TEXCOORD",
            .SemanticIndex = input->semantic,
            .Format = format,
            .InputSlot = input->semantic,
            .AlignedByteOffset = 0,
            .InputSlotClass = input->instance_step_rate ?
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA :
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            .InstanceDataStepRate = input->instance_step_rate,
        };
    }
    desc.InputLayout = (D3D12_INPUT_LAYOUT_DESC) {
        .pInputElementDescs = input_elements,
        .NumElements = vs->vertex_input_count,
    };
    desc.NumRenderTargets = target_count;
    for (uint32_t i = 0; i < target_count; i++) {
        uint32_t cb = shadps4_d3d12_color_slot_for_surface(
            &d3d12->surfaces[targets[i]], context, context_count);
        uint32_t cb_blend = context_count > 0x1e0 + cb ?
            context[0x1e0 + cb] : 0;
        D3D12_RENDER_TARGET_BLEND_DESC *rt =
            &desc.BlendState.RenderTarget[i];

        desc.RTVFormats[i] = d3d12->surfaces[targets[i]].dxgi_format;
        *rt = (D3D12_RENDER_TARGET_BLEND_DESC) {
            .BlendEnable = (cb_blend >> 30) & 1,
            .SrcBlend = shadps4_d3d12_blend_factor(cb_blend & 0x1f),
            .DestBlend = shadps4_d3d12_blend_factor(
                (cb_blend >> 8) & 0x1f),
            .BlendOp = shadps4_d3d12_blend_op((cb_blend >> 5) & 7),
            .SrcBlendAlpha = shadps4_d3d12_blend_factor(
                (cb_blend >> 16) & 0x1f),
            .DestBlendAlpha = shadps4_d3d12_blend_factor(
                (cb_blend >> 24) & 0x1f),
            .BlendOpAlpha = shadps4_d3d12_blend_op(
                (cb_blend >> 21) & 7),
            .RenderTargetWriteMask = (color_mask >> (cb * 4)) & 0xf,
        };
    }
    if (depth_target >= 0) {
        desc.DSVFormat = d3d12->depth_surfaces[depth_target].format;
    }
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(
        d3d12->device, &desc, &IID_ID3D12PipelineState,
        (void **)&slot->pso);

    if (FAILED(hr)) {
        static bool probes_reported;

        shadps4_d3d12_report("CreateGraphicsPipelineState", hr);
        shadps4_d3d12_report_debug_messages(d3d12->device);
        warn_report("shadPS4 D3D12 PSO: VS=%zu HS=%zu DS=%zu GS=%zu PS=%zu "
                    "root=%p targets=%u formats=[%u,%u,%u,%u,%u,%u,%u,%u] "
                    "DSV=%u samples=%u topology=%u primitive=%u depth=%#x "
                    "stencil=%#x blend=%#x mask=%#x device=%#lx",
                    vs->dxil.size, hs ? hs->dxil.size : 0,
                    ds ? ds->dxil.size : 0, gs ? gs->dxil.size : 0,
                    ps ? ps->dxil.size : 0, d3d12->root_signature,
                    target_count, desc.RTVFormats[0], desc.RTVFormats[1],
                    desc.RTVFormats[2], desc.RTVFormats[3],
                    desc.RTVFormats[4], desc.RTVFormats[5],
                    desc.RTVFormats[6], desc.RTVFormats[7], desc.DSVFormat,
                    samples, desc.PrimitiveTopologyType, primitive,
                    depth_control, stencil_control, blend, color_mask,
                    (unsigned long)ID3D12Device_GetDeviceRemovedReason(
                        d3d12->device));
        if (!probes_reported) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC probe = desc;
            ID3D12PipelineState *probe_pso = NULL;
            HRESULT no_depth_hr;
            HRESULT vs_only_hr;

            probes_reported = true;
            probe.DepthStencilState.DepthEnable = FALSE;
            probe.DepthStencilState.StencilEnable = FALSE;
            probe.DSVFormat = DXGI_FORMAT_UNKNOWN;
            no_depth_hr = ID3D12Device_CreateGraphicsPipelineState(
                d3d12->device, &probe, &IID_ID3D12PipelineState,
                (void **)&probe_pso);
            if (probe_pso) {
                ID3D12PipelineState_Release(probe_pso);
                probe_pso = NULL;
            }
            probe.PS = (D3D12_SHADER_BYTECODE) { 0 };
            probe.NumRenderTargets = 0;
            memset(probe.RTVFormats, 0, sizeof(probe.RTVFormats));
            vs_only_hr = ID3D12Device_CreateGraphicsPipelineState(
                d3d12->device, &probe, &IID_ID3D12PipelineState,
                (void **)&probe_pso);
            if (probe_pso) {
                ID3D12PipelineState_Release(probe_pso);
            }
            warn_report("shadPS4 D3D12 PSO probes: full=%#lx "
                        "VS+PS-no-depth=%#lx VS-only=%#lx",
                        (unsigned long)hr, (unsigned long)no_depth_hr,
                        (unsigned long)vs_only_hr);
        }
        return NULL;
    }
    slot->key = key;
    slot->last_use = ++d3d12->pipeline_cache_clock;
    return slot;
}

static ShadPS4D3D12Pipeline *shadps4_d3d12_compute_pipeline(
    ShadPS4D3D12State *d3d12, ShadPS4D3D12Shader *cs)
{
    ShadPS4D3D12Pipeline *slot = NULL;

    for (uint32_t i = 0; i < SHADPS4_PIPELINE_CACHE_SIZE; i++) {
        ShadPS4D3D12Pipeline *pipeline = &d3d12->compute_pipelines[i];

        if (pipeline->pso && pipeline->key == cs->key) {
            pipeline->last_use = ++d3d12->pipeline_cache_clock;
            return pipeline;
        }
        if (!pipeline->pso || !slot || pipeline->last_use < slot->last_use) {
            slot = pipeline;
        }
    }
    if (slot->pso) {
        ID3D12PipelineState_Release(slot->pso);
        memset(slot, 0, sizeof(*slot));
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {
        .pRootSignature = d3d12->root_signature,
        .CS = { cs->dxil.data, cs->dxil.size },
    };
    HRESULT hr = ID3D12Device_CreateComputePipelineState(
        d3d12->device, &desc, &IID_ID3D12PipelineState,
        (void **)&slot->pso);

    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateComputePipelineState", hr);
        return NULL;
    }
    slot->key = cs->key;
    slot->last_use = ++d3d12->pipeline_cache_clock;
    return slot;
}

static void shadps4_d3d12_bind_constants(ShadPS4D3D12State *d3d12,
                                         bool compute, uint32_t page)
{
    D3D12_GPU_VIRTUAL_ADDRESS address =
        ID3D12Resource_GetGPUVirtualAddress(d3d12->constant_upload) +
        page * 512;

    if (compute) {
        ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(
            d3d12->list, 0, address);
        ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(
            d3d12->list, 1, address + 256);
    } else {
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
            d3d12->list, 0, address);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
            d3d12->list, 1, address + 256);
    }
}

static void shadps4_d3d12_add_shader_user_data(
    uint8_t *constant_page, const ShadPS4D3D12Shader *shader)
{
    uint32_t *push_user_data = (uint32_t *)(constant_page + 16);

    if (!shader || shader->user_data_binding_start >= 16) {
        return;
    }
    for (uint32_t i = 0; i < shader->user_data_value_count &&
                         shader->user_data_binding_start + i < 16; i++) {
        push_user_data[shader->user_data_binding_start + i] =
            shader->user_data_values[i];
    }
}

static void shadps4_d3d12_update_graphics_push_data(
    ShadPS4D3D12State *d3d12, const uint32_t *context,
    uint32_t context_count, const ShadPS4D3D12Shader *vs,
    const ShadPS4D3D12Shader *hs, const ShadPS4D3D12Shader *ds,
    const ShadPS4D3D12Shader *gs, const ShadPS4D3D12Shader *ps,
    uint32_t page)
{
    uint8_t *constant_page = d3d12->constant_map + page * 512;
    float *push = (float *)constant_page;
    uint32_t control = context_count > 0x206 ? context[0x206] : 0;

    memset(constant_page, 0, 128);
    push[2] = 1.0f;
    push[3] = 1.0f;
    if (context_count > 0x112) {
        if (control & (1u << 1)) {
            memcpy(&push[0], &context[0x110], sizeof(float));
        }
        if (control & (1u << 3)) {
            memcpy(&push[1], &context[0x112], sizeof(float));
        }
        if (control & (1u << 0)) {
            memcpy(&push[2], &context[0x10f], sizeof(float));
        }
        if (control & (1u << 2)) {
            memcpy(&push[3], &context[0x111], sizeof(float));
        }
    }
    shadps4_d3d12_add_shader_user_data(constant_page, vs);
    shadps4_d3d12_add_shader_user_data(constant_page, hs);
    shadps4_d3d12_add_shader_user_data(constant_page, ds);
    shadps4_d3d12_add_shader_user_data(constant_page, gs);
    shadps4_d3d12_add_shader_user_data(constant_page, ps);
}

static void shadps4_d3d12_update_compute_push_data(
    ShadPS4D3D12State *d3d12, const ShadPS4D3D12Shader *cs, uint32_t page)
{
    uint8_t *constant_page = d3d12->constant_map + page * 512;

    memset(constant_page, 0, 128);
    shadps4_d3d12_add_shader_user_data(constant_page, cs);
}

static DXGI_FORMAT shadps4_d3d12_resource_format(
    const ShadPS4GcnResource *resource)
{
    bool sint = resource->number_format == 5;
    bool uint = resource->number_format == 4;
    bool snorm = resource->number_format == 1 ||
                 resource->number_format == 6;
    bool srgb = resource->number_format == 9;
    bool fp = resource->number_format == 7;

    switch (resource->data_format) {
    case 1: return sint ? DXGI_FORMAT_R8_SINT : uint ? DXGI_FORMAT_R8_UINT :
                   snorm ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_UNORM;
    case 2: return fp ? DXGI_FORMAT_R16_FLOAT :
                   sint ? DXGI_FORMAT_R16_SINT : uint ? DXGI_FORMAT_R16_UINT :
                   snorm ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_UNORM;
    case 3: return sint ? DXGI_FORMAT_R8G8_SINT : uint ? DXGI_FORMAT_R8G8_UINT :
                   snorm ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM;
    case 4: return fp ? DXGI_FORMAT_R32_FLOAT :
                   sint ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
    case 5: return fp ? DXGI_FORMAT_R16G16_FLOAT :
                   sint ? DXGI_FORMAT_R16G16_SINT : uint ? DXGI_FORMAT_R16G16_UINT :
                   snorm ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
    case 6: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 8: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 9: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 10: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB :
                    sint ? DXGI_FORMAT_R8G8B8A8_SINT :
                    uint ? DXGI_FORMAT_R8G8B8A8_UINT :
                    snorm ? DXGI_FORMAT_R8G8B8A8_SNORM :
                    DXGI_FORMAT_R8G8B8A8_UNORM;
    case 11: return fp ? DXGI_FORMAT_R32G32_FLOAT :
                    sint ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32G32_UINT;
    case 12: return fp ? DXGI_FORMAT_R16G16B16A16_FLOAT :
                    sint ? DXGI_FORMAT_R16G16B16A16_SINT :
                    uint ? DXGI_FORMAT_R16G16B16A16_UINT :
                    snorm ? DXGI_FORMAT_R16G16B16A16_SNORM :
                    DXGI_FORMAT_R16G16B16A16_UNORM;
    case 13: return fp ? DXGI_FORMAT_R32G32B32_FLOAT :
                    sint ? DXGI_FORMAT_R32G32B32_SINT : DXGI_FORMAT_R32G32B32_UINT;
    case 14: return fp ? DXGI_FORMAT_R32G32B32A32_FLOAT :
                    sint ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_UINT;
    case 16: return DXGI_FORMAT_B5G6R5_UNORM;
    case 17: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 19: return DXGI_FORMAT_B4G4R4A4_UNORM;
    case 35: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case 36: return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    case 37: return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case 38: return snorm ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM;
    case 39: return snorm ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC5_UNORM;
    case 40: return snorm ? DXGI_FORMAT_BC6H_SF16 : DXGI_FORMAT_BC6H_UF16;
    case 41: return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static UINT shadps4_d3d12_component_mapping(uint32_t component)
{
    switch (component) {
    case 0: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    case 1: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    case 4: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case 5: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case 6: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case 7: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
    default: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    }
}

static D3D12_TEXTURE_ADDRESS_MODE shadps4_d3d12_address_mode(uint32_t clamp)
{
    switch (clamp) {
    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 2: case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: case 5: case 6: case 7: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

static D3D12_COMPARISON_FUNC shadps4_d3d12_compare_func(uint32_t compare)
{
    static const D3D12_COMPARISON_FUNC funcs[8] = {
        D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS,
    };
    return funcs[compare & 7];
}

static void shadps4_d3d12_bound_clear(ShadPS4D3D12BoundResource *bound)
{
    if (bound->upload) {
        if (bound->mapped) {
            ID3D12Resource_Unmap(bound->upload, 0, NULL);
        }
        ID3D12Resource_Release(bound->upload);
    } else if (bound->resource && bound->mapped) {
        ID3D12Resource_Unmap(bound->resource, 0, NULL);
    }
    if (bound->resource) {
        ID3D12Resource_Release(bound->resource);
    }
    if (bound->readback) {
        ID3D12Resource_Release(bound->readback);
    }
    g_free(bound->footprints);
    g_free(bound->footprint_rows);
    memset(bound, 0, sizeof(*bound));
}

static bool shadps4_d3d12_prepare_sampler(ShadPS4D3D12State *d3d12,
    const ShadPS4GcnResource *resource, uint32_t descriptor_page)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    bool anisotropic = resource->sampler_filter[0] >= 2 ||
                       resource->sampler_filter[1] >= 2;
    bool linear_min = resource->sampler_filter[0] == 1 ||
                      resource->sampler_filter[0] == 3;
    bool linear_mag = resource->sampler_filter[1] == 1 ||
                      resource->sampler_filter[1] == 3;
    bool linear_mip = resource->sampler_filter[2] == 2;
    D3D12_SAMPLER_DESC desc = {
        .Filter = anisotropic ? D3D12_FILTER_ANISOTROPIC :
            D3D12_ENCODE_BASIC_FILTER(linear_min ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                      linear_mag ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                      linear_mip ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                      D3D12_FILTER_REDUCTION_TYPE_STANDARD),
        .AddressU = shadps4_d3d12_address_mode(resource->sampler_clamp[0]),
        .AddressV = shadps4_d3d12_address_mode(resource->sampler_clamp[1]),
        .AddressW = shadps4_d3d12_address_mode(resource->sampler_clamp[2]),
        .MipLODBias = resource->sampler_lod_bias,
        .MaxAnisotropy = MAX(1, (UINT)resource->sampler_max_aniso),
        .ComparisonFunc = shadps4_d3d12_compare_func(resource->sampler_compare),
        .MinLOD = resource->sampler_min_lod,
        .MaxLOD = resource->sampler_max_lod,
    };
    if (resource->sampler_border_color == 1) {
        desc.BorderColor[3] = 1.0f;
    } else if (resource->sampler_border_color == 2) {
        desc.BorderColor[0] = desc.BorderColor[1] =
            desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
    }
    d3d12->sampler_heaps[descriptor_page]->lpVtbl->
        GetCPUDescriptorHandleForHeapStart(
            d3d12->sampler_heaps[descriptor_page], &handle);
    handle.ptr += resource->binding * d3d12->sampler_stride;
    ID3D12Device_CreateSampler(d3d12->device, &desc, handle);
    return true;
}

static uint32_t shadps4_d3d12_micro_pixel(
    const ShadPS4GcnResource *resource, uint32_t x, uint32_t y)
{
    uint32_t x0 = x & 1, x1 = x >> 1 & 1, x2 = x >> 2 & 1;
    uint32_t y0 = y & 1, y1 = y >> 1 & 1, y2 = y >> 2 & 1;

    if (resource->micro_tile_mode != 0) {
        return x0 | y0 << 1 | x1 << 2 | y1 << 3 | x2 << 4 | y2 << 5;
    }
    switch (resource->bits_per_element) {
    case 8: return x0 | x1 << 1 | x2 << 2 | y1 << 3 | y0 << 4 | y2 << 5;
    case 16: return x0 | x1 << 1 | x2 << 2 | y0 << 3 | y1 << 4 | y2 << 5;
    case 32: return x0 | x1 << 1 | y0 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    case 64: return x0 | y0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    default: return y0 | x0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    }
}

static uint32_t shadps4_d3d12_tiled_pipe(
    const ShadPS4GcnResource *resource, uint32_t x, uint32_t y)
{
    uint32_t tx = x / 8, ty = y / 8;
    uint32_t x3 = tx & 1, x4 = tx >> 1 & 1, x5 = tx >> 2 & 1;
    uint32_t y3 = ty & 1, y4 = ty >> 1 & 1, y5 = ty >> 2 & 1;

    if (resource->pipe_config == 0) {
        return x3 ^ y3;
    }
    if (resource->pipe_config == 10) {
        return (x4 ^ y3 ^ x5) | (x3 ^ y4) << 1 |
               (x5 ^ y5) << 2;
    }
    return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1 |
           (x5 ^ y5) << 2;
}

static uint32_t shadps4_d3d12_tiled_bank(
    const ShadPS4GcnResource *resource, uint32_t x, uint32_t y,
    uint32_t slice)
{
    uint32_t tx = x / 8 / (resource->bank_width *
        (resource->pipe_config == 0 ? 2 : 8));
    uint32_t ty = y / 8 / resource->bank_height;
    uint32_t x3 = tx & 1, x4 = tx >> 1 & 1;
    uint32_t x5 = tx >> 2 & 1, x6 = tx >> 3 & 1;
    uint32_t y3 = ty & 1, y4 = ty >> 1 & 1;
    uint32_t y5 = ty >> 2 & 1, y6 = ty >> 3 & 1;
    uint32_t bank;

    switch (resource->num_banks) {
    case 16:
        bank = (x3 ^ y6) | (x4 ^ y5 ^ y6) << 1 |
               (x5 ^ y4) << 2 | (x6 ^ y3) << 3;
        break;
    case 8:
        bank = (x3 ^ y5) | (x4 ^ y4 ^ y5) << 1 |
               (x5 ^ y3) << 2;
        break;
    case 4:
        bank = (x3 ^ y4) | (x4 ^ y3) << 1;
        break;
    default:
        bank = x3 ^ y3;
        break;
    }
    if (resource->array_mode == 4) {
        bank ^= (resource->num_banks / 2 - 1) * slice;
    }
    return (bank ^ resource->bank_swizzle) & (resource->num_banks - 1);
}

static uint64_t shadps4_d3d12_macro_tiled_offset(
    const ShadPS4GcnResource *resource, uint32_t x, uint32_t y,
    uint32_t slice)
{
    uint32_t bytes = resource->bits_per_element / 8;
    uint32_t pipes = resource->pipe_config == 0 ? 2 : 8;
    uint32_t pipe_bits = resource->pipe_config == 0 ? 1 : 3;
    uint32_t bank_bits = 0;
    uint32_t pixel = shadps4_d3d12_micro_pixel(resource, x, y);
    uint32_t micro_bytes = 64 * bytes;
    uint32_t element = pixel * bytes;
    uint32_t macro_pitch = 8 * resource->bank_width * pipes *
                           resource->macro_tile_aspect;
    uint32_t macro_height = 8 * resource->bank_height *
                            resource->num_banks /
                            resource->macro_tile_aspect;
    uint32_t pitch = ROUND_UP(resource->pitch, macro_pitch);
    uint32_t height = ROUND_UP(resource->height, macro_height);
    uint32_t macro_bytes = micro_bytes * (macro_pitch / 8) *
                           (macro_height / 8) /
                           (pipes * resource->num_banks);
    uint32_t macro_offset = ((y / macro_height) *
        (pitch / macro_pitch) + x / macro_pitch) * macro_bytes;
    uint32_t slice_bytes = (pitch / macro_pitch) *
                           (height / macro_height) * macro_bytes;
    uint32_t tile_row = (y / 8) % resource->bank_height;
    uint32_t tile_col = ((x / 8) / pipes) % resource->bank_width;
    uint32_t total = macro_offset + element +
                     (tile_row * resource->bank_width + tile_col) *
                     micro_bytes;
    uint32_t pipe = shadps4_d3d12_tiled_pipe(resource, x, y);
    uint32_t bank = shadps4_d3d12_tiled_bank(resource, x, y, slice);

    (void)slice_bytes;
    while ((1u << bank_bits) < resource->num_banks) {
        bank_bits++;
    }
    return (total & 0xff) | (uint64_t)pipe << 8 |
           (uint64_t)bank << (8 + pipe_bits) |
           (uint64_t)(total >> 8) << (8 + pipe_bits + bank_bits);
}

static uint64_t shadps4_d3d12_image_element_offset(
    const ShadPS4GcnResource *resource, uint32_t x, uint32_t y,
    uint32_t slice)
{
    uint32_t bytes = resource->bits_per_element / 8;
    bool macro_tiled = resource->array_mode == 4 &&
        resource->micro_tile_thickness == 1 && resource->bank_width &&
        resource->bank_height && resource->num_banks &&
        resource->macro_tile_aspect;
    bool micro_tiled = resource->tiling_mode == 5 ||
        resource->tiling_mode == 9 || resource->tiling_mode == 13;

    if (macro_tiled) {
        return shadps4_d3d12_macro_tiled_offset(resource, x, y, slice);
    }
    if (micro_tiled) {
        uint32_t pixel = shadps4_d3d12_micro_pixel(resource, x, y);
        uint64_t tile = ((uint64_t)(y / 8) *
            (ROUND_UP(resource->pitch, 8) / 8) + x / 8) * 64 * bytes;

        return tile + (uint64_t)pixel * bytes;
    }
    return ((uint64_t)y * resource->pitch + x) * bytes;
}

static void shadps4_d3d12_image_view_desc(
    const ShadPS4GcnResource *resource, D3D12_RESOURCE_DESC *desc)
{
    uint32_t levels = MIN(MAX(resource->levels, 1), 16);
    uint32_t layers = MAX(resource->layers, 1);

    memset(desc, 0, sizeof(*desc));
    desc->Dimension = resource->image_type == 8 || resource->image_type == 12 ?
        D3D12_RESOURCE_DIMENSION_TEXTURE1D : resource->image_type == 10 ?
        D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc->Width = MAX(resource->width, 1);
    desc->Height = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ?
        1 : MAX(resource->height, 1);
    desc->DepthOrArraySize = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
        MIN(MAX(resource->depth, 1), UINT16_MAX) : MIN(layers, UINT16_MAX);
    desc->MipLevels = levels;
    desc->Format = shadps4_d3d12_resource_format(resource);
    desc->SampleDesc.Count = 1;
    desc->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc->Flags = resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN ?
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
}

static bool shadps4_d3d12_prepare_linear_image(
    ShadPS4D3D12State *d3d12, CPUState *cs,
    const ShadPS4GcnResource *resource, ShadPS4D3D12BoundResource *bound,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_HEAP_PROPERTIES default_heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_HEAP_PROPERTIES upload_heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC texture_desc;
    D3D12_RESOURCE_DESC upload_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    D3D12_RANGE read_range = { 0, 0 };
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    UINT64 upload_size = 0;
    g_autofree uint8_t *guest_pixels = NULL;
    uint64_t guest_size;
    uint32_t levels = MIN(MAX(resource->levels, 1), 16);
    uint32_t layers = MAX(resource->layers, 1);
    uint32_t subresources;
    HRESULT hr;

    shadps4_d3d12_image_view_desc(resource, &texture_desc);
    subresources = texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
        levels : levels * layers;
    if (texture_desc.Format == DXGI_FORMAT_UNKNOWN || !resource->width ||
        !resource->height || !resource->bits_per_element || !subresources ||
        subresources > D3D12_REQ_SUBRESOURCES || resource->samples > 1 ||
        ((resource->flags & SHADPS4_GCN_RESOURCE_COMPRESSED) &&
         (resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN))) {
        return false;
    }
    guest_size = resource->guest_size;
    if (!guest_size || guest_size > UINT64_C(512) * 1024 * 1024) {
        return false;
    }
    if (!bound->resource) {
        shadps4_d3d12_bound_clear(bound);
        hr = ID3D12Device_CreateCommittedResource(
            d3d12->device, &default_heap, D3D12_HEAP_FLAG_NONE,
            &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&bound->resource);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Create guest image", hr);
            return false;
        }
        bound->footprints = g_new0(D3D12_PLACED_SUBRESOURCE_FOOTPRINT,
                                   subresources);
        bound->footprint_rows = g_new0(UINT, subresources);
        ID3D12Device_GetCopyableFootprints(d3d12->device, &texture_desc,
            0, subresources, 0, bound->footprints, bound->footprint_rows,
            NULL, &upload_size);
        upload_desc.Width = upload_size;
        hr = ID3D12Device_CreateCommittedResource(
            d3d12->device, &upload_heap, D3D12_HEAP_FLAG_NONE,
            &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&bound->upload);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Create guest image upload", hr);
            shadps4_d3d12_bound_clear(bound);
            return false;
        }
        hr = ID3D12Resource_Map(bound->upload, 0, &read_range,
                                (void **)&bound->mapped);
        if (FAILED(hr)) {
            shadps4_d3d12_bound_clear(bound);
            return false;
        }
        bound->size = guest_size;
        bound->upload_size = upload_size;
        bound->subresource_count = subresources;
        bound->state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN) {
            D3D12_HEAP_PROPERTIES readback_heap = upload_heap;

            readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
            hr = ID3D12Device_CreateCommittedResource(
                d3d12->device, &readback_heap, D3D12_HEAP_FLAG_NONE,
                &upload_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void **)&bound->readback);
            if (FAILED(hr)) {
                shadps4_d3d12_bound_clear(bound);
                return false;
            }
        }
    }
    guest_pixels = g_malloc(guest_size);
    if (cpu_memory_rw_debug(cs, resource->guest_address,
                            guest_pixels, guest_size, false) != 0) {
        return false;
    }
    memset(bound->mapped, 0, bound->upload_size);
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t mip = 0; mip < levels; mip++) {
            ShadPS4GcnResource layout = *resource;
            uint32_t subresource = texture_desc.Dimension ==
                D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip : mip + layer * levels;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp =
                &bound->footprints[subresource];
            uint32_t width = MAX(resource->width >> mip, 1);
            uint32_t height = MAX(resource->height >> mip, 1);
            uint32_t depth = texture_desc.Dimension ==
                D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
                MAX(resource->depth >> mip, 1) : 1;
            uint32_t copy_width = resource->flags & SHADPS4_GCN_RESOURCE_COMPRESSED ?
                MAX((width + 3) / 4, 1) : width;
            uint32_t copy_height = resource->flags & SHADPS4_GCN_RESOURCE_COMPRESSED ?
                MAX((height + 3) / 4, 1) : height;
            uint32_t bytes = resource->bits_per_element / 8;
            uint64_t layer_size = resource->mip_size[mip] / layers;
            uint64_t slice_size = layer_size / depth;
            uint64_t base = resource->mip_offset[mip] + layer * layer_size;

            layout.pitch = resource->mip_pitch[mip] ?:
                MAX(resource->pitch >> mip, 1);
            layout.height = resource->mip_height[mip] ?: copy_height;
            for (uint32_t z = 0; z < depth; z++) {
                uint8_t *dest_slice = bound->mapped + fp->Offset +
                    (uint64_t)z * fp->Footprint.RowPitch *
                    bound->footprint_rows[subresource];

                for (uint32_t y = 0; y < copy_height; y++) {
                    uint8_t *dest = dest_slice +
                        (uint64_t)y * fp->Footprint.RowPitch;

                    for (uint32_t x = 0; x < copy_width; x++) {
                        uint64_t source = base + z * slice_size +
                            shadps4_d3d12_image_element_offset(
                                &layout, x, y,
                                texture_desc.Dimension ==
                                D3D12_RESOURCE_DIMENSION_TEXTURE3D ? z : layer);

                        if (source > guest_size - bytes) {
                            return false;
                        }
                        memcpy(dest + (uint64_t)x * bytes,
                               guest_pixels + source, bytes);
                    }
                }
            }
        }
    }
    if (bound->state != D3D12_RESOURCE_STATE_COPY_DEST) {
        barrier = (D3D12_RESOURCE_BARRIER) {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = bound->resource,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = bound->state,
                .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST,
            },
        };
        ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
    }
    for (uint32_t i = 0; i < subresources; i++) {
        D3D12_TEXTURE_COPY_LOCATION source_location = {
            .pResource = bound->upload,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = bound->footprints[i],
        };
        D3D12_TEXTURE_COPY_LOCATION dest_location = {
            .pResource = bound->resource,
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = i,
        };

        ID3D12GraphicsCommandList_CopyTextureRegion(
            d3d12->list, &dest_location, 0, 0, 0, &source_location, NULL);
    }
    bound->state = resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN ?
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier = (D3D12_RESOURCE_BARRIER) {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = bound->resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
            .StateAfter = bound->state,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
    if (resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { .Format = texture_desc.Format };

        if (texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            uav.ViewDimension = layers > 1 ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY :
                D3D12_UAV_DIMENSION_TEXTURE1D;
            uav.Texture1DArray.ArraySize = layers;
        } else if (texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav.Texture3D.WSize = texture_desc.DepthOrArraySize;
        } else {
            uav.ViewDimension = layers > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY :
                D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2DArray.ArraySize = layers;
        }
        ID3D12Device_CreateUnorderedAccessView(
            d3d12->device, bound->resource, NULL, &uav, handle);
        if (!shadps4_d3d12_queue_writeback(d3d12, bound,
                resource->guest_address, resource->guest_size)) {
            return false;
        }
        for (uint32_t i = 0; i < d3d12->writeback_count; i++) {
            if (d3d12->writebacks[i].bound == bound) {
                d3d12->writebacks[i].is_image = true;
                d3d12->writebacks[i].image = *resource;
                break;
            }
        }
        return true;
    }
    memset(&srv, 0, sizeof(srv));
    srv.Format = texture_desc.Format;
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        shadps4_d3d12_component_mapping(resource->component_swizzle[0]),
        shadps4_d3d12_component_mapping(resource->component_swizzle[1]),
        shadps4_d3d12_component_mapping(resource->component_swizzle[2]),
        shadps4_d3d12_component_mapping(resource->component_swizzle[3]));
    if (texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        layers == 1) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        srv.Texture1D.MipLevels = levels;
    } else if (texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        srv.Texture1DArray.MipLevels = levels;
        srv.Texture1DArray.ArraySize = layers;
    } else if (texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = levels;
    } else if (resource->flags & SHADPS4_GCN_RESOURCE_CUBE) {
        srv.ViewDimension = layers > 6 ? D3D12_SRV_DIMENSION_TEXTURECUBEARRAY :
            D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = levels;
        if (layers > 6) {
            srv.TextureCubeArray.NumCubes = layers / 6;
        }
    } else if (layers > 1) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = levels;
        srv.Texture2DArray.ArraySize = layers;
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = levels;
    }
    ID3D12Device_CreateShaderResourceView(
        d3d12->device, bound->resource, &srv, handle);
    return true;
}

static bool shadps4_d3d12_prepare_surface_image(
    ShadPS4D3D12State *d3d12, const ShadPS4GcnResource *resource,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    int index = shadps4_d3d12_find_surface_by_address(
        d3d12, resource->guest_address);
    ShadPS4D3D12Surface *surface;

    if (index < 0 && resource->image_type == 9 &&
        !(resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN)) {
        for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_DEPTH_SURFACES; i++) {
            ShadPS4D3D12DepthSurface *depth = &d3d12->depth_surfaces[i];

            if (depth->texture &&
                (depth->guest_address & ~UINT64_C(0xff)) ==
                (resource->guest_address & ~UINT64_C(0xff))) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view = {
                    .Format = depth->srv_format,
                    .ViewDimension = depth->samples > 1 ?
                        D3D12_SRV_DIMENSION_TEXTURE2DMS :
                        D3D12_SRV_DIMENSION_TEXTURE2D,
                    .Shader4ComponentMapping =
                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                };

                if (depth->samples == 1) {
                    view.Texture2D.MipLevels = 1;
                }
                shadps4_d3d12_transition_depth_surface(d3d12, depth,
                    D3D12_RESOURCE_STATE_DEPTH_READ |
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ID3D12Device_CreateShaderResourceView(
                    d3d12->device, depth->texture, &view, handle);
                return true;
            }
        }
    }
    if (index < 0 || resource->image_type != 9) {
        return false;
    }
    surface = &d3d12->surfaces[index];
    surface->last_use = ++d3d12->surface_clock;
    if (resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {
            .Format = surface->dxgi_format,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        };

        if (surface->video_out || surface->samples > 1) {
            return false;
        }
        shadps4_d3d12_transition_surface(
            d3d12, surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Device_CreateUnorderedAccessView(
            d3d12->device, surface->texture, NULL, &view, handle);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {
            .Format = surface->dxgi_format,
            .ViewDimension = surface->samples > 1 ?
                D3D12_SRV_DIMENSION_TEXTURE2DMS :
                D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping =
                D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                    shadps4_d3d12_component_mapping(
                        resource->component_swizzle[0]),
                    shadps4_d3d12_component_mapping(
                        resource->component_swizzle[1]),
                    shadps4_d3d12_component_mapping(
                        resource->component_swizzle[2]),
                    shadps4_d3d12_component_mapping(
                        resource->component_swizzle[3])),
        };

        if (surface->samples == 1) {
            view.Texture2D.MipLevels = 1;
        }

        shadps4_d3d12_transition_surface(d3d12, surface,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12Device_CreateShaderResourceView(
            d3d12->device, surface->texture, &view, handle);
    }
    return true;
}

static bool shadps4_d3d12_queue_writeback(ShadPS4D3D12State *d3d12,
    ShadPS4D3D12BoundResource *bound, uint64_t guest_address, uint64_t size)
{
    for (uint32_t i = 0; i < d3d12->writeback_count; i++) {
        if (d3d12->writebacks[i].bound == bound) {
            return true;
        }
    }
    if (d3d12->writeback_count >= SHADPS4_MAX_WRITEBACKS) {
        return false;
    }
    d3d12->writebacks[d3d12->writeback_count++] =
        (ShadPS4D3D12Writeback) {
            .bound = bound,
            .guest_address = guest_address,
            .size = size,
        };
    return true;
}

static bool shadps4_d3d12_prepare_writable_buffer(
    ShadPS4D3D12State *d3d12, CPUState *cs,
    const ShadPS4GcnResource *resource, ShadPS4D3D12BoundResource *bound,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_HEAP_PROPERTIES default_heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_HEAP_PROPERTIES upload_heap = default_heap;
    D3D12_HEAP_PROPERTIES readback_heap = default_heap;
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = ROUND_UP(resource->guest_size, 4),
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    D3D12_RANGE read_range = { 0, 0 };
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_UNORDERED_ACCESS_VIEW_DESC view = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = {
            .NumElements = ROUND_UP(resource->guest_size, 4) / 4,
            .Flags = D3D12_BUFFER_UAV_FLAG_RAW,
        },
    };
    HRESULT hr;

    if (!resource->guest_size ||
        resource->guest_size > UINT64_C(256) * 1024 * 1024) {
        return false;
    }
    if (!bound->resource || bound->size != resource->guest_size) {
        shadps4_d3d12_bound_clear(bound);
        hr = ID3D12Device_CreateCommittedResource(
            d3d12->device, &default_heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
            (void **)&bound->resource);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Create writable guest buffer", hr);
            return false;
        }
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = ID3D12Device_CreateCommittedResource(
            d3d12->device, &upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
            (void **)&bound->upload);
        if (FAILED(hr)) {
            shadps4_d3d12_bound_clear(bound);
            return false;
        }
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        hr = ID3D12Device_CreateCommittedResource(
            d3d12->device, &readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
            (void **)&bound->readback);
        if (FAILED(hr)) {
            shadps4_d3d12_bound_clear(bound);
            return false;
        }
        hr = ID3D12Resource_Map(bound->upload, 0, &read_range,
                                (void **)&bound->mapped);
        if (FAILED(hr)) {
            shadps4_d3d12_bound_clear(bound);
            return false;
        }
        bound->size = resource->guest_size;
        bound->state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (cpu_memory_rw_debug(cs, resource->guest_address, bound->mapped,
                            resource->guest_size, false) != 0) {
        return false;
    }
    if (bound->state != D3D12_RESOURCE_STATE_COPY_DEST) {
        barrier = (D3D12_RESOURCE_BARRIER) {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = bound->resource,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = bound->state,
                .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST,
            },
        };
        ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
    }
    ID3D12GraphicsCommandList_CopyBufferRegion(
        d3d12->list, bound->resource, 0, bound->upload, 0,
        resource->guest_size);
    barrier = (D3D12_RESOURCE_BARRIER) {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = bound->resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
            .StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
    bound->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Device_CreateUnorderedAccessView(
        d3d12->device, bound->resource, NULL, &view, handle);
    return shadps4_d3d12_queue_writeback(
        d3d12, bound, resource->guest_address, resource->guest_size);
}

static bool shadps4_d3d12_prepare_shader_resources(
    ShadPS4D3D12State *d3d12, CPUState *cs, ShadPS4D3D12Shader *shader,
    uint32_t descriptor_page)
{
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start;

    d3d12->resource_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        d3d12->resource_heap, &heap_start);
    for (size_t i = 0; i < shader->resource_count; i++) {
        const ShadPS4GcnResource *resource = &shader->resources[i];
        ShadPS4D3D12BoundResource *bound = &shader->bound_resources[i];
        const void *source = NULL;
        uint64_t size = resource->guest_size;

        if (resource->binding >= 256 || resource->binding_count != 1) {
            return false;
        }
        if (resource->type == SHADPS4_GCN_RESOURCE_SAMPLER) {
            if (!shadps4_d3d12_prepare_sampler(
                    d3d12, resource, descriptor_page)) {
                return false;
            }
            continue;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_start;

        handle.ptr += (descriptor_page * 512 +
            (resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN ? 256 : 0) +
            resource->binding) * d3d12->resource_stride;
        if (resource->type == SHADPS4_GCN_RESOURCE_IMAGE) {
            if (!shadps4_d3d12_prepare_surface_image(
                    d3d12, resource, handle) &&
                !shadps4_d3d12_prepare_linear_image(
                    d3d12, cs, resource, bound, handle)) {
                return false;
            }
            continue;
        }
        if (resource->type == SHADPS4_GCN_RESOURCE_BUFFER &&
            resource->flags & SHADPS4_GCN_RESOURCE_WRITTEN) {
            if (!shadps4_d3d12_prepare_writable_buffer(
                    d3d12, cs, resource, bound, handle)) {
                return false;
            }
            continue;
        }
        if (resource->type == SHADPS4_GCN_RESOURCE_FLAT_BUFFER) {
            source = shader->flat_user_data;
            size = shader->flat_user_data_words * sizeof(uint32_t);
        } else if (resource->type != SHADPS4_GCN_RESOURCE_BUFFER) {
            return false;
        }
        if (!size || size > UINT64_C(256) * 1024 * 1024) {
            return false;
        }
        if (!bound->resource || bound->size != size) {
            D3D12_HEAP_PROPERTIES heap = {
                .Type = D3D12_HEAP_TYPE_UPLOAD,
                .CreationNodeMask = 1,
                .VisibleNodeMask = 1,
            };
            D3D12_RESOURCE_DESC desc = {
                .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                .Width = ROUND_UP(size, 4),
                .Height = 1,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .SampleDesc = { .Count = 1 },
                .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            };
            D3D12_RANGE read_range = { 0, 0 };
            HRESULT hr;

            shadps4_d3d12_bound_clear(bound);
            hr = ID3D12Device_CreateCommittedResource(
                d3d12->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void **)&bound->resource);
            if (FAILED(hr)) {
                shadps4_d3d12_report("Create guest buffer", hr);
                return false;
            }
            hr = ID3D12Resource_Map(bound->resource, 0, &read_range,
                                    (void **)&bound->mapped);
            if (FAILED(hr)) {
                return false;
            }
            bound->size = size;
        }
        if (source) {
            memcpy(bound->mapped, source, size);
        } else if (cpu_memory_rw_debug(cs, resource->guest_address,
                                       bound->mapped, size, false) != 0) {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {
            .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {
                .NumElements = ROUND_UP(size, 4) / 4,
                .Flags = D3D12_BUFFER_SRV_FLAG_RAW,
            },
        };
        ID3D12Device_CreateShaderResourceView(
            d3d12->device, bound->resource, &view, handle);
    }
    return true;
}

static void shadps4_d3d12_bind_descriptor_heaps(
    ShadPS4D3D12State *d3d12, bool compute, uint32_t descriptor_page)
{
    ID3D12DescriptorHeap *heaps[] = {
        d3d12->resource_heap,
        d3d12->sampler_heaps[descriptor_page],
    };
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    D3D12_GPU_DESCRIPTOR_HANDLE uav;
    D3D12_GPU_DESCRIPTOR_HANDLE sampler;

    d3d12->resource_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        d3d12->resource_heap, &srv);
    srv.ptr += descriptor_page * 512 * d3d12->resource_stride;
    uav = srv;
    uav.ptr += 256 * d3d12->resource_stride;
    d3d12->sampler_heaps[descriptor_page]->lpVtbl->
        GetGPUDescriptorHandleForHeapStart(
            d3d12->sampler_heaps[descriptor_page], &sampler);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(
        d3d12->list, ARRAY_SIZE(heaps), heaps);
    if (compute) {
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
            d3d12->list, 2, srv);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
            d3d12->list, 3, uav);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
            d3d12->list, 4, sampler);
    } else {
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
            d3d12->list, 2, srv);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
            d3d12->list, 3, uav);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
            d3d12->list, 4, sampler);
    }
}

static bool shadps4_d3d12_upload_index_buffer(
    ShadPS4D3D12State *d3d12, CPUState *cs, uint64_t guest_address,
    uint32_t index_count, uint32_t index_type, D3D12_INDEX_BUFFER_VIEW *view)
{
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    uint32_t index_size = index_type == 1 ? 4 : 2;
    uint64_t size = (uint64_t)index_count * index_size;
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = ROUND_UP(size, 4),
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    D3D12_RANGE read_range = { 0, 0 };
    ID3D12Resource *buffer = NULL;
    void *mapped = NULL;
    HRESULT hr;

    if (!guest_address || !index_count || index_type > 1 ||
        size > 64 * 1024 * 1024 ||
        d3d12->transient_count >= SHADPS4_MAX_TRANSIENTS) {
        return false;
    }
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
        (void **)&buffer);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create index upload", hr);
        return false;
    }
    hr = ID3D12Resource_Map(buffer, 0, &read_range, &mapped);
    if (FAILED(hr) || cpu_memory_rw_debug(
            cs, guest_address, mapped, size, false) != 0) {
        if (SUCCEEDED(hr)) {
            ID3D12Resource_Unmap(buffer, 0, NULL);
        }
        ID3D12Resource_Release(buffer);
        return false;
    }
    ID3D12Resource_Unmap(buffer, 0, NULL);
    d3d12->transients[d3d12->transient_count++] = buffer;
    *view = (D3D12_INDEX_BUFFER_VIEW) {
        .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffer),
        .SizeInBytes = size,
        .Format = index_type == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
    };
    return true;
}

static bool shadps4_d3d12_upload_vertex_buffers(
    ShadPS4D3D12State *d3d12, CPUState *cs,
    const ShadPS4D3D12Shader *vs)
{
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    for (uint32_t i = 0; i < vs->vertex_input_count; i++) {
        const ShadPS4GcnVertexInput *input = &vs->vertex_inputs[i];
        uint64_t size = input->guest_size;
        D3D12_RESOURCE_DESC desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = ROUND_UP(size, 4),
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        D3D12_RANGE read_range = { 0, 0 };
        D3D12_VERTEX_BUFFER_VIEW view;
        ID3D12Resource *buffer = NULL;
        void *mapped = NULL;
        HRESULT hr;

        if (!input->guest_address || !size || size > 64 * 1024 * 1024 ||
            !input->stride || size > UINT32_MAX ||
            d3d12->transient_count >= SHADPS4_MAX_TRANSIENTS) {
            return false;
        }
        hr = ID3D12Device_CreateCommittedResource(
            d3d12->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
            (void **)&buffer);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Create vertex upload", hr);
            return false;
        }
        hr = ID3D12Resource_Map(buffer, 0, &read_range, &mapped);
        if (FAILED(hr) || cpu_memory_rw_debug(
                cs, input->guest_address, mapped, size, false) != 0) {
            if (SUCCEEDED(hr)) {
                ID3D12Resource_Unmap(buffer, 0, NULL);
            }
            ID3D12Resource_Release(buffer);
            return false;
        }
        ID3D12Resource_Unmap(buffer, 0, NULL);
        d3d12->transients[d3d12->transient_count++] = buffer;
        view = (D3D12_VERTEX_BUFFER_VIEW) {
            .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffer),
            .SizeInBytes = size,
            .StrideInBytes = input->stride,
        };
        ID3D12GraphicsCommandList_IASetVertexBuffers(
            d3d12->list, input->semantic, 1, &view);
    }
    return true;
}

static bool shadps4_d3d12_record_draw(ShadPS4D3D12State *d3d12,
                                      CPUState *cs,
                                      ShadPS4D3D12Shader *vs,
                                      ShadPS4D3D12Shader *hs,
                                      ShadPS4D3D12Shader *ds,
                                      ShadPS4D3D12Shader *gs,
                                      ShadPS4D3D12Shader *ps,
                                      const uint32_t *context,
                                      uint32_t context_count,
                                      const int *targets,
                                      uint32_t target_count, uint32_t opcode,
                                      const uint32_t *payload,
                                      uint32_t payload_count,
                                      uint64_t index_base,
                                      uint32_t index_type,
                                      uint32_t num_instances,
                                      uint64_t indirect_base,
                                      const char **failure_reason)
{
    ShadPS4D3D12Pipeline *pipeline;
    ShadPS4D3D12Surface *surface;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    uint32_t vertex_count;
    uint32_t first_index = 0;
    uint32_t first_vertex = 0;
    uint32_t first_instance = 0;
    int32_t base_vertex = 0;
    uint32_t descriptor_page;
    bool indexed = false;
    int depth_target;
    uint32_t target_width;
    uint32_t target_height;
    D3D12_INDEX_BUFFER_VIEW index_view;

    *failure_reason = "unknown draw failure";
    depth_target = shadps4_d3d12_find_depth_target(
        d3d12, context, context_count);
    if (!vs) {
        *failure_reason = "missing vertex shader";
        return false;
    }
    if (target_count && !ps) {
        *failure_reason = "missing pixel shader";
        return false;
    }
    if (!target_count && depth_target < 0) {
        *failure_reason = "no color or depth render target";
        return false;
    }
    if (target_count > 8 || !payload_count) {
        *failure_reason = "invalid target or payload count";
        return false;
    }
    for (uint32_t i = 1; i < target_count; i++) {
        if (d3d12->surfaces[targets[i]].samples !=
            d3d12->surfaces[targets[0]].samples) {
            *failure_reason = "MRT sample-count mismatch";
            return false;
        }
    }
    if (depth_target >= 0 && target_count &&
        d3d12->depth_surfaces[depth_target].samples !=
        d3d12->surfaces[targets[0]].samples) {
        *failure_reason = "depth/color sample-count mismatch";
        return false;
    }
    if (depth_target >= 0) {
        shadps4_d3d12_transition_depth_surface(d3d12,
            &d3d12->depth_surfaces[depth_target],
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    if (d3d12->descriptor_page >= SHADPS4_DESCRIPTOR_PAGES) {
        *failure_reason = "descriptor pages exhausted";
        return false;
    }
    descriptor_page = d3d12->descriptor_page++;
    if (!shadps4_d3d12_prepare_shader_resources(
            d3d12, cs, vs, descriptor_page) ||
        (hs && !shadps4_d3d12_prepare_shader_resources(
            d3d12, cs, hs, descriptor_page)) ||
        (ds && !shadps4_d3d12_prepare_shader_resources(
            d3d12, cs, ds, descriptor_page)) ||
        (gs && !shadps4_d3d12_prepare_shader_resources(
            d3d12, cs, gs, descriptor_page)) ||
        (ps && !shadps4_d3d12_prepare_shader_resources(
            d3d12, cs, ps, descriptor_page))) {
        *failure_reason = "shader resource preparation failed";
        return false;
    }
    pipeline = shadps4_d3d12_graphics_pipeline(
        d3d12, vs, hs, ds, gs, ps, context, context_count, targets, target_count,
        depth_target);
    if (!pipeline) {
        *failure_reason = "graphics pipeline creation failed";
        return false;
    }
    switch (opcode) {
    case SHADPS4_PM4_DRAW_INDEX_AUTO:
        vertex_count = le32_to_cpu(payload[0]);
        break;
    case SHADPS4_PM4_DRAW_INDEX2:
        if (payload_count < 5) {
            *failure_reason = "truncated DRAW_INDEX2 packet";
            return false;
        }
        index_base = (le32_to_cpu(payload[1]) & ~UINT64_C(1)) |
                     ((uint64_t)(le32_to_cpu(payload[2]) & 0xff) << 32);
        vertex_count = le32_to_cpu(payload[3]);
        indexed = true;
        break;
    case SHADPS4_PM4_DRAW_INDEX_OFFSET2:
        if (payload_count < 4) {
            *failure_reason = "truncated DRAW_INDEX_OFFSET2 packet";
            return false;
        }
        first_index = le32_to_cpu(payload[1]);
        vertex_count = le32_to_cpu(payload[2]);
        indexed = true;
        break;
    case SHADPS4_PM4_DRAW_INDIRECT: {
        uint32_t args[4];

        if (payload_count < 4 || !indirect_base ||
            cpu_memory_rw_debug(cs, indirect_base + le32_to_cpu(payload[0]),
                                args, sizeof(args), false) != 0) {
            *failure_reason = "invalid indirect draw arguments";
            return false;
        }
        vertex_count = le32_to_cpu(args[0]);
        num_instances = le32_to_cpu(args[1]);
        first_vertex = le32_to_cpu(args[2]);
        first_instance = le32_to_cpu(args[3]);
        break;
    }
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT: {
        uint32_t args[5];

        if (payload_count < 4 || !indirect_base ||
            cpu_memory_rw_debug(cs, indirect_base + le32_to_cpu(payload[0]),
                                args, sizeof(args), false) != 0) {
            *failure_reason = "invalid indexed indirect draw arguments";
            return false;
        }
        vertex_count = le32_to_cpu(args[0]);
        num_instances = le32_to_cpu(args[1]);
        first_index = le32_to_cpu(args[2]);
        base_vertex = le32_to_cpu(args[3]);
        first_instance = le32_to_cpu(args[4]);
        indexed = true;
        break;
    }
    default:
        *failure_reason = "unsupported draw packet";
        return false;
    }
    if (!vertex_count) {
        return true;
    }
    num_instances = MAX(num_instances, 1);
    if (!shadps4_d3d12_upload_vertex_buffers(d3d12, cs, vs)) {
        *failure_reason = "vertex buffer upload failed";
        return false;
    }
    if (indexed && !shadps4_d3d12_upload_index_buffer(
            d3d12, cs, index_base, vertex_count + first_index,
            index_type, &index_view)) {
        *failure_reason = "index buffer upload failed";
        return false;
    }
    surface = target_count ? &d3d12->surfaces[targets[0]] : NULL;
    target_width = surface ? surface->width :
        d3d12->depth_surfaces[depth_target].width;
    target_height = surface ? surface->height :
        d3d12->depth_surfaces[depth_target].height;
    for (uint32_t i = 0; i < target_count; i++) {
        ShadPS4D3D12Surface *target = &d3d12->surfaces[targets[i]];

        shadps4_d3d12_transition_surface(
            d3d12, target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d3d12->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            d3d12->rtv_heap, &rtvs[i]);
        rtvs[i].ptr += (SIZE_T)targets[i] * d3d12->rtv_stride;
    }
    viewport = (D3D12_VIEWPORT) {
        .Width = target_width,
        .Height = target_height,
        .MaxDepth = 1.0f,
    };
    scissor = (D3D12_RECT) {
        .right = target_width,
        .bottom = target_height,
    };
    if (context_count > 0x114) {
        float values[6];
        uint32_t control = context[0x206];

        memcpy(values, &context[0x10f], sizeof(values));
        if (values[0] != 0.0f) {
            float xscale = control & 1 ? values[0] : 1.0f;
            float xoffset = control & 2 ? values[1] : 0.0f;
            float yscale = control & 4 ? values[2] : 1.0f;
            float yoffset = control & 8 ? values[3] : 0.0f;
            float zscale = control & 16 ? values[4] : 1.0f;
            float zoffset = control & 32 ? values[5] : 0.0f;

            viewport.TopLeftX = xoffset - fabsf(xscale);
            viewport.TopLeftY = yoffset - fabsf(yscale);
            viewport.Width = MIN(fabsf(xscale) * 2.0f, target_width);
            viewport.Height = MIN(fabsf(yscale) * 2.0f, target_height);
            viewport.MinDepth = MAX(0.0f, zoffset - zscale);
            viewport.MaxDepth = MIN(1.0f, zoffset + zscale);
        }
    }
    if (context_count > 0x91) {
        uint32_t tl = context[0x90];
        uint32_t br = context[0x91];
        LONG left = tl & 0x7fff;
        LONG top = (tl >> 16) & 0x7fff;
        LONG right = br & 0x7fff;
        LONG bottom = (br >> 16) & 0x7fff;

        if (right > left && bottom > top) {
            scissor.left = MIN(left, (LONG)target_width);
            scissor.top = MIN(top, (LONG)target_height);
            scissor.right = MIN(right, (LONG)target_width);
            scissor.bottom = MIN(bottom, (LONG)target_height);
        }
    }
    ID3D12GraphicsCommandList_SetPipelineState(d3d12->list, pipeline->pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(
        d3d12->list, d3d12->root_signature);
    shadps4_d3d12_update_graphics_push_data(
        d3d12, context, context_count, vs, hs, ds, gs, ps, descriptor_page);
    shadps4_d3d12_bind_constants(d3d12, false, descriptor_page);
    shadps4_d3d12_bind_descriptor_heaps(d3d12, false, descriptor_page);
    ID3D12GraphicsCommandList_RSSetViewports(d3d12->list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(d3d12->list, 1, &scissor);
    if (depth_target >= 0) {
        d3d12->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            d3d12->dsv_heap, &dsv);
        dsv.ptr += (SIZE_T)depth_target * d3d12->dsv_stride;
        if (!d3d12->depth_surfaces[depth_target].initialized) {
            D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;

            if (d3d12->depth_surfaces[depth_target].has_stencil) {
                flags |= D3D12_CLEAR_FLAG_STENCIL;
            }
            ID3D12GraphicsCommandList_ClearDepthStencilView(
                d3d12->list, dsv, flags, 1.0f, 0, 0, NULL);
            d3d12->depth_surfaces[depth_target].initialized = true;
        }
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(
        d3d12->list, target_count, rtvs, FALSE,
        depth_target >= 0 ? &dsv : NULL);
    if (depth_target >= 0 && context_count > 0x10c) {
        ID3D12GraphicsCommandList_OMSetStencilRef(
            d3d12->list, context[0x10c] & 0xff);
    }
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(
        d3d12->list, hs ? (D3D_PRIMITIVE_TOPOLOGY)(
            D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST +
            CLAMP(context_count > 0x2d6 ?
                (context[0x2d6] >> 8) & 0x3f : 3, 1, 32) - 1) :
        shadps4_d3d12_topology(
            context_count > SHADPS4_CONTEXT_HOST_PRIMITIVE ?
            context[SHADPS4_CONTEXT_HOST_PRIMITIVE] : 4));
    if (!d3d12->traced_first_draw_state) {
        uint32_t primitive = context_count > SHADPS4_CONTEXT_HOST_PRIMITIVE ?
            context[SHADPS4_CONTEXT_HOST_PRIMITIVE] : 4;
        uint32_t target_mask = context_count > 0x8e ? context[0x8e] : 0;
        uint32_t shader_mask = context_count > 0x8f ? context[0x8f] : 0;
        uint32_t polygon = context_count > 0x205 ? context[0x205] : 0;
        uint32_t depth_control = context_count > 0x200 ?
            context[0x200] : 0;

        d3d12->traced_first_draw_state = true;
        info_report("shadPS4 D3D12 first draw state: vertices=%u"
                    " instances=%u indexed=%u primitive=%u target=%#" PRIx64
                    " viewport=[%.3f,%.3f %.3fx%.3f %.3f..%.3f]"
                    " scissor=[%ld,%ld..%ld,%ld] masks=%#x/%#x"
                    " polygon=%#x depth=%#x VS_inputs=%u PS_resources=%zu",
                    vertex_count, num_instances, indexed, primitive,
                    surface ? surface->guest_address : 0,
                    viewport.TopLeftX, viewport.TopLeftY,
                    viewport.Width, viewport.Height,
                    viewport.MinDepth, viewport.MaxDepth,
                    scissor.left, scissor.top, scissor.right, scissor.bottom,
                    target_mask, shader_mask, polygon, depth_control,
                    vs->vertex_input_count, ps ? ps->resource_count : 0);
        if (ps && ps->resource_count) {
            const ShadPS4GcnResource *resource = &ps->resources[0];
            uint8_t sample[64] = { 0 };
            uint32_t nonzero = 0;
            bool readable = resource->guest_address &&
                cpu_memory_rw_debug(cs, resource->guest_address, sample,
                                    sizeof(sample), false) == 0;

            for (uint32_t i = 0; readable && i < sizeof(sample); i++) {
                nonzero += sample[i] != 0;
            }
            info_report("shadPS4 D3D12 first PS resource: type=%u"
                        " guest=%#" PRIx64 " size=%" PRIu64
                        " dimensions=%ux%ux%u pitch=%u levels=%u"
                        " flags=%#x readable=%u sample_nonzero=%u/64",
                        resource->type, resource->guest_address,
                        resource->guest_size, resource->width,
                        resource->height, resource->depth, resource->pitch,
                        resource->levels, resource->flags, readable, nonzero);
        }
    }
    if (indexed) {
        ID3D12GraphicsCommandList_IASetIndexBuffer(d3d12->list, &index_view);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(
            d3d12->list, vertex_count, num_instances, first_index,
            base_vertex, first_instance);
    } else {
        ID3D12GraphicsCommandList_DrawInstanced(
            d3d12->list, vertex_count, num_instances,
            first_vertex, first_instance);
    }
    for (uint32_t i = 0; i < target_count; i++) {
        d3d12->surfaces[targets[i]].rendered = true;
    }
    return true;
}

static void shadps4_d3d12_report_draw_failure(
    ShadPS4D3D12State *d3d12, uint32_t opcode, const char *reason,
    uint32_t payload_count, uint32_t stage_mode, uint32_t target_count,
    uint64_t index_base, uint32_t index_type)
{
    uint64_t count = ++d3d12->draw_failure_count;

    if (!(count & (count - 1))) {
        warn_report("shadPS4 D3D12 draw rejected: count=%" PRIu64
                    " opcode=%#x reason='%s' payload=%u stage_mode=%#x"
                    " targets=%u index_base=%#" PRIx64 " index_type=%u",
                    count, opcode, reason ?: "unknown", payload_count,
                    stage_mode, target_count, index_base, index_type);
    }
}

static bool shadps4_d3d12_record_dispatch(ShadPS4D3D12State *d3d12,
                                          CPUState *cpu,
                                          ShadPS4D3D12Shader *cs,
                                          uint32_t opcode,
                                          const uint32_t *payload,
                                          uint32_t payload_count,
                                          uint64_t indirect_base)
{
    ShadPS4D3D12Pipeline *pipeline;
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t descriptor_page;

    if (!cs || payload_count < 2) {
        return false;
    }
    if (d3d12->descriptor_page >= SHADPS4_DESCRIPTOR_PAGES) {
        return false;
    }
    descriptor_page = d3d12->descriptor_page++;
    if (!shadps4_d3d12_prepare_shader_resources(
            d3d12, cpu, cs, descriptor_page)) {
        return false;
    }
    pipeline = shadps4_d3d12_compute_pipeline(d3d12, cs);
    if (!pipeline) {
        return false;
    }
    if (opcode == SHADPS4_PM4_DISPATCH_DIRECT && payload_count >= 3) {
        x = le32_to_cpu(payload[0]);
        y = le32_to_cpu(payload[1]);
        z = le32_to_cpu(payload[2]);
    } else if (opcode == SHADPS4_PM4_DISPATCH_INDIRECT && indirect_base) {
        uint32_t args[3];

        if (cpu_memory_rw_debug(cpu,
                indirect_base + le32_to_cpu(payload[0]),
                args, sizeof(args), false) != 0) {
            return false;
        }
        x = le32_to_cpu(args[0]);
        y = le32_to_cpu(args[1]);
        z = le32_to_cpu(args[2]);
    } else {
        return false;
    }
    if (!x || !y || !z) {
        return true;
    }
    ID3D12GraphicsCommandList_SetPipelineState(d3d12->list, pipeline->pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(
        d3d12->list, d3d12->root_signature);
    shadps4_d3d12_update_compute_push_data(d3d12, cs, descriptor_page);
    shadps4_d3d12_bind_constants(d3d12, true, descriptor_page);
    shadps4_d3d12_bind_descriptor_heaps(d3d12, true, descriptor_page);
    ID3D12GraphicsCommandList_Dispatch(d3d12->list, x, y, z);
    return true;
}

static void shadps4_d3d12_release(ShadPS4D3D12State *d3d12)
{
    for (uint32_t i = 0; i < d3d12->transient_count; i++) {
        ID3D12Resource_Release(d3d12->transients[i]);
    }
    d3d12->transient_count = 0;
    for (uint32_t i = 0; i < SHADPS4_SHADER_CACHE_SIZE; i++) {
        shadps4_d3d12_shader_clear(&d3d12->shaders[i]);
    }
    for (uint32_t i = 0; i < SHADPS4_PIPELINE_CACHE_SIZE; i++) {
        if (d3d12->graphics_pipelines[i].pso) {
            ID3D12PipelineState_Release(d3d12->graphics_pipelines[i].pso);
        }
        if (d3d12->compute_pipelines[i].pso) {
            ID3D12PipelineState_Release(d3d12->compute_pipelines[i].pso);
        }
    }
    if (d3d12->constant_upload) {
        if (d3d12->constant_map) {
            ID3D12Resource_Unmap(d3d12->constant_upload, 0, NULL);
        }
        ID3D12Resource_Release(d3d12->constant_upload);
        d3d12->constant_upload = NULL;
        d3d12->constant_map = NULL;
    }
    if (d3d12->root_signature) {
        ID3D12RootSignature_Release(d3d12->root_signature);
        d3d12->root_signature = NULL;
    }
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_SURFACES; i++) {
        if (d3d12->surfaces[i].shared_handle) {
            CloseHandle(d3d12->surfaces[i].shared_handle);
        }
        if (d3d12->surfaces[i].readback) {
            ID3D12Resource_Release(d3d12->surfaces[i].readback);
        }
        if (d3d12->surfaces[i].texture) {
            ID3D12Resource_Release(d3d12->surfaces[i].texture);
        }
        memset(&d3d12->surfaces[i], 0, sizeof(d3d12->surfaces[i]));
    }
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_DEPTH_SURFACES; i++) {
        if (d3d12->depth_surfaces[i].texture) {
            ID3D12Resource_Release(d3d12->depth_surfaces[i].texture);
        }
        memset(&d3d12->depth_surfaces[i], 0,
               sizeof(d3d12->depth_surfaces[i]));
    }
    if (d3d12->rtv_heap) {
        ID3D12DescriptorHeap_Release(d3d12->rtv_heap);
        d3d12->rtv_heap = NULL;
    }
    if (d3d12->dsv_heap) {
        ID3D12DescriptorHeap_Release(d3d12->dsv_heap);
        d3d12->dsv_heap = NULL;
    }
    if (d3d12->resource_heap) {
        ID3D12DescriptorHeap_Release(d3d12->resource_heap);
        d3d12->resource_heap = NULL;
    }
    for (uint32_t i = 0; i < SHADPS4_DESCRIPTOR_PAGES; i++) {
        if (d3d12->sampler_heaps[i]) {
            ID3D12DescriptorHeap_Release(d3d12->sampler_heaps[i]);
            d3d12->sampler_heaps[i] = NULL;
        }
    }
    if (d3d12->fence_event) {
        CloseHandle(d3d12->fence_event);
        d3d12->fence_event = NULL;
    }
    if (d3d12->shared_fence_handle) {
        CloseHandle(d3d12->shared_fence_handle);
        d3d12->shared_fence_handle = NULL;
    }
    if (d3d12->fence) {
        ID3D12Fence_Release(d3d12->fence);
        d3d12->fence = NULL;
    }
    if (d3d12->list) {
        ID3D12GraphicsCommandList_Release(d3d12->list);
        d3d12->list = NULL;
    }
    if (d3d12->allocator) {
        ID3D12CommandAllocator_Release(d3d12->allocator);
        d3d12->allocator = NULL;
    }
    if (d3d12->queue) {
        ID3D12CommandQueue_Release(d3d12->queue);
        d3d12->queue = NULL;
    }
    if (d3d12->device) {
        ID3D12Device_Release(d3d12->device);
        d3d12->device = NULL;
    }
    d3d12->ready = false;
}

ShadPS4D3D12State *shadps4_d3d12_create(void)
{
    ShadPS4D3D12State *d3d12 = g_new0(ShadPS4D3D12State, 1);
    D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = SHADPS4_D3D12_MAX_SURFACES,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    D3D12_DESCRIPTOR_HEAP_DESC resource_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 512 * SHADPS4_DESCRIPTOR_PAGES,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = SHADPS4_D3D12_MAX_DEPTH_SURFACES,
    };
    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        .NumDescriptors = 256,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    HRESULT hr;
    uint64_t dxil_version;
    uint64_t gcn_version;
    char *dxil_error = NULL;

    shadps4_d3d12_enable_debug_layer();
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void **)&d3d12->device);
    if (FAILED(hr)) {
        shadps4_d3d12_report("D3D12CreateDevice", hr);
        goto fail;
    }
    d3d12->device->lpVtbl->GetAdapterLuid(d3d12->device,
                                          &d3d12->adapter_luid);
    if (!shadps4_d3d12_create_root_signature(d3d12) ||
        !shadps4_d3d12_create_constant_upload(d3d12)) {
        goto fail;
    }
    hr = ID3D12Device_CreateCommandQueue(
        d3d12->device, &queue_desc, &IID_ID3D12CommandQueue,
        (void **)&d3d12->queue);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateCommandQueue", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateCommandAllocator(
        d3d12->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&d3d12->allocator);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateCommandAllocator", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateCommandList(
        d3d12->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d3d12->allocator, NULL, &IID_ID3D12GraphicsCommandList,
        (void **)&d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateCommandList", hr);
        goto fail;
    }
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Close initial command list", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateFence(d3d12->device, 0,
                                  D3D12_FENCE_FLAG_SHARED,
                                  &IID_ID3D12Fence,
                                  (void **)&d3d12->fence);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateFence", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateSharedHandle(
        d3d12->device, (ID3D12DeviceChild *)d3d12->fence, NULL,
        GENERIC_ALL, NULL, &d3d12->shared_fence_handle);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create shared VideoOut fence handle", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateDescriptorHeap(
        d3d12->device, &heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&d3d12->rtv_heap);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create RTV descriptor heap", hr);
        goto fail;
    }
    d3d12->rtv_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        d3d12->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hr = ID3D12Device_CreateDescriptorHeap(
        d3d12->device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&d3d12->dsv_heap);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create DSV descriptor heap", hr);
        goto fail;
    }
    d3d12->dsv_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        d3d12->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    hr = ID3D12Device_CreateDescriptorHeap(
        d3d12->device, &resource_heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&d3d12->resource_heap);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create resource descriptor heap", hr);
        goto fail;
    }
    for (uint32_t i = 0; i < SHADPS4_DESCRIPTOR_PAGES; i++) {
        hr = ID3D12Device_CreateDescriptorHeap(
            d3d12->device, &sampler_heap_desc, &IID_ID3D12DescriptorHeap,
            (void **)&d3d12->sampler_heaps[i]);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Create sampler descriptor heap", hr);
            goto fail;
        }
    }
    d3d12->resource_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        d3d12->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3d12->sampler_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        d3d12->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d12->fence_event = CreateEventExW(NULL, NULL, 0,
                                        EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!d3d12->fence_event) {
        warn_report("shadPS4 D3D12: CreateEventExW failed: error=%lu",
                    GetLastError());
        goto fail;
    }
    d3d12->ready = true;
    if (shadps4_dxil_available(&dxil_version) &&
        shadps4_dxil_self_test(&dxil_error) &&
        shadps4_d3d12_shader_self_test(&gcn_version, &dxil_error)) {
        info_report("shadPS4 D3D12 backend initialized: feature-level=11_0 "
                    "gcn-bridge=%#" PRIx64 " spirv-to-dxil=%#" PRIx64
                    " self-test=ok", gcn_version, dxil_version);
    } else {
        warn_report("shadPS4 D3D12: SPIR-V to DXIL translator unavailable: "
                    "%s; draw and dispatch translation is disabled",
                    dxil_error ?: "library not linked");
    }
    g_free(dxil_error);
    return d3d12;

fail:
    shadps4_d3d12_release(d3d12);
    g_free(d3d12);
    return NULL;
}

void shadps4_d3d12_destroy(ShadPS4D3D12State *d3d12)
{
    if (!d3d12) {
        return;
    }
    shadps4_d3d12_finish(d3d12);
    shadps4_d3d12_release(d3d12);
    g_free(d3d12);
}

bool shadps4_d3d12_reset(ShadPS4D3D12State *d3d12)
{
    if (!d3d12 || !d3d12->ready || !shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    d3d12->submit_count = 0;
    d3d12->draw_count = 0;
    d3d12->dispatch_count = 0;
    d3d12->unmatched_target_count = 0;
    d3d12->unsupported_opcode_count = 0;
    d3d12->draw_failure_count = 0;
    d3d12->invalid_shader_count = 0;
    d3d12->shader_compile_count = 0;
    d3d12->shader_cache_hit_count = 0;
    d3d12->shader_cache_clock = 0;
    d3d12->pipeline_cache_clock = 0;
    memset(d3d12->reported_no_draw_dwords, 0,
           sizeof(d3d12->reported_no_draw_dwords));
    d3d12->reported_no_draw_count = 0;
    d3d12->traced_first_draw = false;
    d3d12->traced_first_shader = false;
    memset(d3d12->reported_opcodes, 0, sizeof(d3d12->reported_opcodes));
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_SURFACES; i++) {
        d3d12->surfaces[i].rendered = false;
    }
    return true;
}

static void shadps4_d3d12_record_writebacks(ShadPS4D3D12State *d3d12)
{
    for (uint32_t i = 0; i < d3d12->writeback_count; i++) {
        ShadPS4D3D12BoundResource *bound = d3d12->writebacks[i].bound;
        D3D12_RESOURCE_BARRIER barrier = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = bound->resource,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = bound->state,
                .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
            },
        };

        ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
        if (d3d12->writebacks[i].is_image) {
            for (uint32_t subresource = 0;
                 subresource < bound->subresource_count; subresource++) {
                D3D12_TEXTURE_COPY_LOCATION source = {
                    .pResource = bound->resource,
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = subresource,
                };
                D3D12_TEXTURE_COPY_LOCATION dest = {
                    .pResource = bound->readback,
                    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    .PlacedFootprint = bound->footprints[subresource],
                };

                ID3D12GraphicsCommandList_CopyTextureRegion(
                    d3d12->list, &dest, 0, 0, 0, &source, NULL);
            }
        } else {
            ID3D12GraphicsCommandList_CopyBufferRegion(
                d3d12->list, bound->readback, 0, bound->resource, 0,
                d3d12->writebacks[i].size);
        }
        bound->state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
}

static bool shadps4_d3d12_writeback_image(
    ShadPS4D3D12State *d3d12, const ShadPS4D3D12Writeback *writeback,
    const uint8_t *mapped)
{
    const ShadPS4GcnResource *resource = &writeback->image;
    const ShadPS4D3D12BoundResource *bound = writeback->bound;
    g_autofree uint8_t *guest = g_malloc(writeback->size);
    uint32_t levels = MIN(MAX(resource->levels, 1), 16);
    uint32_t layers = MAX(resource->layers, 1);
    bool is_3d = resource->image_type == 10;

    if (writeback->size > INT_MAX ||
        cpu_memory_rw_debug(d3d12->writeback_cpu, writeback->guest_address,
                            guest, writeback->size, false) != 0) {
        return false;
    }
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t mip = 0; mip < levels; mip++) {
            ShadPS4GcnResource layout = *resource;
            uint32_t subresource = is_3d ? mip : mip + layer * levels;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp =
                &bound->footprints[subresource];
            uint32_t width = MAX(resource->width >> mip, 1);
            uint32_t height = MAX(resource->height >> mip, 1);
            uint32_t depth = is_3d ? MAX(resource->depth >> mip, 1) : 1;
            uint32_t copy_width = resource->flags & SHADPS4_GCN_RESOURCE_COMPRESSED ?
                MAX((width + 3) / 4, 1) : width;
            uint32_t copy_height = resource->flags & SHADPS4_GCN_RESOURCE_COMPRESSED ?
                MAX((height + 3) / 4, 1) : height;
            uint32_t bytes = resource->bits_per_element / 8;
            uint64_t layer_size = resource->mip_size[mip] / layers;
            uint64_t slice_size = layer_size / depth;
            uint64_t base = resource->mip_offset[mip] + layer * layer_size;

            layout.pitch = resource->mip_pitch[mip] ?:
                MAX(resource->pitch >> mip, 1);
            layout.height = resource->mip_height[mip] ?: copy_height;
            for (uint32_t z = 0; z < depth; z++) {
                const uint8_t *source_slice = mapped + fp->Offset +
                    (uint64_t)z * fp->Footprint.RowPitch *
                    bound->footprint_rows[subresource];

                for (uint32_t y = 0; y < copy_height; y++) {
                    const uint8_t *source = source_slice +
                        (uint64_t)y * fp->Footprint.RowPitch;

                    for (uint32_t x = 0; x < copy_width; x++) {
                        uint64_t dest = base + z * slice_size +
                            shadps4_d3d12_image_element_offset(
                                &layout, x, y, is_3d ? z : layer);

                        if (dest > writeback->size - bytes) {
                            return false;
                        }
                        memcpy(guest + dest, source + (uint64_t)x * bytes,
                               bytes);
                    }
                }
            }
        }
    }
    return cpu_memory_rw_debug(d3d12->writeback_cpu,
        writeback->guest_address, guest, writeback->size, true) == 0;
}

static bool shadps4_d3d12_complete_writebacks(ShadPS4D3D12State *d3d12)
{
    bool ok = true;

    for (uint32_t i = 0; i < d3d12->writeback_count; i++) {
        const ShadPS4D3D12Writeback *writeback = &d3d12->writebacks[i];
        D3D12_RANGE range = { 0, writeback->is_image ?
            writeback->bound->upload_size : writeback->size };
        void *mapped = NULL;
        HRESULT hr = ID3D12Resource_Map(
            writeback->bound->readback, 0, &range, &mapped);

        if (FAILED(hr) || !mapped || (writeback->is_image ?
            !shadps4_d3d12_writeback_image(d3d12, writeback, mapped) :
            writeback->size > INT_MAX ||
            cpu_memory_rw_debug(d3d12->writeback_cpu,
                writeback->guest_address, mapped, writeback->size, true) != 0)) {
            warn_report("shadPS4 D3D12: guest UAV writeback failed "
                        "address=%#" PRIx64 " size=%" PRIu64,
                        writeback->guest_address, writeback->size);
            ok = false;
        }
        if (SUCCEEDED(hr) && mapped) {
            D3D12_RANGE written = { 0, 0 };
            ID3D12Resource_Unmap(writeback->bound->readback, 0, &written);
        }
    }
    d3d12->writeback_count = 0;
    d3d12->writeback_cpu = NULL;
    return ok;
}

bool shadps4_d3d12_submit(ShadPS4D3D12State *d3d12,
                          CPUState *cs,
                          const uint32_t *commands,
                          uint32_t command_dwords, uint32_t queue_id,
                          const uint32_t *context_registers,
                          uint32_t context_register_count,
                          const uint32_t *shader_registers,
                          uint32_t shader_register_count)
{
    ID3D12CommandList *lists[1];
    uint32_t current_context[0x400];
    uint32_t current_shader[0x400];
    uint32_t offset = 0;
    uint64_t index_base = 0;
    uint64_t indirect_base = 0;
    uint32_t index_type = 0;
    uint32_t num_instances = 1;
    uint64_t draws = 0;
    uint64_t emitted_draws = 0;
    uint64_t dispatches = 0;
    uint32_t opcode_counts[256] = { 0 };
    HRESULT hr;

    if (!d3d12 || !d3d12->ready || !commands || !command_dwords ||
        queue_id >= 64 || !context_registers ||
        context_register_count > ARRAY_SIZE(current_context) ||
        !shader_registers ||
        shader_register_count > ARRAY_SIZE(current_shader)) {
        return false;
    }
    if (!shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    for (uint32_t i = 0; i < d3d12->transient_count; i++) {
        ID3D12Resource_Release(d3d12->transients[i]);
    }
    d3d12->transient_count = 0;
    hr = ID3D12CommandAllocator_Reset(d3d12->allocator);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Reset command allocator", hr);
        return false;
    }
    hr = ID3D12GraphicsCommandList_Reset(d3d12->list,
                                         d3d12->allocator, NULL);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Reset command list", hr);
        return false;
    }
    d3d12->descriptor_page = 0;
    d3d12->writeback_count = 0;
    d3d12->writeback_cpu = cs;
    memcpy(current_context, context_registers,
           context_register_count * sizeof(*current_context));
    current_context[SHADPS4_CONTEXT_HOST_PRIMITIVE] = 4;
    memcpy(current_shader, shader_registers,
           shader_register_count * sizeof(*current_shader));
    while (offset < command_dwords) {
        uint32_t header = le32_to_cpu(commands[offset]);
        uint32_t type = header >> 30;
        uint32_t payload_count;
        uint32_t opcode;

        if ((header == 0 || header == UINT32_C(0x80000000)) &&
            shadps4_d3d12_is_tail_padding(
                commands, offset, command_dwords)) {
            break;
        }

        if (type == 2) {
            offset++;
            continue;
        }
        if (type == 0 || type == 1) {
            payload_count = type == 0 ? ((header >> 16) & 0x3fff) + 1 : 2;
        } else if (type == SHADPS4_PM4_TYPE3) {
            payload_count = ((header >> 16) & 0x3fff) + 1;
            opcode = (header >> 8) & 0xff;
            opcode_counts[opcode]++;
            if (payload_count > command_dwords - offset - 1) {
                goto fail_recording;
            }
            if (opcode == SHADPS4_PM4_SET_CONTEXT_REG &&
                payload_count >= 2) {
                uint32_t reg = le32_to_cpu(commands[offset + 1]);
                uint32_t values = payload_count - 1;

                if (reg >= context_register_count ||
                    values > context_register_count - reg) {
                    goto fail_recording;
                }
                for (uint32_t i = 0; i < values; i++) {
                    current_context[reg + i] =
                        le32_to_cpu(commands[offset + 2 + i]);
                }
            }
            if (opcode == SHADPS4_PM4_SET_SH_REG && payload_count >= 2) {
                uint32_t reg = le32_to_cpu(commands[offset + 1]);
                uint32_t values = payload_count - 1;

                if (reg >= shader_register_count ||
                    values > shader_register_count - reg) {
                    goto fail_recording;
                }
                for (uint32_t i = 0; i < values; i++) {
                    current_shader[reg + i] =
                        le32_to_cpu(commands[offset + 2 + i]);
                }
            }
            if (opcode == SHADPS4_PM4_SET_UCONFIG_REG &&
                payload_count >= 2) {
                uint32_t reg = le32_to_cpu(commands[offset + 1]);

                if (reg <= SHADPS4_UCONFIG_PRIMITIVE_TYPE &&
                    reg + payload_count - 1 >
                    SHADPS4_UCONFIG_PRIMITIVE_TYPE) {
                    current_context[SHADPS4_CONTEXT_HOST_PRIMITIVE] =
                        le32_to_cpu(commands[offset + 2 +
                            SHADPS4_UCONFIG_PRIMITIVE_TYPE - reg]) & 0x3f;
                }
            }
            if (opcode == SHADPS4_PM4_INDEX_BASE && payload_count >= 2) {
                index_base = (le32_to_cpu(commands[offset + 1]) &
                              ~UINT64_C(1)) |
                    ((uint64_t)(le32_to_cpu(commands[offset + 2]) & 0xff)
                     << 32);
            } else if (opcode == SHADPS4_PM4_INDEX_TYPE &&
                       payload_count >= 1) {
                index_type = le32_to_cpu(commands[offset + 1]) & 3;
            } else if (opcode == SHADPS4_PM4_NUM_INSTANCES &&
                       payload_count >= 1) {
                num_instances = MAX(le32_to_cpu(commands[offset + 1]), 1);
            } else if (opcode == SHADPS4_PM4_SET_BASE &&
                       payload_count >= 3 &&
                       (le32_to_cpu(commands[offset + 1]) & 0xf) == 1) {
                indirect_base = le32_to_cpu(commands[offset + 2]) |
                    ((uint64_t)(le32_to_cpu(commands[offset + 3]) & 0xffff)
                     << 32);
            }
            if (shadps4_d3d12_is_draw(opcode)) {
                ShadPS4D3D12Shader *vs;
                ShadPS4D3D12Shader *hs = NULL;
                ShadPS4D3D12Shader *ds = NULL;
                ShadPS4D3D12Shader *gs = NULL;
                ShadPS4D3D12Shader *ps;
                int targets[8];
                uint32_t stage_mode = context_register_count >
                    SHADPS4_CONTEXT_SHADER_STAGES ?
                    current_context[SHADPS4_CONTEXT_SHADER_STAGES] & 0xff : 0;
                uint32_t unified = 0, buffers = 0, user_data = 0;
                bool stages_valid = true;
                const char *draw_failure = NULL;
                uint32_t target_count;
                bool trace_draw = !d3d12->traced_first_draw;

                if (trace_draw) {
                    d3d12->traced_first_draw = true;
                    info_report("shadPS4 D3D12 first draw: opcode=%#x"
                                " payload=%u stage_mode=%#x begin",
                                opcode, payload_count, stage_mode);
                }
                target_count = shadps4_d3d12_find_color_targets(
                    d3d12, current_context, context_register_count, targets);
                if (trace_draw) {
                    info_report("shadPS4 D3D12 first draw: targets=%u",
                                target_count);
                }
                draws++;
                if (stage_mode == 0xb0 || stage_mode == 0xad) {
                    vs = shadps4_d3d12_compile_shader(
                        d3d12, cs, current_shader, shader_register_count,
                        current_context, context_register_count,
                        stage_mode == 0xad ? SHADPS4_SHADER_LS_PGM_LO :
                        SHADPS4_SHADER_ES_PGM_LO,
                        stage_mode == 0xad ? SHADPS4_GCN_STAGE_LOCAL :
                        SHADPS4_GCN_STAGE_EXPORT,
                        SHADPS4_GCN_LOGICAL_VERTEX,
                        SHADPS4_DXIL_STAGE_VERTEX, unified, buffers,
                        user_data, stage_mode == 0xad ? "LS" : "ES");
                } else if (stage_mode == 0x45) {
                    vs = shadps4_d3d12_compile_shader(
                        d3d12, cs, current_shader, shader_register_count,
                        current_context, context_register_count,
                        SHADPS4_SHADER_LS_PGM_LO, SHADPS4_GCN_STAGE_LOCAL,
                        SHADPS4_GCN_LOGICAL_VERTEX,
                        SHADPS4_DXIL_STAGE_VERTEX, unified, buffers,
                        user_data, "LS");
                } else {
                    if (trace_draw) {
                        info_report("shadPS4 D3D12 first draw: compile VS");
                    }
                    vs = shadps4_d3d12_compile_shader(
                        d3d12, cs, current_shader, shader_register_count,
                        current_context, context_register_count,
                        SHADPS4_SHADER_VS_PGM_LO, SHADPS4_GCN_STAGE_VERTEX,
                        SHADPS4_GCN_LOGICAL_VERTEX,
                        SHADPS4_DXIL_STAGE_VERTEX, unified, buffers,
                        user_data, "VS");
                }
                if (trace_draw) {
                    info_report("shadPS4 D3D12 first draw: VS=%p", vs);
                }
                if (vs) {
                    unified = vs->unified_bindings;
                    buffers = vs->buffer_bindings;
                    user_data = vs->user_data_bindings;
                }
                if (stage_mode == 0x45 || stage_mode == 0xad) {
                    hs = shadps4_d3d12_compile_shader(
                        d3d12, cs, current_shader, shader_register_count,
                        current_context, context_register_count,
                        SHADPS4_SHADER_HS_PGM_LO, SHADPS4_GCN_STAGE_HULL,
                        SHADPS4_GCN_LOGICAL_TESS_CONTROL,
                        SHADPS4_DXIL_STAGE_TESS_CONTROL, unified, buffers,
                        user_data, "HS");
                    if (hs) {
                        unified = hs->unified_bindings;
                        buffers = hs->buffer_bindings;
                        user_data = hs->user_data_bindings;
                    }
                    ds = shadps4_d3d12_compile_shader(
                        d3d12, cs, current_shader, shader_register_count,
                        current_context, context_register_count,
                        stage_mode == 0xad ? SHADPS4_SHADER_ES_PGM_LO :
                        SHADPS4_SHADER_VS_PGM_LO,
                        stage_mode == 0xad ? SHADPS4_GCN_STAGE_EXPORT :
                        SHADPS4_GCN_STAGE_VERTEX,
                        SHADPS4_GCN_LOGICAL_TESS_EVALUATION,
                        SHADPS4_DXIL_STAGE_TESS_EVALUATION, unified, buffers,
                        user_data, stage_mode == 0xad ? "ES/DS" : "VS/DS");
                    if (ds) {
                        unified = ds->unified_bindings;
                        buffers = ds->buffer_bindings;
                        user_data = ds->user_data_bindings;
                    }
                    stages_valid = vs && hs && ds;
                }
                if (stage_mode == 0xb0 || stage_mode == 0xad) {
                    gs = shadps4_d3d12_compile_shader(
                        d3d12, cs, current_shader, shader_register_count,
                        current_context, context_register_count,
                        SHADPS4_SHADER_GS_PGM_LO, SHADPS4_GCN_STAGE_GEOMETRY,
                        SHADPS4_GCN_LOGICAL_GEOMETRY,
                        SHADPS4_DXIL_STAGE_GEOMETRY, unified, buffers,
                        user_data, "GS");
                    if (gs) {
                        unified = gs->unified_bindings;
                        buffers = gs->buffer_bindings;
                        user_data = gs->user_data_bindings;
                    }
                    stages_valid = stages_valid && vs && gs;
                }
                if (trace_draw) {
                    info_report("shadPS4 D3D12 first draw: compile PS");
                }
                ps = shadps4_d3d12_compile_shader(
                    d3d12, cs, current_shader, shader_register_count,
                    current_context, context_register_count,
                    SHADPS4_SHADER_PS_PGM_LO, SHADPS4_GCN_STAGE_FRAGMENT,
                    SHADPS4_GCN_LOGICAL_FRAGMENT,
                    SHADPS4_DXIL_STAGE_FRAGMENT,
                    unified, buffers, user_data, "PS");
                if (trace_draw) {
                    info_report("shadPS4 D3D12 first draw: PS=%p emit", ps);
                }
                if (!stages_valid || !shadps4_d3d12_record_draw(
                        d3d12, cs, vs, hs, ds, gs, ps, current_context,
                        context_register_count, targets, target_count, opcode,
                        &commands[offset + 1], payload_count, index_base,
                        index_type, num_instances, indirect_base,
                        &draw_failure)) {
                    shadps4_d3d12_report_draw_failure(
                        d3d12, opcode,
                        stages_valid ? draw_failure :
                        "required shader stage missing",
                        payload_count, stage_mode, target_count, index_base,
                        index_type);
                } else {
                    emitted_draws++;
                }
                if (!target_count) {
                    d3d12->unmatched_target_count++;
                }
            } else if (shadps4_d3d12_is_dispatch(opcode)) {
                ShadPS4D3D12Shader *compute;

                dispatches++;
                compute = shadps4_d3d12_compile_shader(
                    d3d12, cs, current_shader, shader_register_count,
                    NULL, 0,
                    SHADPS4_SHADER_CS_PGM_LO, SHADPS4_GCN_STAGE_COMPUTE,
                    SHADPS4_GCN_LOGICAL_COMPUTE,
                    SHADPS4_DXIL_STAGE_COMPUTE, 0, 0, 0, "CS");
                if (!shadps4_d3d12_record_dispatch(
                        d3d12, cs, compute, opcode, &commands[offset + 1],
                        payload_count, indirect_base)) {
                    shadps4_d3d12_report_opcode_once(d3d12, opcode);
                }
            } else if (shadps4_d3d12_is_sync_opcode(opcode)) {
                shadps4_d3d12_record_sync(d3d12);
            }
        } else {
            goto fail_recording;
        }
        if (payload_count > command_dwords - offset - 1) {
            goto fail_recording;
        }
        offset += payload_count + 1;
    }
    if (d3d12->unmatched_target_count &&
        !(d3d12->unmatched_target_count &
          (d3d12->unmatched_target_count - 1))) {
        warn_report("shadPS4 D3D12: no registered VideoOut surface "
                    "matches CB_COLOR_BASE; unmatched=%" PRIu64,
                    d3d12->unmatched_target_count);
    }
    shadps4_d3d12_record_writebacks(d3d12);
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Close command list", hr);
        d3d12->ready = false;
        return false;
    }
    lists[0] = (ID3D12CommandList *)d3d12->list;
    ID3D12CommandQueue_ExecuteCommandLists(d3d12->queue, 1, lists);
    d3d12->submit_count++;
    d3d12->draw_count += draws;
    d3d12->dispatch_count += dispatches;
    if (!(d3d12->submit_count & (d3d12->submit_count - 1))) {
        info_report("shadPS4 D3D12 submit: count=%" PRIu64
                    " dwords=%u draws=%" PRIu64 " emitted=%" PRIu64
                    " dispatches=%" PRIu64 " total_draws=%" PRIu64,
                    d3d12->submit_count, command_dwords, draws, emitted_draws,
                    dispatches, d3d12->draw_count);
    }
    bool report_no_draw = !draws && !dispatches;

    for (uint32_t i = 0; i < d3d12->reported_no_draw_count; i++) {
        report_no_draw = report_no_draw &&
            d3d12->reported_no_draw_dwords[i] != command_dwords;
    }
    report_no_draw = report_no_draw &&
        d3d12->reported_no_draw_count <
        ARRAY_SIZE(d3d12->reported_no_draw_dwords);
    if (report_no_draw) {
        g_autoptr(GString) opcodes = g_string_new(NULL);

        for (uint32_t i = 0; i < ARRAY_SIZE(opcode_counts); i++) {
            if (opcode_counts[i]) {
                g_string_append_printf(opcodes, "%s%#x:%u",
                                       opcodes->len ? "," : "",
                                       i, opcode_counts[i]);
            }
        }
        d3d12->reported_no_draw_dwords[
            d3d12->reported_no_draw_count++] = command_dwords;
        info_report("shadPS4 D3D12 no-draw submit: dwords=%u opcodes=[%s]",
                    command_dwords, opcodes->str);
    }
    return true;

fail_recording:
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Close aborted command list", hr);
        d3d12->ready = false;
    }
    return false;
}

bool shadps4_d3d12_finish(ShadPS4D3D12State *d3d12)
{
    HRESULT hr;
    DWORD wait_result;

    if (!d3d12 || !d3d12->ready) {
        return false;
    }
    d3d12->fence_value++;
    hr = ID3D12CommandQueue_Signal(d3d12->queue, d3d12->fence,
                                   d3d12->fence_value);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Signal", hr);
        d3d12->ready = false;
        return false;
    }
    if (ID3D12Fence_GetCompletedValue(d3d12->fence) >=
        d3d12->fence_value) {
        return shadps4_d3d12_complete_writebacks(d3d12);
    }
    hr = ID3D12Fence_SetEventOnCompletion(d3d12->fence,
                                          d3d12->fence_value,
                                          d3d12->fence_event);
    if (FAILED(hr)) {
        shadps4_d3d12_report("SetEventOnCompletion", hr);
        d3d12->ready = false;
        return false;
    }
    wait_result = WaitForSingleObjectEx(
        d3d12->fence_event, SHADPS4_D3D12_FENCE_TIMEOUT_MS, FALSE);
    if (wait_result != WAIT_OBJECT_0) {
        HRESULT removed = ID3D12Device_GetDeviceRemovedReason(d3d12->device);

        warn_report("shadPS4 D3D12: fence wait failed: result=%lu error=%lu"
                    " requested=%" PRIu64 " completed=%" PRIu64
                    " device=%#lx; renderer disabled",
                    wait_result, GetLastError(), d3d12->fence_value,
                    ID3D12Fence_GetCompletedValue(d3d12->fence),
                    (unsigned long)removed);
        d3d12->ready = false;
        return false;
    }
    return shadps4_d3d12_complete_writebacks(d3d12);
}

bool shadps4_d3d12_is_ready(const ShadPS4D3D12State *d3d12)
{
    return d3d12 && d3d12->ready;
}

bool shadps4_d3d12_register_surface(ShadPS4D3D12State *d3d12,
                                    uint32_t index, uint64_t guest_address,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch, uint32_t format,
                                    uint32_t tiling_mode)
{
    ShadPS4D3D12Surface *surface;
    D3D12_HEAP_PROPERTIES default_heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_HEAP_PROPERTIES readback_heap = default_heap;
    D3D12_RESOURCE_DESC texture_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .Color = { 0.0f, 0.0f, 0.0f, 1.0f },
    };
    D3D12_RESOURCE_DESC readback_desc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    UINT rows;
    UINT64 row_size;
    HRESULT hr;

    if (!d3d12 || !d3d12->ready || index >= SHADPS4_D3D12_VIDEO_SURFACES ||
        !guest_address || !width || !height || pitch < width ||
        width > 4096 || height > 2160) {
        return false;
    }
    surface = &d3d12->surfaces[index];
    if (!shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    if (surface->readback) {
        ID3D12Resource_Release(surface->readback);
    }
    if (surface->shared_handle) {
        CloseHandle(surface->shared_handle);
    }
    if (surface->texture) {
        ID3D12Resource_Release(surface->texture);
    }
    memset(surface, 0, sizeof(*surface));
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &default_heap, D3D12_HEAP_FLAG_SHARED, &texture_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
        &IID_ID3D12Resource, (void **)&surface->texture);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create VideoOut render target", hr);
        return false;
    }
    hr = ID3D12Device_CreateSharedHandle(
        d3d12->device, (ID3D12DeviceChild *)surface->texture, NULL,
        GENERIC_ALL, NULL, &surface->shared_handle);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create shared VideoOut surface handle", hr);
        ID3D12Resource_Release(surface->texture);
        memset(surface, 0, sizeof(*surface));
        return false;
    }
    ID3D12Device_GetCopyableFootprints(
        d3d12->device, &texture_desc, 0, 1, 0, &surface->footprint,
        &rows, &row_size, &surface->readback_size);
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_desc = (D3D12_RESOURCE_DESC) {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = surface->readback_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &readback_heap, D3D12_HEAP_FLAG_NONE,
        &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
        &IID_ID3D12Resource, (void **)&surface->readback);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create VideoOut readback", hr);
        CloseHandle(surface->shared_handle);
        ID3D12Resource_Release(surface->texture);
        memset(surface, 0, sizeof(*surface));
        return false;
    }
    d3d12->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        d3d12->rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)index * d3d12->rtv_stride;
    ID3D12Device_CreateRenderTargetView(d3d12->device, surface->texture,
                                        NULL, rtv);
    surface->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    surface->width = width;
    surface->height = height;
    surface->guest_address = guest_address;
    surface->pitch = pitch;
    surface->format = format;
    surface->tiling_mode = tiling_mode;
    surface->samples = 1;
    surface->dxgi_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    surface->video_out = true;
    surface->last_use = ++d3d12->surface_clock;
    info_report("shadPS4 D3D12: VideoOut surface=%u guest=%#" PRIx64
                " size=%ux%u pitch=%u format=%u tiling=%u",
                index, guest_address, width, height, pitch, format,
                tiling_mode);
    return true;
}

bool shadps4_d3d12_publish_surface(ShadPS4D3D12State *d3d12,
                                   uint32_t index, uint64_t frame_id)
{
    ShadPS4D3D12Surface *surface;
    ID3D12CommandList *lists[1];
    QemuHostD3D12VideoFrame frame;
    HRESULT hr;

    if (!d3d12 || !d3d12->ready ||
        index >= SHADPS4_D3D12_VIDEO_SURFACES) {
        return false;
    }
    surface = &d3d12->surfaces[index];
    if (!surface->texture || !surface->shared_handle || !surface->rendered ||
        !d3d12->shared_fence_handle || !shadps4_d3d12_finish(d3d12)) {
        return false;
    }

    if (surface->state != D3D12_RESOURCE_STATE_COMMON) {
        D3D12_RESOURCE_BARRIER barrier = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = surface->texture,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = surface->state,
                .StateAfter = D3D12_RESOURCE_STATE_COMMON,
            },
        };

        hr = ID3D12CommandAllocator_Reset(d3d12->allocator);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Reset VideoOut share allocator", hr);
            return false;
        }
        hr = ID3D12GraphicsCommandList_Reset(d3d12->list,
                                             d3d12->allocator, NULL);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Reset VideoOut share list", hr);
            return false;
        }
        ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barrier);
        hr = ID3D12GraphicsCommandList_Close(d3d12->list);
        if (FAILED(hr)) {
            shadps4_d3d12_report("Close VideoOut share list", hr);
            return false;
        }
        lists[0] = (ID3D12CommandList *)d3d12->list;
        ID3D12CommandQueue_ExecuteCommandLists(d3d12->queue, 1, lists);
        surface->state = D3D12_RESOURCE_STATE_COMMON;
    }

    d3d12->fence_value++;
    hr = ID3D12CommandQueue_Signal(d3d12->queue, d3d12->fence,
                                   d3d12->fence_value);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Signal shared VideoOut surface", hr);
        return false;
    }
    frame = (QemuHostD3D12VideoFrame) {
        .size = sizeof(frame),
        .version = QEMU_HOST_D3D12_VIDEO_FRAME_VERSION,
        .frame_id = frame_id,
        .buffer_index = index,
        .width = surface->width,
        .height = surface->height,
        .dxgi_format = surface->dxgi_format,
        .resource_state = D3D12_RESOURCE_STATE_COMMON,
        .adapter_luid_low = d3d12->adapter_luid.LowPart,
        .adapter_luid_high = d3d12->adapter_luid.HighPart,
        .resource_handle = (uint64_t)(uintptr_t)surface->shared_handle,
        .fence_handle = (uint64_t)(uintptr_t)d3d12->shared_fence_handle,
        .fence_value = d3d12->fence_value,
        .device = d3d12->device,
        .command_queue = d3d12->queue,
        .resource = surface->texture,
        .fence = d3d12->fence,
    };
    qemu_host_emit_d3d12_video_frame(&frame);
    return true;
}

bool shadps4_d3d12_read_surface(ShadPS4D3D12State *d3d12,
                                uint32_t index, uint8_t *pixels,
                                uint32_t stride)
{
    ShadPS4D3D12Surface *surface;
    D3D12_RESOURCE_BARRIER barriers[2];
    D3D12_TEXTURE_COPY_LOCATION source;
    D3D12_TEXTURE_COPY_LOCATION destination;
    ID3D12CommandList *lists[1];
    D3D12_RANGE read_range;
    D3D12_RANGE write_range = { 0, 0 };
    uint8_t *mapped;
    HRESULT hr;

    if (!d3d12 || !d3d12->ready || index >= SHADPS4_D3D12_VIDEO_SURFACES ||
        !pixels) {
        return false;
    }
    surface = &d3d12->surfaces[index];
    if (!surface->texture || !surface->readback ||
        stride < surface->width * 4 || !surface->rendered ||
        !shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    hr = ID3D12CommandAllocator_Reset(d3d12->allocator);
    if (FAILED(hr)) {
        return false;
    }
    hr = ID3D12GraphicsCommandList_Reset(d3d12->list,
                                         d3d12->allocator, NULL);
    if (FAILED(hr)) {
        return false;
    }
    barriers[0] = (D3D12_RESOURCE_BARRIER) {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = surface->texture,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = surface->state,
            .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
        },
    };
    barriers[1] = barriers[0];
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, barriers);
    source = (D3D12_TEXTURE_COPY_LOCATION) {
        .pResource = surface->texture,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    destination = (D3D12_TEXTURE_COPY_LOCATION) {
        .pResource = surface->readback,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = surface->footprint,
    };
    ID3D12GraphicsCommandList_CopyTextureRegion(
        d3d12->list, &destination, 0, 0, 0, &source, NULL);
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barriers[1]);
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        return false;
    }
    lists[0] = (ID3D12CommandList *)d3d12->list;
    ID3D12CommandQueue_ExecuteCommandLists(d3d12->queue, 1, lists);
    if (!shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    read_range = (D3D12_RANGE) { 0, surface->readback_size };
    hr = ID3D12Resource_Map(surface->readback, 0, &read_range,
                            (void **)&mapped);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Map VideoOut readback", hr);
        return false;
    }
    for (uint32_t y = 0; y < surface->height; y++) {
        memcpy(pixels + (size_t)y * stride,
               mapped + surface->footprint.Offset +
               (size_t)y * surface->footprint.Footprint.RowPitch,
               surface->width * 4);
    }
    ID3D12Resource_Unmap(surface->readback, 0, &write_range);
    return true;
}
