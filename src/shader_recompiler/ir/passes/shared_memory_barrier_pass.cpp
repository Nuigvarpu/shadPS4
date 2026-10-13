// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include "common/logging/log.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block, bool subgroup = false) {
    const auto emit_barrier = [subgroup](IR::IREmitter& ir) {
        if (subgroup) {
            ir.SubgroupBarrier();
        } else {
            ir.Barrier();
        }
    };
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                emit_barrier(ir);
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                emit_barrier(ir);
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        emit_barrier(ir);
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

static bool IsDivergent(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth) {
    const IR::U1 cond = data.if_node.cond;
    if (IsDivergent(cond)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list) {
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (IsDivergent(node.data.repeat.cond)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (IsDivergent(node.data.break_node.cond)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// Shared memory is also synchronized with workgroup barriers in workgroups larger than one GCN
// wave when the host subgroup is narrower than 64 lanes. This keeps inFAMOUS Second Son's exposure
// from pulsing on NVIDIA. GCN code may rely on the 64 lanes of a wave running in lockstep even in
// large workgroups (per-wave scratch areas, reduction tails), which does not hold when a wave is
// split across two host subgroups. Inside divergent control flow, where a workgroup barrier is not
// allowed, subgroup barriers keep at least the order within each host subgroup.

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (shared_memory_size == 0) {
        return;
    }
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup.
    const bool extended = profile.needs_lds_barriers && threadgroup_size > GcnSubgroupSize;
    if ((threadgroup_size != GcnSubgroupSize || !profile.needs_lds_barriers) && !extended) {
        // GCN waves can use shared memory between invocations without explicit barriers because
        // they run in lockstep. Host compilers can reorder these accesses, so keep their order
        // with subgroup memory barriers.
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == IR::AbstractSyntaxNode::Type::Block) {
                EmitBarrierInBlock(node.data.block, true);
            }
        }
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    u32 divergent_lds_blocks{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
            }
            continue;
        }
        if (node.type == Type::Block) {
            if (divergence_depth == 0) {
                EmitBarrierInBlock(node.data.block);
            } else if (extended) {
                const bool uses_lds =
                    std::ranges::any_of(node.data.block->Instructions(), [](const IR::Inst& inst) {
                        return IsLoadShared(inst) || IsWriteShared(inst);
                    });
                if (uses_lds) {
                    ++divergent_lds_blocks;
                }
                EmitBarrierInBlock(node.data.block, true);
            }
        }
    }
    if (divergent_lds_blocks != 0) {
        // Only subgroup barriers are allowed here, which do not order the two host subgroups
        // that make up one GCN wave.
        LOG_DEBUG(Render_Recompiler,
                  "LDS sync: {} divergent blocks use shared memory in compute shader {:#x} "
                  "(workgroup {}x{}x{})",
                  divergent_lds_blocks, program.info.pgm_hash, cs_info.workgroup_size[0],
                  cs_info.workgroup_size[1], cs_info.workgroup_size[2]);
    }
}

static bool IsBufferLoad(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::LoadBufferU32:
    case IR::Opcode::LoadBufferU32x2:
    case IR::Opcode::LoadBufferU32x3:
    case IR::Opcode::LoadBufferU32x4:
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::LoadBufferF32:
    case IR::Opcode::LoadBufferF32x2:
    case IR::Opcode::LoadBufferF32x3:
    case IR::Opcode::LoadBufferF32x4:
    case IR::Opcode::LoadBufferFormatF32:
        return true;
    default:
        return false;
    }
}

static bool IsBufferStore(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::StoreBufferU8:
    case IR::Opcode::StoreBufferU16:
    case IR::Opcode::StoreBufferU32:
    case IR::Opcode::StoreBufferU32x2:
    case IR::Opcode::StoreBufferU32x3:
    case IR::Opcode::StoreBufferU32x4:
    case IR::Opcode::StoreBufferU64:
    case IR::Opcode::StoreBufferF32:
    case IR::Opcode::StoreBufferF32x2:
    case IR::Opcode::StoreBufferF32x3:
    case IR::Opcode::StoreBufferF32x4:
    case IR::Opcode::StoreBufferFormatF32:
        return true;
    default:
        return false;
    }
}

