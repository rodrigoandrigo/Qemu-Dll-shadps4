/*
 * C ABI for the shadPS4 GCN shader recompiler.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "shadps4-gcn-bridge.h"
#include "bridge-memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/texture_cache/tile.h"

static thread_local ShadPS4GcnReadMemory current_read_memory;
static thread_local void *current_read_memory_opaque;

bool shadps4_gcn_bridge_read_memory(uint64_t address, void *data, size_t size)
{
    return current_read_memory &&
           current_read_memory(current_read_memory_opaque, address, data, size);
}

class ScopedMemoryReader {
public:
    explicit ScopedMemoryReader(const ShadPS4GcnInput& input)
    {
        current_read_memory = input.read_memory;
        current_read_memory_opaque = input.read_memory_opaque;
    }

    ~ScopedMemoryReader()
    {
        current_read_memory = nullptr;
        current_read_memory_opaque = nullptr;
    }
};

void assert_fail_impl() {
    throw std::runtime_error("shadPS4 shader assertion failed");
}

[[noreturn]] void unreachable_impl() {
    throw std::runtime_error("shadPS4 shader reached unreachable code");
}

static char *copy_error(const char *message) {
    const char *text = message && *message ? message : "GCN shader compilation failed";
    const size_t size = std::strlen(text) + 1;
    char *copy = static_cast<char *>(std::malloc(size));

    if (copy) {
        std::memcpy(copy, text, size);
    }
    return copy;
}

static void collect_resources(const Shader::Info& info,
                              uint32_t initial_binding,
                              ShadPS4GcnOutput *output)
{
    const size_t count = info.buffers.size() + info.images.size() +
                         info.samplers.size();
    uint32_t binding = initial_binding;
    size_t index = 0;

    output->resources = static_cast<ShadPS4GcnResource *>(
        std::calloc(count, sizeof(ShadPS4GcnResource)));
    if (count && !output->resources) {
        throw std::bad_alloc();
    }
    output->resource_count = count;
    for (const auto& desc : info.buffers) {
        auto& resource = output->resources[index++];

        resource.binding = binding++;
        resource.binding_count = 1;
        resource.sharp_index = desc.sharp_idx;
        resource.flags = SHADPS4_GCN_RESOURCE_STORAGE |
                         (desc.is_written ? SHADPS4_GCN_RESOURCE_WRITTEN : 0);
        if (desc.buffer_type == Shader::BufferType::Guest) {
            const auto sharp = desc.GetSharp(info);

            resource.type = SHADPS4_GCN_RESOURCE_BUFFER;
            resource.guest_address = sharp.base_address;
            resource.guest_size = sharp.GetSize();
            resource.sharp_words = sizeof(sharp) / sizeof(uint32_t);
            std::memcpy(resource.sharp, &sharp, sizeof(sharp));
        } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
            resource.type = SHADPS4_GCN_RESOURCE_FLAT_BUFFER;
            resource.guest_size = info.flattened_ud_buf.size() * sizeof(uint32_t);
        } else {
            resource.type = SHADPS4_GCN_RESOURCE_SPECIAL_BUFFER;
        }
    }
    for (const auto& desc : info.images) {
        auto& resource = output->resources[index++];
        const auto sharp = desc.GetSharp(info);
        const uint32_t num_bindings = desc.NumBindings(info);

        resource.type = SHADPS4_GCN_RESOURCE_IMAGE;
        resource.binding = binding;
        resource.binding_count = num_bindings;
        resource.sharp_index = desc.sharp_idx;
        resource.flags = (desc.is_written ?
                          SHADPS4_GCN_RESOURCE_WRITTEN |
                          SHADPS4_GCN_RESOURCE_STORAGE : 0) |
                         (desc.is_depth ? SHADPS4_GCN_RESOURCE_DEPTH : 0) |
                         (desc.is_array ? SHADPS4_GCN_RESOURCE_ARRAY : 0);
        resource.guest_address = sharp.Address();
        resource.width = sharp.width + 1;
        resource.height = sharp.height + 1;
        resource.depth = sharp.GetBaseType() == AmdGpu::ImageType::Color3D
                             ? sharp.depth + 1 : 1;
        resource.pitch = sharp.Pitch();
        resource.levels = sharp.NumLevels();
        resource.layers = sharp.NumLayers();
        resource.samples = sharp.NumSamples();
        resource.data_format = static_cast<uint32_t>(sharp.GetDataFmt());
        resource.number_format = static_cast<uint32_t>(sharp.GetNumberFmt());
        resource.image_type = static_cast<uint32_t>(sharp.GetViewType(desc.is_array));
        resource.tiling_mode = static_cast<uint32_t>(sharp.GetTileMode());
        resource.bits_per_element = AmdGpu::NumBitsPerElement(sharp.GetDataFmt());
        const bool block_coded = AmdGpu::IsBlockCoded(sharp.GetDataFmt());
        if (block_coded) {
            resource.flags |= SHADPS4_GCN_RESOURCE_COMPRESSED;
            resource.bits_per_element = AmdGpu::NumBitsPerBlock(sharp.GetDataFmt());
        }
        if (sharp.IsCube()) {
            resource.flags |= SHADPS4_GCN_RESOURCE_CUBE;
        }
        const auto array_mode = AmdGpu::GetArrayMode(sharp.GetTileMode());
        const auto micro_mode = AmdGpu::GetMicroTileMode(sharp.GetTileMode());
        resource.array_mode = static_cast<uint32_t>(array_mode);
        resource.micro_tile_mode = static_cast<uint32_t>(micro_mode);
        resource.micro_tile_thickness = AmdGpu::GetMicroTileThickness(array_mode);
        resource.pipe_config = static_cast<uint32_t>(
            AmdGpu::GetPipeConfig(sharp.GetTileMode()));
        resource.bank_swizzle = sharp.GetBankSwizzle();
        if (AmdGpu::IsMacroTiled(array_mode) && resource.bits_per_element) {
            const auto macro_mode = AmdGpu::CalculateMacrotileMode(
                sharp.GetTileMode(), resource.bits_per_element, sharp.NumSamples());
            resource.bank_width = AmdGpu::GetBankWidth(macro_mode);
            resource.bank_height = AmdGpu::GetBankHeight(macro_mode);
            resource.num_banks = AmdGpu::GetNumBanks(macro_mode);
            resource.macro_tile_aspect = AmdGpu::GetMacrotileAspect(macro_mode);
            resource.tile_split_bytes = AmdGpu::CalculateTileSplit(
                sharp.GetTileMode(), array_mode, micro_mode,
                resource.bits_per_element);
            const uint32_t pipes = AmdGpu::GetPipeCount(
                AmdGpu::GetPipeConfig(sharp.GetTileMode()));
            const uint32_t macro_pitch = 8 * resource.bank_width * pipes *
                resource.macro_tile_aspect;
            const uint32_t macro_height = 8 * resource.bank_height *
                resource.num_banks / resource.macro_tile_aspect;
            const uint32_t padded_pitch =
                (resource.pitch + macro_pitch - 1) / macro_pitch * macro_pitch;
            const uint32_t padded_height =
                (resource.height + macro_height - 1) / macro_height * macro_height;
            resource.guest_size = static_cast<uint64_t>(padded_pitch) *
                padded_height * resource.layers * resource.samples *
                resource.bits_per_element / 8;
        }
        resource.guest_size = 0;
        for (uint32_t mip = 0; mip < std::min(resource.levels, 16u); mip++) {
            uint32_t mip_width = std::max(resource.pitch >> mip, 1u);
            uint32_t mip_height = std::max(resource.height >> mip, 1u);
            uint32_t mip_depth = std::max(resource.depth >> mip, 1u);
            uint32_t thickness = 1;
            size_t mip_size = 0;

            if (block_coded) {
                mip_width = std::max((mip_width + 3) / 4, 1u);
                mip_height = std::max((mip_height + 3) / 4, 1u);
            }
            switch (array_mode) {
            case AmdGpu::ArrayMode::ArrayLinearAligned:
                std::tie(resource.mip_pitch[mip], resource.mip_height[mip], mip_size) =
                    VideoCore::ImageSizeLinearAligned(mip_width, mip_height,
                        resource.bits_per_element, resource.samples);
                break;
            case AmdGpu::ArrayMode::Array1DTiledThick:
                thickness = 4;
                [[fallthrough]];
            case AmdGpu::ArrayMode::Array1DTiledThin1:
                std::tie(resource.mip_pitch[mip], resource.mip_height[mip], mip_size) =
                    VideoCore::ImageSizeMicroTiled(mip_width, mip_height, thickness,
                        resource.bits_per_element, resource.samples);
                break;
            case AmdGpu::ArrayMode::Array2DTiledThick:
                thickness = 4;
                [[fallthrough]];
            case AmdGpu::ArrayMode::Array2DTiledThin1:
                std::tie(resource.mip_pitch[mip], resource.mip_height[mip], mip_size) =
                    VideoCore::ImageSizeMacroTiled(mip_width, mip_height, thickness,
                        resource.bits_per_element, resource.samples,
                        sharp.GetTileMode(), mip, bool(sharp.alt_tile_mode));
                break;
            default:
                std::tie(resource.mip_pitch[mip], resource.mip_height[mip], mip_size) =
                    VideoCore::ImageSizeLinearAligned(mip_width, mip_height,
                        resource.bits_per_element, resource.samples);
                break;
            }
            mip_size *= mip_depth * resource.layers;
            resource.mip_offset[mip] = resource.guest_size;
            resource.mip_size[mip] = static_cast<uint32_t>(mip_size);
            resource.guest_size += mip_size;
        }
        const auto swizzle = sharp.DstSelect();
        for (size_t component = 0; component < 4; component++) {
            resource.component_swizzle[component] =
                static_cast<uint32_t>(swizzle.array[component]);
        }
        resource.sharp_words = sizeof(sharp) / sizeof(uint32_t);
        std::memcpy(resource.sharp, &sharp, sizeof(sharp));
        binding += num_bindings;
    }
    for (const auto& desc : info.samplers) {
        auto& resource = output->resources[index++];
        const auto sharp = desc.GetSharp(info);

        resource.type = SHADPS4_GCN_RESOURCE_SAMPLER;
        resource.binding = binding++;
        resource.binding_count = 1;
        resource.sharp_index = desc.sharp_idx;
        resource.sharp_words = sizeof(sharp) / sizeof(uint32_t);
        std::memcpy(resource.sharp, &sharp, sizeof(sharp));
        resource.sampler_clamp[0] = static_cast<uint32_t>(sharp.clamp_x.Value());
        resource.sampler_clamp[1] = static_cast<uint32_t>(sharp.clamp_y.Value());
        resource.sampler_clamp[2] = static_cast<uint32_t>(sharp.clamp_z.Value());
        resource.sampler_filter[0] = static_cast<uint32_t>(sharp.xy_min_filter.Value());
        resource.sampler_filter[1] = static_cast<uint32_t>(sharp.xy_mag_filter.Value());
        resource.sampler_filter[2] = static_cast<uint32_t>(sharp.mip_filter.Value());
        resource.sampler_compare = static_cast<uint32_t>(sharp.depth_compare_func.Value());
        resource.sampler_border_color = static_cast<uint32_t>(sharp.border_color_type.Value());
        resource.sampler_lod_bias = sharp.LodBias();
        resource.sampler_min_lod = sharp.MinLod();
        resource.sampler_max_lod = sharp.MaxLod();
        const uint32_t aniso = static_cast<uint32_t>(sharp.max_aniso.Value());
        resource.sampler_max_aniso = aniso < 5 ?
            static_cast<float>(1u << aniso) : 1.0f;
    }
    output->flat_user_data_words = info.flattened_ud_buf.size();
    if (output->flat_user_data_words) {
        output->flat_user_data = static_cast<uint32_t *>(std::malloc(
            info.flattened_ud_buf.size() * sizeof(uint32_t)));
        if (!output->flat_user_data) {
            throw std::bad_alloc();
        }
        std::memcpy(output->flat_user_data, info.flattened_ud_buf.data(),
                    info.flattened_ud_buf.size() * sizeof(uint32_t));
    }
}

static Shader::Profile make_profile() {
    Shader::Profile profile{};

    profile.max_ubo_size = 64 * 1024;
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    profile.max_shared_memory_size = 64 * 1024;
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 64;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = true;
    profile.support_float16 = true;
    profile.support_float64 = true;
    profile.supports_denorm_behavior_independence = true;
    profile.supports_rounding_mode_independence = true;
    profile.supports_image_load_store_lod = true;
    profile.supports_native_cube_calc = true;
    profile.supports_trinary_minmax = true;
    profile.supports_buffer_int64_atomics = true;
    profile.supports_shared_int64_atomics = true;
    profile.supports_workgroup_explicit_memory_layout = true;
    profile.needs_buffer_offsets = true;
    return profile;
}

extern "C" bool shadps4_gcn_compile(const ShadPS4GcnInput *input,
                                     ShadPS4GcnOutput *output,
                                     char **error) {
    if (error) {
        *error = nullptr;
    }
    if (!output) {
        return false;
    }
    shadps4_gcn_output_free(output);
    if (!input || !input->code || input->code_words == 0 ||
        !input->user_data || input->user_data_words != 16 ||
        input->stage > SHADPS4_GCN_STAGE_COMPUTE ||
        input->logical_stage > SHADPS4_GCN_LOGICAL_COMPUTE) {
        if (error) {
            *error = copy_error("invalid GCN shader input");
        }
        return false;
    }

    try {
        const ScopedMemoryReader memory_reader{*input};
        const auto stage = static_cast<Shader::Stage>(input->stage);
        const auto logical = static_cast<Shader::LogicalStage>(input->logical_stage);
        Shader::ShaderParams params{
            .user_data = std::span<const uint32_t, 16>{input->user_data, 16},
            .code = std::span<const uint32_t>{input->code, input->code_words},
            .hash = input->hash,
        };
        Shader::Info info{stage, logical, params};
        Shader::RuntimeInfo runtime{};
        Shader::Pools pools;
        Shader::Backend::Bindings bindings{
            .unified = input->initial_unified_bindings,
            .buffer = input->initial_buffer_bindings,
            .user_data = input->initial_user_data_bindings,
        };
        const Shader::Profile profile = make_profile();

        runtime.Initialize(stage);
        runtime.num_user_data = input->runtime.num_user_data;
        runtime.num_input_vgprs = input->runtime.num_input_vgprs;
        runtime.num_allocated_vgprs = input->runtime.num_allocated_vgprs;
        if (stage == Shader::Stage::Vertex) {
            runtime.vs_info.num_outputs = input->runtime.vertex_num_outputs;
            for (size_t i = 0; i < 3; i++) {
                for (size_t component = 0; component < 4; component++) {
                    runtime.vs_info.outputs[i][component] =
                        static_cast<Shader::Output>(
                            input->runtime.vertex_outputs[i * 4 + component]);
                }
            }
            runtime.vs_info.step_rate_0 = input->runtime.vertex_step_rate[0];
            runtime.vs_info.step_rate_1 = input->runtime.vertex_step_rate[1];
            runtime.vs_info.clip_disable = input->runtime.vertex_clip_disable;
            if (logical == Shader::LogicalStage::TessellationEval) {
                runtime.es_vs_info.tess_type = static_cast<AmdGpu::TessellationType>(
                    input->runtime.tess_type);
                runtime.es_vs_info.tess_topology =
                    static_cast<AmdGpu::TessellationTopology>(
                        input->runtime.tess_topology);
                runtime.es_vs_info.tess_partitioning =
                    static_cast<AmdGpu::TessellationPartitioning>(
                        input->runtime.tess_partitioning);
            }
        } else if (stage == Shader::Stage::Local) {
            runtime.ls_info.ls_stride = input->runtime.tess_ls_stride;
        } else if (stage == Shader::Stage::Hull) {
            runtime.hs_info.num_input_control_points =
                input->runtime.tess_input_control_points;
            runtime.hs_info.num_threads = input->runtime.tess_output_control_points;
            runtime.hs_info.tess_type = static_cast<AmdGpu::TessellationType>(
                input->runtime.tess_type);
            runtime.hs_info.ls_stride = input->runtime.tess_ls_stride;
        } else if (stage == Shader::Stage::Export) {
            runtime.es_info.vertex_data_size =
                input->runtime.geometry_input_vertex_size;
            if (logical == Shader::LogicalStage::TessellationEval) {
                runtime.es_vs_info.tess_type = static_cast<AmdGpu::TessellationType>(
                    input->runtime.tess_type);
                runtime.es_vs_info.tess_topology =
                    static_cast<AmdGpu::TessellationTopology>(
                        input->runtime.tess_topology);
                runtime.es_vs_info.tess_partitioning =
                    static_cast<AmdGpu::TessellationPartitioning>(
                        input->runtime.tess_partitioning);
            }
        } else if (stage == Shader::Stage::Geometry) {
            runtime.gs_info.num_outputs = input->runtime.vertex_num_outputs;
            for (size_t i = 0; i < 3; i++) {
                for (size_t component = 0; component < 4; component++) {
                    runtime.gs_info.outputs[i][component] =
                        static_cast<Shader::Output>(
                            input->runtime.vertex_outputs[i * 4 + component]);
                }
            }
            runtime.gs_info.output_vertices =
                input->runtime.geometry_output_vertices;
            runtime.gs_info.num_invocations =
                std::max(input->runtime.geometry_invocations, 1u);
            runtime.gs_info.in_primitive = static_cast<AmdGpu::PrimitiveType>(
                input->runtime.geometry_input_primitive);
            for (size_t i = 0; i < 4; i++) {
                runtime.gs_info.out_primitive[i] =
                    static_cast<AmdGpu::GsOutputPrimitiveType>(
                        input->runtime.geometry_output_primitive[i]);
            }
            runtime.gs_info.in_vertex_data_size =
                input->runtime.geometry_input_vertex_size;
            runtime.gs_info.out_vertex_data_size =
                input->runtime.geometry_output_vertex_size;
            runtime.gs_info.mode = static_cast<AmdGpu::GsScenario>(
                input->runtime.geometry_mode);
            runtime.gs_info.vs_copy = std::span<const uint32_t>{
                input->runtime.geometry_copy_code,
                input->runtime.geometry_copy_code_words};
            runtime.gs_info.vs_copy_hash = input->runtime.geometry_copy_hash;
        } else if (stage == Shader::Stage::Fragment) {
            std::memcpy(&runtime.fs_info.en_flags,
                        &input->runtime.fragment_input_enable, sizeof(uint32_t));
            std::memcpy(&runtime.fs_info.addr_flags,
                        &input->runtime.fragment_input_address, sizeof(uint32_t));
            runtime.fs_info.num_inputs =
                std::min(input->runtime.fragment_num_inputs, 32u);
            for (size_t i = 0; i < runtime.fs_info.num_inputs; i++) {
                const uint32_t packed = input->runtime.fragment_inputs[i];
                runtime.fs_info.inputs[i] = {
                    .param_index = static_cast<uint8_t>(packed & 0x1f),
                    .is_default = bool((packed >> 5) & 1),
                    .is_flat = bool((packed >> 10) & 1),
                    .default_value = static_cast<uint8_t>((packed >> 8) & 3),
                };
            }
            for (size_t i = 0; i < Shader::MaxColorBuffers; i++) {
                auto& color = runtime.fs_info.color_buffers[i];
                color.data_format = static_cast<AmdGpu::DataFormat>(
                    input->runtime.fragment_color_data_format[i]);
                color.num_format = static_cast<AmdGpu::NumberFormat>(
                    input->runtime.fragment_color_number_format[i]);
                color.num_conversion = AmdGpu::NumberConversion::None;
                color.export_format = static_cast<AmdGpu::ShaderExportFormat>(
                    input->runtime.fragment_color_export_format[i]);
                color.swizzle = {
                    .r = AmdGpu::CompSwizzle::Red,
                    .g = AmdGpu::CompSwizzle::Green,
                    .b = AmdGpu::CompSwizzle::Blue,
                    .a = AmdGpu::CompSwizzle::Alpha,
                };
            }
            runtime.fs_info.z_export_format =
                static_cast<AmdGpu::ShaderExportFormat>(
                    input->runtime.fragment_z_export_format);
            runtime.fs_info.mrtz_mask = input->runtime.fragment_mrtz_mask;
        }
        if (stage == Shader::Stage::Compute) {
            runtime.cs_info.shared_memory_size = input->runtime.shared_memory_size;
            for (size_t i = 0; i < 3; ++i) {
                runtime.cs_info.workgroup_size[i] = input->runtime.workgroup_size[i];
                runtime.cs_info.tgid_enable[i] = input->runtime.tgid_enable[i];
            }
        }

        auto program = Shader::TranslateProgram(params.code, pools, info, runtime, profile);
        if (stage == Shader::Stage::Vertex && info.has_fetch_shader) {
            const auto fetch = Shader::Gcn::ParseFetchShader(info);

            if (fetch) {
                output->vertex_input_count = std::min<size_t>(
                    fetch->attributes.size(), SHADPS4_GCN_MAX_VERTEX_INPUTS);
                for (size_t i = 0; i < output->vertex_input_count; i++) {
                    const auto& attribute = fetch->attributes[i];
                    const auto sharp = attribute.GetSharp(info);
                    auto& vertex = output->vertex_inputs[i];

                    vertex.semantic = attribute.semantic;
                    vertex.guest_address = sharp.base_address;
                    vertex.guest_size = sharp.GetSize();
                    vertex.stride = sharp.GetStride();
                    vertex.data_format = static_cast<uint32_t>(
                        sharp.GetDataFmt());
                    vertex.number_format = static_cast<uint32_t>(
                        sharp.GetNumberFmt());
                    switch (attribute.GetStepRate()) {
                    case Shader::Gcn::VertexAttribute::OverStepRate0:
                        vertex.instance_step_rate = std::max(
                            input->runtime.vertex_step_rate[0], 1u);
                        break;
                    case Shader::Gcn::VertexAttribute::OverStepRate1:
                        vertex.instance_step_rate = std::max(
                            input->runtime.vertex_step_rate[1], 1u);
                        break;
                    case Shader::Gcn::VertexAttribute::Plain:
                        vertex.instance_step_rate = 1;
                        break;
                    default:
                        vertex.instance_step_rate = 0;
                        break;
                    }
                }
            }
        }
        auto spirv = Shader::Backend::SPIRV::EmitSPIRV(
            profile, runtime, program, bindings);
        if (spirv.empty()) {
            throw std::runtime_error("shadPS4 emitted an empty SPIR-V module");
        }
        output->spirv = static_cast<uint32_t *>(
            std::malloc(spirv.size() * sizeof(uint32_t)));
        if (!output->spirv) {
            throw std::bad_alloc();
        }
        std::memcpy(output->spirv, spirv.data(),
                    spirv.size() * sizeof(uint32_t));
        output->spirv_words = spirv.size();
        output->unified_bindings = bindings.unified;
        output->buffer_bindings = bindings.buffer;
        output->user_data_bindings = bindings.user_data;
        output->user_data_binding_start = input->initial_user_data_bindings;
        for (uint32_t mask = program.info.ud_mask.mask; mask;
             mask &= mask - 1) {
            const uint32_t user_index = std::countr_zero(mask);

            output->user_data_values[output->user_data_value_count++] =
                input->user_data[user_index];
        }
        collect_resources(program.info, input->initial_unified_bindings,
                          output);
        return true;
    } catch (const std::exception &exception) {
        shadps4_gcn_output_free(output);
        if (error) {
            *error = copy_error(exception.what());
        }
        return false;
    } catch (...) {
        shadps4_gcn_output_free(output);
        if (error) {
            *error = copy_error("unknown GCN shader compilation failure");
        }
        return false;
    }
}

extern "C" void shadps4_gcn_output_free(ShadPS4GcnOutput *output) {
    if (!output) {
        return;
    }
    std::free(output->spirv);
    std::free(output->resources);
    std::free(output->flat_user_data);
    std::memset(output, 0, sizeof(*output));
}

extern "C" void shadps4_gcn_error_free(char *error) {
    std::free(error);
}

extern "C" uint64_t shadps4_gcn_bridge_version(void) {
    return UINT64_C(0x00010005);
}
