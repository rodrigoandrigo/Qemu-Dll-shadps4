/*
 * QEMU/UWP-safe fetch shader reader.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <array>
#include <cstring>
#include <limits>

#include "bridge-memory.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"

namespace Shader::Gcn {

static constexpr size_t FetchShaderMaxDwords = 4096;
static thread_local std::array<u32, FetchShaderMaxDwords> fetch_code;
static thread_local size_t fetch_code_words;

static bool IsTypedBufferLoad(const GcnInst& inst) {
    return inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_X ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XY ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZ ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZW;
}

const u32* GetFetchShaderCode(const Info& info, u32 sgpr_base) {
    uint64_t address = 0;

    if (sgpr_base + 1 >= info.user_data.size()) {
        return nullptr;
    }
    std::memcpy(&address, &info.user_data[sgpr_base], sizeof(address));
    address &= UINT64_C(0xffffffffffff);
    fetch_code.fill(0);
    fetch_code_words = 0;
    if (!address) {
        return nullptr;
    }
    while (fetch_code_words < fetch_code.size() &&
           shadps4_gcn_bridge_read_memory(
               address + fetch_code_words * sizeof(u32),
               &fetch_code[fetch_code_words], sizeof(u32))) {
        fetch_code_words++;
    }
    if (!fetch_code_words) {
        return nullptr;
    }
    return fetch_code.data();
}

std::optional<FetchShaderData> ParseFetchShader(const Info& info) {
    if (!info.has_fetch_shader) {
        return std::nullopt;
    }
    FetchShaderData data{};
    const u32* code = GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    if (!code) {
        return data;
    }

    GcnCodeSlice code_slice(code, code + fetch_code_words);
    GcnDecodeContext decoder;
    struct VsharpLoad {
        u32 dword_offset{};
        u32 base_sgpr{};
    };
    std::array<VsharpLoad, 104> loads{};
    u32 semantic_index = 0;

    while (!code_slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(code_slice);
        data.size += inst.length;
        if (inst.opcode == Opcode::S_SETPC_B64) {
            return data;
        }
        if (inst.inst_class == InstClass::ScalarMemRd) {
            const u32 destination = inst.dst[0].code;
            if (destination >= loads.size()) {
                return data;
            }
            loads[destination] = {
                inst.control.smrd.offset, inst.src[0].code * 2
            };
            continue;
        }
        if (inst.opcode == Opcode::V_ADD_I32) {
            const auto vgpr = inst.dst[0].code;
            const auto sgpr = s8(inst.src[0].code);
            if (vgpr == 0) {
                data.vertex_offset_sgpr = sgpr;
            } else if (vgpr == 3) {
                data.instance_offset_sgpr = sgpr;
            } else {
                return data;
            }
        }
        if (inst.inst_class == InstClass::VectorMemBufFmt) {
            const u32 base_sgpr = inst.src[2].code * 4;
            if (base_sgpr >= loads.size()) {
                return data;
            }
            auto& attrib = data.attributes.emplace_back();
            attrib.semantic = semantic_index++;
            attrib.dest_vgpr = inst.src[1].code;
            attrib.num_elements = inst.control.mubuf.count;
            attrib.sgpr_base = loads[base_sgpr].base_sgpr;
            attrib.dword_offset = loads[base_sgpr].dword_offset;
            attrib.inst_offset = inst.control.mtbuf.offset;
            attrib.instance_data = inst.src[0].code;
            if (IsTypedBufferLoad(inst)) {
                attrib.data_format = inst.control.mtbuf.dfmt;
                attrib.num_format = inst.control.mtbuf.nfmt;
            }
        }
    }
    return data;
}

} // namespace Shader::Gcn
