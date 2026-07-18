/*
 * Interpreted SRT/user-data flattening for QEMU and UWP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <cstring>
#include <unordered_map>

#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>

#include "bridge-memory.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/srt_gvn_table.h"

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8 *, size_t)
{
    return nullptr;
}

namespace Optimization {
namespace {

struct PassInfo {
    using PtrUserList = boost::container::flat_map<u32, IR::Inst *>;

    SrtGvnTable gvn_table;
    std::unordered_map<IR::Inst *, PtrUserList> pointer_uses;
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst *, 1> srt_roots;
    std::unordered_map<u32, IR::Inst *> vn_to_inst;
    u32 dst_off_dw = NUM_USER_DATA_REGS;

    PtrUserList *GetUsesAsPointer(IR::Inst *inst)
    {
        auto it = pointer_uses.find(inst);
        return it == pointer_uses.end() ? nullptr : &it->second;
    }

    IR::Inst *DeduplicateInstruction(IR::Inst *inst)
    {
        return vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst)
            .first->second;
    }
};

static uint64_t ReadPointer(uint64_t address)
{
    uint64_t pointer = 0;

    shadps4_gcn_bridge_read_memory(address, &pointer, sizeof(pointer));
    return pointer & UINT64_C(0xffffffffffff);
}

static u32 ReadDword(uint64_t address)
{
    u32 value = 0;

    shadps4_gcn_bridge_read_memory(address, &value, sizeof(value));
    return value;
}

static void VisitPointer(uint64_t pointer, IR::Inst *subtree,
                         PassInfo& pass, Info& info)
{
    PassInfo::PtrUserList *uses = pass.GetUsesAsPointer(subtree);

    if (!uses) {
        return;
    }
    for (const auto [src_off_dw, use] : *uses) {
        info.flattened_ud_buf.push_back(
            ReadDword(pointer + static_cast<uint64_t>(src_off_dw) * 4));
        use->SetFlags<u32>(pass.dst_off_dw++);
    }
    for (const auto [src_off_dw, use] : *uses) {
        if (pass.GetUsesAsPointer(use)) {
            VisitPointer(ReadPointer(pointer +
                                     static_cast<uint64_t>(src_off_dw) * 4),
                         use, pass, info);
        }
    }
}

} // namespace

void FlattenExtendedUserdataPass(IR::Program& program)
{
    Info& info = program.info;
    PassInfo pass;
    boost::container::small_vector<IR::Inst *, 32> all_readconsts;

    for (auto block_it = program.post_order_blocks.rbegin();
         block_it != program.post_order_blocks.rend(); ++block_it) {
        for (IR::Inst& inst : **block_it) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst ||
                !inst.Arg(1).IsImmediate()) {
                continue;
            }
            all_readconsts.push_back(&inst);
            if (pass.DeduplicateInstruction(&inst) != &inst) {
                continue;
            }

            IR::Inst *composite = inst.Arg(0).InstRecursive();
            const auto find_pointer = [](IR::Inst *candidate)
                    -> std::optional<IR::Inst *> {
                if (candidate->GetOpcode() == IR::Opcode::GetUserData ||
                    candidate->GetOpcode() == IR::Opcode::ReadConst) {
                    return candidate;
                }
                return std::nullopt;
            };
            auto base0 = IR::BreadthFirstSearch(composite->Arg(0), find_pointer);
            auto base1 = IR::BreadthFirstSearch(composite->Arg(1), find_pointer);
            if (!base0 || !base1) {
                continue;
            }

            IR::Inst *pointer = pass.DeduplicateInstruction(*base0);
            pass.pointer_uses.try_emplace(pointer, PassInfo::PtrUserList{})
                .first->second[inst.Arg(1).U32()] = &inst;
            if (pointer->GetOpcode() == IR::Opcode::GetUserData) {
                pass.srt_roots[pointer->Arg(0).ScalarReg()] = pointer;
            }
        }
    }

    info.flattened_ud_buf.assign(info.user_data.begin(), info.user_data.end());
    info.flattened_ud_buf.resize(NUM_USER_DATA_REGS);
    for (const auto& [sgpr_base, root] : pass.srt_roots) {
        const u32 base = static_cast<u32>(sgpr_base);
        uint64_t pointer = 0;

        if (base + 1 < info.user_data.size()) {
            pointer = static_cast<uint64_t>(info.user_data[base]) |
                      (static_cast<uint64_t>(info.user_data[base + 1]) << 32);
        }
        VisitPointer(pointer & UINT64_C(0xffffffffffff), root, pass, info);
    }
    info.srt_info.flattened_bufsize_dw = pass.dst_off_dw;

    for (IR::Inst *readconst : all_readconsts) {
        IR::Inst *original = pass.DeduplicateInstruction(readconst);
        readconst->SetFlags<u32>(original->Flags<u32>());
    }
}

} // namespace Optimization
} // namespace Shader
