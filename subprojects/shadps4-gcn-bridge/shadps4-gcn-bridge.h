/*
 * C ABI for the shadPS4 GCN shader recompiler.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SHADPS4_GCN_BRIDGE_H
#define SHADPS4_GCN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# if defined(SHADPS4_GCN_BRIDGE_BUILD)
#  define SHADPS4_GCN_API __declspec(dllexport)
# else
#  define SHADPS4_GCN_API __declspec(dllimport)
# endif
#else
# define SHADPS4_GCN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ShadPS4GcnStage {
    SHADPS4_GCN_STAGE_FRAGMENT = 0,
    SHADPS4_GCN_STAGE_VERTEX = 1,
    SHADPS4_GCN_STAGE_GEOMETRY = 2,
    SHADPS4_GCN_STAGE_EXPORT = 3,
    SHADPS4_GCN_STAGE_HULL = 4,
    SHADPS4_GCN_STAGE_LOCAL = 5,
    SHADPS4_GCN_STAGE_COMPUTE = 6,
} ShadPS4GcnStage;

typedef enum ShadPS4GcnLogicalStage {
    SHADPS4_GCN_LOGICAL_FRAGMENT = 0,
    SHADPS4_GCN_LOGICAL_TESS_CONTROL = 1,
    SHADPS4_GCN_LOGICAL_TESS_EVALUATION = 2,
    SHADPS4_GCN_LOGICAL_VERTEX = 3,
    SHADPS4_GCN_LOGICAL_GEOMETRY = 4,
    SHADPS4_GCN_LOGICAL_COMPUTE = 5,
} ShadPS4GcnLogicalStage;

typedef struct ShadPS4GcnRuntime {
    uint32_t num_user_data;
    uint32_t num_input_vgprs;
    uint32_t num_allocated_vgprs;
    uint32_t workgroup_size[3];
    bool tgid_enable[3];
    uint32_t shared_memory_size;
    uint32_t vertex_num_outputs;
    uint32_t vertex_outputs[12];
    uint32_t vertex_step_rate[2];
    bool vertex_clip_disable;
    uint32_t fragment_input_enable;
    uint32_t fragment_input_address;
    uint32_t fragment_num_inputs;
    uint32_t fragment_inputs[32];
    uint32_t fragment_color_data_format[8];
    uint32_t fragment_color_number_format[8];
    uint32_t fragment_color_export_format[8];
    uint32_t fragment_z_export_format;
    uint32_t fragment_mrtz_mask;
    uint32_t tess_input_control_points;
    uint32_t tess_output_control_points;
    uint32_t tess_type;
    uint32_t tess_partitioning;
    uint32_t tess_topology;
    uint32_t tess_ls_stride;
    uint32_t geometry_output_vertices;
    uint32_t geometry_invocations;
    uint32_t geometry_input_primitive;
    uint32_t geometry_output_primitive[4];
    uint32_t geometry_input_vertex_size;
    uint32_t geometry_output_vertex_size;
    uint32_t geometry_mode;
    const uint32_t *geometry_copy_code;
    size_t geometry_copy_code_words;
    uint64_t geometry_copy_hash;
} ShadPS4GcnRuntime;

typedef bool (*ShadPS4GcnReadMemory)(void *opaque, uint64_t address,
                                     void *data, size_t size);

typedef struct ShadPS4GcnInput {
    const uint32_t *code;
    size_t code_words;
    const uint32_t *user_data;
    size_t user_data_words;
    uint64_t hash;
    ShadPS4GcnStage stage;
    ShadPS4GcnLogicalStage logical_stage;
    ShadPS4GcnRuntime runtime;
    uint32_t initial_unified_bindings;
    uint32_t initial_buffer_bindings;
    uint32_t initial_user_data_bindings;
    ShadPS4GcnReadMemory read_memory;
    void *read_memory_opaque;
} ShadPS4GcnInput;

typedef enum ShadPS4GcnResourceType {
    SHADPS4_GCN_RESOURCE_BUFFER = 0,
    SHADPS4_GCN_RESOURCE_FLAT_BUFFER = 1,
    SHADPS4_GCN_RESOURCE_IMAGE = 2,
    SHADPS4_GCN_RESOURCE_SAMPLER = 3,
    SHADPS4_GCN_RESOURCE_SPECIAL_BUFFER = 4,
} ShadPS4GcnResourceType;

enum {
    SHADPS4_GCN_RESOURCE_WRITTEN = 1u << 0,
    SHADPS4_GCN_RESOURCE_STORAGE = 1u << 1,
    SHADPS4_GCN_RESOURCE_DEPTH = 1u << 2,
    SHADPS4_GCN_RESOURCE_ARRAY = 1u << 3,
    SHADPS4_GCN_RESOURCE_COMPRESSED = 1u << 4,
    SHADPS4_GCN_RESOURCE_CUBE = 1u << 5,
};

typedef struct ShadPS4GcnResource {
    ShadPS4GcnResourceType type;
    uint32_t binding;
    uint32_t binding_count;
    uint32_t flags;
    uint32_t sharp_index;
    uint64_t guest_address;
    uint64_t guest_size;
    uint32_t sharp[8];
    uint32_t sharp_words;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t pitch;
    uint32_t levels;
    uint32_t layers;
    uint32_t samples;
    uint32_t bits_per_element;
    uint32_t data_format;
    uint32_t number_format;
    uint32_t image_type;
    uint32_t tiling_mode;
    uint32_t array_mode;
    uint32_t micro_tile_mode;
    uint32_t micro_tile_thickness;
    uint32_t pipe_config;
    uint32_t bank_width;
    uint32_t bank_height;
    uint32_t num_banks;
    uint32_t tile_split_bytes;
    uint32_t macro_tile_aspect;
    uint32_t bank_swizzle;
    uint32_t mip_offset[16];
    uint32_t mip_size[16];
    uint32_t mip_pitch[16];
    uint32_t mip_height[16];
    uint32_t component_swizzle[4];
    uint32_t sampler_clamp[3];
    uint32_t sampler_filter[3];
    uint32_t sampler_compare;
    uint32_t sampler_border_color;
    float sampler_lod_bias;
    float sampler_min_lod;
    float sampler_max_lod;
    float sampler_max_aniso;
} ShadPS4GcnResource;

#define SHADPS4_GCN_MAX_VERTEX_INPUTS 32

typedef struct ShadPS4GcnVertexInput {
    uint32_t semantic;
    uint64_t guest_address;
    uint64_t guest_size;
    uint32_t stride;
    uint32_t data_format;
    uint32_t number_format;
    uint32_t instance_step_rate;
} ShadPS4GcnVertexInput;

typedef struct ShadPS4GcnOutput {
    uint32_t *spirv;
    size_t spirv_words;
    uint32_t unified_bindings;
    uint32_t buffer_bindings;
    uint32_t user_data_bindings;
    ShadPS4GcnResource *resources;
    size_t resource_count;
    uint32_t *flat_user_data;
    size_t flat_user_data_words;
    uint32_t user_data_binding_start;
    uint32_t user_data_value_count;
    uint32_t user_data_values[16];
    uint32_t vertex_input_count;
    ShadPS4GcnVertexInput vertex_inputs[SHADPS4_GCN_MAX_VERTEX_INPUTS];
} ShadPS4GcnOutput;

SHADPS4_GCN_API bool shadps4_gcn_compile(const ShadPS4GcnInput *input,
                                         ShadPS4GcnOutput *output,
                                         char **error);
SHADPS4_GCN_API void shadps4_gcn_output_free(ShadPS4GcnOutput *output);
SHADPS4_GCN_API void shadps4_gcn_error_free(char *error);
SHADPS4_GCN_API uint64_t shadps4_gcn_bridge_version(void);

#ifdef __cplusplus
}
#endif

#endif