static std::optional<u32> BufferHandle(const IR::Inst& inst) {
    const IR::Value handle = inst.Arg(0);
    if (!handle.IsImmediate()) {
        return std::nullopt;
    }
    return handle.U32();
}

static void EmitWaveSync(IR::Block* block, IR::Block::iterator where) {
    IR::IREmitter ir{*block, where};
    ir.WorkgroupMemoryBarrier();
    ir.Barrier();
    ir.WorkgroupMemoryBarrier();
}

// A compute workgroup of exactly one GCN wave runs in lockstep on hardware, so after a buffer
// store (and s_waitcnt) every lane can read what the other 63 lanes wrote without a barrier.
// GPU sorts (merge path and similar) rely on this. When the host subgroup is narrower than 64
// lanes the workgroup is split across subgroups that do not run in lockstep, so a lane can read
// the old contents before another subgroup has written them, duplicating and dropping elements.
// Insert workgroup barriers between a buffer store and a later load of the same buffer.
void WaveGlobalMemorySyncPass(IR::Program& program, const RuntimeInfo& runtime_info,
                              const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || !profile.needs_lds_barriers) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (threadgroup_size != GcnSubgroupSize) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;

    // Buffers loaded inside each loop, keyed by the loop merge block.
    std::unordered_map<const IR::Block*, std::unordered_set<u32>> loop_loads;
    {
        std::vector<std::unordered_set<u32>> stack;
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == Type::Loop) {
                stack.emplace_back();
            } else if (node.type == Type::Block) {
                for (const IR::Inst& inst : node.data.block->Instructions()) {
                    if (IsBufferLoad(inst)) {
                        if (const auto handle = BufferHandle(inst)) {
                            for (auto& set : stack) {
                                set.insert(*handle);
                            }
                        }
                    }
                }
            } else if (node.type == Type::Repeat && !stack.empty()) {
                loop_loads[node.data.repeat.merge] = std::move(stack.back());
                stack.pop_back();
            }
        }
    }

    // Buffers loaded by each block, in program order, and by every later block.
    std::vector<IR::Block*> blocks;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::Block) {
            blocks.push_back(node.data.block);
        }
    }
    std::unordered_map<const IR::Block*, std::unordered_set<u32>> loads_after;
    {
        std::unordered_set<u32> suffix;
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            loads_after[*it] = suffix;
            for (const IR::Inst& inst : (*it)->Instructions()) {
                if (IsBufferLoad(inst)) {
                    if (const auto handle = BufferHandle(inst)) {
                        suffix.insert(*handle);
                    }
                }
            }
        }
    }

    // Place the barrier right after the last store that a later read depends on. That point is
    // reached by every invocation that stored, unlike the (often divergent) reads themselves.
    std::vector<const std::unordered_set<u32>*> loops;
    u32 barriers = 0;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::Loop) {
            const auto it = loop_loads.find(node.data.loop.merge);
            loops.push_back(it != loop_loads.end() ? &it->second : nullptr);
            continue;
        }
        if (node.type == Type::Repeat) {
            if (!loops.empty()) {
                loops.pop_back();
            }
            continue;
        }
        if (node.type != Type::Block) {
            continue;
        }
        IR::Block* block = node.data.block;
        const auto& later = loads_after[block];
        const auto depends = [&](u32 handle) {
            if (later.contains(handle)) {
                return true;
            }
            return std::ranges::any_of(loops, [&](const std::unordered_set<u32>* set) {
                return set != nullptr && set->contains(handle);
            });
        };
        auto last_store = block->end();
        for (auto it = block->begin(); it != block->end(); ++it) {
            if (IsBufferStore(*it)) {
                if (const auto handle = BufferHandle(*it); handle && depends(*handle)) {
                    last_store = it;
                }
            }
        }
        if (last_store != block->end()) {
            EmitWaveSync(block, std::next(last_store));
            ++barriers;
        }
    }
    if (barriers != 0) {
        LOG_DEBUG(Render_Recompiler, "Wave sync: {} barriers in compute shader {:#x}", barriers,
                  program.info.pgm_hash);
    }
}

} // namespace Shader::Optimization
