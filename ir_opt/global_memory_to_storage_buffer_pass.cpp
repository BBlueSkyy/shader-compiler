// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <range/v3/algorithm.hpp>
#include <optional>
#include "structured_ssbo_bounds.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>

#include <shader_compiler/common/alignment.h>
#include <shader_compiler/frontend/ir/basic_block.h>
#include <shader_compiler/frontend/ir/breadth_first_search.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include <shader_compiler/frontend/ir/value.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/ir_opt/passes.h>

namespace Shader::Optimization {
namespace {
/// Address in constant buffers to the storage buffer descriptor
struct StorageBufferAddr {
    auto operator<=>(const StorageBufferAddr&) const noexcept = default;

    u32 index;
    u32 offset;
};

/// Block iterator to a global memory instruction and the storage buffer it uses
struct StorageInst {
    StorageBufferAddr storage_buffer;
    IR::Inst* inst;
    IR::Block* block;
};

/// Bias towards a certain range of constant buffers when looking for storage buffers
struct Bias {
    u32 index;
    u32 offset_begin;
    u32 offset_end;
};

using boost::container::flat_set;
using boost::container::small_vector;
using StorageBufferSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;
using StorageInstVector = small_vector<StorageInst, 24>;
using StorageWritesSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;

struct StorageInfo {
    StorageBufferSet set;
    StorageInstVector to_replace;
    StorageWritesSet writes;
};

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemory(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemoryWrite(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Converts a global memory opcode to its storage buffer equivalent
IR::Opcode GlobalToStorage(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalS8:
        return IR::Opcode::LoadStorageS8;
    case IR::Opcode::LoadGlobalU8:
        return IR::Opcode::LoadStorageU8;
    case IR::Opcode::LoadGlobalS16:
        return IR::Opcode::LoadStorageS16;
    case IR::Opcode::LoadGlobalU16:
        return IR::Opcode::LoadStorageU16;
    case IR::Opcode::LoadGlobal32:
        return IR::Opcode::LoadStorage32;
    case IR::Opcode::LoadGlobal64:
        return IR::Opcode::LoadStorage64;
    case IR::Opcode::LoadGlobal128:
        return IR::Opcode::LoadStorage128;
    case IR::Opcode::WriteGlobalS8:
        return IR::Opcode::WriteStorageS8;
    case IR::Opcode::WriteGlobalU8:
        return IR::Opcode::WriteStorageU8;
    case IR::Opcode::WriteGlobalS16:
        return IR::Opcode::WriteStorageS16;
    case IR::Opcode::WriteGlobalU16:
        return IR::Opcode::WriteStorageU16;
    case IR::Opcode::WriteGlobal32:
        return IR::Opcode::WriteStorage32;
    case IR::Opcode::WriteGlobal64:
        return IR::Opcode::WriteStorage64;
    case IR::Opcode::WriteGlobal128:
        return IR::Opcode::WriteStorage128;
    case IR::Opcode::GlobalAtomicIAdd32:
        return IR::Opcode::StorageAtomicIAdd32;
    case IR::Opcode::GlobalAtomicSMin32:
        return IR::Opcode::StorageAtomicSMin32;
    case IR::Opcode::GlobalAtomicUMin32:
        return IR::Opcode::StorageAtomicUMin32;
    case IR::Opcode::GlobalAtomicSMax32:
        return IR::Opcode::StorageAtomicSMax32;
    case IR::Opcode::GlobalAtomicUMax32:
        return IR::Opcode::StorageAtomicUMax32;
    case IR::Opcode::GlobalAtomicInc32:
        return IR::Opcode::StorageAtomicInc32;
    case IR::Opcode::GlobalAtomicDec32:
        return IR::Opcode::StorageAtomicDec32;
    case IR::Opcode::GlobalAtomicAnd32:
        return IR::Opcode::StorageAtomicAnd32;
    case IR::Opcode::GlobalAtomicOr32:
        return IR::Opcode::StorageAtomicOr32;
    case IR::Opcode::GlobalAtomicXor32:
        return IR::Opcode::StorageAtomicXor32;
    case IR::Opcode::GlobalAtomicExchange32:
        return IR::Opcode::StorageAtomicExchange32;
    case IR::Opcode::GlobalAtomicIAdd64:
        return IR::Opcode::StorageAtomicIAdd64;
    case IR::Opcode::GlobalAtomicSMin64:
        return IR::Opcode::StorageAtomicSMin64;
    case IR::Opcode::GlobalAtomicUMin64:
        return IR::Opcode::StorageAtomicUMin64;
    case IR::Opcode::GlobalAtomicSMax64:
        return IR::Opcode::StorageAtomicSMax64;
    case IR::Opcode::GlobalAtomicUMax64:
        return IR::Opcode::StorageAtomicUMax64;
    case IR::Opcode::GlobalAtomicAnd64:
        return IR::Opcode::StorageAtomicAnd64;
    case IR::Opcode::GlobalAtomicOr64:
        return IR::Opcode::StorageAtomicOr64;
    case IR::Opcode::GlobalAtomicXor64:
        return IR::Opcode::StorageAtomicXor64;
    case IR::Opcode::GlobalAtomicExchange64:
        return IR::Opcode::StorageAtomicExchange64;
    case IR::Opcode::GlobalAtomicIAdd32x2:
        return IR::Opcode::StorageAtomicIAdd32x2;
    case IR::Opcode::GlobalAtomicSMin32x2:
        return IR::Opcode::StorageAtomicSMin32x2;
    case IR::Opcode::GlobalAtomicUMin32x2:
        return IR::Opcode::StorageAtomicUMin32x2;
    case IR::Opcode::GlobalAtomicSMax32x2:
        return IR::Opcode::StorageAtomicSMax32x2;
    case IR::Opcode::GlobalAtomicUMax32x2:
        return IR::Opcode::StorageAtomicUMax32x2;
    case IR::Opcode::GlobalAtomicAnd32x2:
        return IR::Opcode::StorageAtomicAnd32x2;
    case IR::Opcode::GlobalAtomicOr32x2:
        return IR::Opcode::StorageAtomicOr32x2;
    case IR::Opcode::GlobalAtomicXor32x2:
        return IR::Opcode::StorageAtomicXor32x2;
    case IR::Opcode::GlobalAtomicExchange32x2:
        return IR::Opcode::StorageAtomicExchange32x2;
    case IR::Opcode::GlobalAtomicAddF32:
        return IR::Opcode::StorageAtomicAddF32;
    case IR::Opcode::GlobalAtomicAddF16x2:
        return IR::Opcode::StorageAtomicAddF16x2;
    case IR::Opcode::GlobalAtomicMinF16x2:
        return IR::Opcode::StorageAtomicMinF16x2;
    case IR::Opcode::GlobalAtomicMaxF16x2:
        return IR::Opcode::StorageAtomicMaxF16x2;
    case IR::Opcode::GlobalAtomicAddF32x2:
        return IR::Opcode::StorageAtomicAddF32x2;
    case IR::Opcode::GlobalAtomicMinF32x2:
        return IR::Opcode::StorageAtomicMinF32x2;
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return IR::Opcode::StorageAtomicMaxF32x2;
    default:
        throw InvalidArgument("Invalid global memory opcode {}", opcode);
    }
}

/// Returns true when a storage buffer address satisfies a bias
bool MeetsBias(const StorageBufferAddr& storage_buffer, const Bias& bias) noexcept {
    return storage_buffer.index == bias.index && storage_buffer.offset >= bias.offset_begin &&
           storage_buffer.offset < bias.offset_end;
}

struct LowAddrInfo {
    IR::U32 value;
    s32 imm_offset;
    IR::U32 high;
};

/// Tries to track the first 32-bits of a global memory instruction
std::optional<LowAddrInfo> TrackLowAddress(IR::Inst* inst) {
    // The first argument is the low level GPU pointer to the global memory instruction
    const IR::Value addr{inst->Arg(0)};
    if (addr.IsImmediate()) {
        // Not much we can do if it's an immediate
        return std::nullopt;
    }
    // This address is expected to either be a PackUint2x32, a IAdd64, or a CompositeConstructU32x2
    IR::Inst* addr_inst{addr.InstRecursive()};
    s32 imm_offset{0};
    if (addr_inst->GetOpcode() == IR::Opcode::IAdd64) {
        // If it's an IAdd64, get the immediate offset it is applying and grab the address
        // instruction. This expects for the instruction to be canonicalized having the address on
        // the first argument and the immediate offset on the second one.
        const IR::U64 imm_offset_value{addr_inst->Arg(1)};
        if (!imm_offset_value.IsImmediate()) {
            return std::nullopt;
        }
        imm_offset = static_cast<s32>(static_cast<s64>(imm_offset_value.U64()));
        const IR::U64 iadd_addr{addr_inst->Arg(0)};
        if (iadd_addr.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = iadd_addr.InstRecursive();
    }
    // With IAdd64 handled, now PackUint2x32 is expected
    if (addr_inst->GetOpcode() == IR::Opcode::PackUint2x32) {
        // PackUint2x32 is expected to be generated from a vector
        const IR::Value vector{addr_inst->Arg(0)};
        if (vector.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = vector.InstRecursive();
    }
    // The vector is expected to be a CompositeConstructU32x2
    if (addr_inst->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
        return std::nullopt;
    }
    // Grab the first argument from the CompositeConstructU32x2, this is the low address.
    return LowAddrInfo{
        .value{IR::U32{addr_inst->Arg(0)}},
        .imm_offset = imm_offset,
        .high{IR::U32{addr_inst->Arg(1)}},
    };
}

/// Tries to track the storage buffer address used by a global memory instruction
std::optional<StorageBufferAddr> Track(const IR::Value& value, const IR::Value& high, const Bias* bias,
                                     u32 pointer_alignment = 16) {
    const auto pred{[bias, &high, pointer_alignment](const IR::Inst* inst) -> std::optional<StorageBufferAddr> {
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 &&
            inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return std::nullopt;
        }
        const IR::Value index{inst->Arg(0)};
        const IR::Value offset{inst->Arg(1)};
        if (!index.IsImmediate()) {
            // Definitely not a storage buffer if it's read from a
            // non-immediate index
            return std::nullopt;
        }
        if (!offset.IsImmediate()) {
            // TODO: Support SSBO arrays
            return std::nullopt;
        }
        const StorageBufferAddr storage_buffer{
            .index = index.U32(),
            .offset = offset.U32(),
        };
        if (!Common::IsAligned(storage_buffer.offset, bias ? 16 : pointer_alignment)) {
            // NVN records are 16-byte aligned; standalone 64-bit pointers need only 8.
            return std::nullopt;
        }
        if (bias && !MeetsBias(storage_buffer, *bias)) {
            // We have to blacklist some addresses in case we wrongly
            // point to them
            return std::nullopt;
        }
        if (!bias && pointer_alignment == 8) {
            // Preserve legacy 16-byte candidates, including low32-only pointers.
            // A scalar offset can also be cbuf-aligned. Confirm the adjacent high
            // word belongs to this pointer before admitting an unbiased candidate.
            const auto high_word{BreadthFirstSearch(high, [&](const IR::Inst* candidate) -> std::optional<bool> {
                const auto opcode{candidate->GetOpcode()};
                if (opcode != IR::Opcode::GetCbufU32 && opcode != IR::Opcode::GetCbufU32x2)
                    return std::nullopt;
                if (!candidate->Arg(0).IsImmediate() || !candidate->Arg(1).IsImmediate())
                    return std::nullopt;
                const auto expected{storage_buffer.offset + (opcode == IR::Opcode::GetCbufU32 ? 4U : 0U)};
                if (candidate->Arg(0).U32() == storage_buffer.index && candidate->Arg(1).U32() == expected)
                    return true;
                return std::nullopt;
            })};
            if (!high_word) return std::nullopt;
        }
        return storage_buffer;
    }};
    return BreadthFirstSearch(value, pred);
}

// Infer a conservative affine offset, replacing only the selected pointer with zero.
// Unrecognized expressions stay unknown; no guessed stride or game-specific layout.
struct AffineOffsetRange : ComputeStorageBufferAccessRange {
    u64 pointer_coefficient{};
    bool exact_constant{}; // Scalar bounds cannot scale the symbolic pointer base.
};

std::optional<AffineOffsetRange>
TrackOffsetRange(const IR::Value &value, StorageBufferAddr pointer, u32 &budget, u32 depth = 0,
                 const StructuredSsboBounds *flow = nullptr,
                 const StructuredSsboBounds::Facts *facts = nullptr) {
    constexpr u64 limit{1ULL << 32};
    if (depth > 64 || !budget)
        return std::nullopt;
    --budget; // Shared SSA subtrees must not cause exponential traversal.
    AffineOffsetRange result;
    result.known = true;
    if (value.IsImmediate()) {
        if (value.Type() != IR::Type::U32)
            return std::nullopt;
        result.constant = value.U32();
        result.exact_constant = true;
        return result;
    }
    const IR::Inst *inst{value.InstRecursive()};
    switch (inst->GetOpcode()) {
    case IR::Opcode::BitFieldUExtract: {
        if (!flow || !inst->Arg(1).IsImmediate() || !inst->Arg(2).IsImmediate())
            return std::nullopt;
        const u32 shift{inst->Arg(1).U32()}, bits{inst->Arg(2).U32()};
        if (shift >= 32 || !bits || bits > 32 - shift)
            return std::nullopt;
        const auto input = TrackOffsetRange(inst->Arg(0), pointer, budget, depth + 1, flow, facts);
        if (!input || input->pointer_coefficient || input->constant >= limit)
            return std::nullopt;
        for (size_t axis{}; axis < 3; ++axis)
            if (input->workgroup[axis] || input->local[axis])
                return std::nullopt;
        result.constant = (std::min)(input->constant >> shift, (u64{1} << bits) - 1);
        return result;
    }
    case IR::Opcode::Phi: {
        if (!flow || !flow->Valid() || !facts || inst->Type() != IR::Type::U32)
            return std::nullopt;
        if (const auto bound = flow->InductionBound(inst)) {
            result.constant = *bound;
            return result;
        }
        if (flow->HasBackedge(inst))
            return std::nullopt;
        std::optional<AffineOffsetRange> combined;
        for (size_t i{}; i < inst->NumArgs(); ++i) {
            const auto incoming = flow->Incoming(inst, inst->PhiBlock(i), *facts);
            if (!incoming)
                continue;
            const auto range =
                TrackOffsetRange(inst->Arg(i), pointer, budget, depth + 1, flow, &*incoming);
            if (!range)
                return std::nullopt;
            if (!combined) {
                combined = range;
                continue;
            }
            if (combined->pointer_coefficient != range->pointer_coefficient)
                return std::nullopt;
            combined->exact_constant &=
                range->exact_constant && combined->constant == range->constant;
            combined->constant = (std::max)(combined->constant, range->constant);
            for (size_t axis{}; axis < 3; ++axis) {
                combined->workgroup[axis] =
                    (std::max)(combined->workgroup[axis], range->workgroup[axis]);
                combined->local[axis] = (std::max)(combined->local[axis], range->local[axis]);
            }
        }
        return combined;
    }
    case IR::Opcode::GetCbufU32:
        if (!inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate())
            return std::nullopt;
        if (inst->Arg(0).U32() == pointer.index && inst->Arg(1).U32() == pointer.offset)
            result.pointer_coefficient = 1;
        else
            result.constant = limit - 1; // Other cbuf values are U32 parameters, not pointer bases.
        return result;
    case IR::Opcode::CompositeExtractU32x3: {
        if (!inst->Arg(1).IsImmediate() || inst->Arg(0).IsImmediate())
            return std::nullopt;
        const auto axis{inst->Arg(1).U32()};
        if (axis >= 3)
            return std::nullopt;
        const auto vector_op{inst->Arg(0).InstRecursive()->GetOpcode()};
        if (vector_op == IR::Opcode::WorkgroupId)
            result.workgroup[axis] = 1;
        else if (vector_op == IR::Opcode::LocalInvocationId)
            result.local[axis] = 1;
        else
            return std::nullopt;
        return result;
    }
    case IR::Opcode::IAdd32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32: {
        auto lhs{TrackOffsetRange(inst->Arg(0), pointer, budget, depth + 1, flow, facts)};
        if (!lhs)
            return std::nullopt;
        std::optional<AffineOffsetRange> rhs;
        if (inst->GetOpcode() == IR::Opcode::ShiftLeftLogical32) {
            if (!inst->Arg(1).IsImmediate() || inst->Arg(1).U32() >= 32)
                return std::nullopt;
            rhs = AffineOffsetRange{};
            rhs->known = true;
            rhs->constant = 1ULL << inst->Arg(1).U32();
            rhs->exact_constant = true;
        } else {
            rhs = TrackOffsetRange(inst->Arg(1), pointer, budget, depth + 1, flow, facts);
        }
        if (!rhs)
            return std::nullopt;
        auto has_axes = [](const ComputeStorageBufferAccessRange &range) {
            for (size_t axis{}; axis < 3; ++axis)
                if (range.workgroup[axis] || range.local[axis])
                    return true;
            return false;
        };
        if (inst->GetOpcode() != IR::Opcode::IAdd32) {
            if (has_axes(*rhs) || rhs->pointer_coefficient)
                std::swap(lhs, rhs);
            if (has_axes(*rhs) || rhs->pointer_coefficient)
                return std::nullopt; // Not an affine product.
            if (lhs->pointer_coefficient && !rhs->exact_constant)
                return std::nullopt;
            lhs->exact_constant &= rhs->exact_constant;
            const u64 factor{rhs->constant};
            auto mul = [factor](u64 x) {
                return factor && x > limit / factor ? limit : x * factor;
            };
            lhs->constant = mul(lhs->constant);
            lhs->pointer_coefficient = mul(lhs->pointer_coefficient);
            for (size_t axis{}; axis < 3; ++axis) {
                lhs->workgroup[axis] = mul(lhs->workgroup[axis]);
                lhs->local[axis] = mul(lhs->local[axis]);
            }
        } else {
            lhs->exact_constant &= rhs->exact_constant;
            lhs->constant = (std::min)(limit, lhs->constant + rhs->constant);
            lhs->pointer_coefficient =
                (std::min)(limit, lhs->pointer_coefficient + rhs->pointer_coefficient);
            for (size_t axis{}; axis < 3; ++axis) {
                lhs->workgroup[axis] =
                    (std::min)(limit, lhs->workgroup[axis] + rhs->workgroup[axis]);
                lhs->local[axis] = (std::min)(limit, lhs->local[axis] + rhs->local[axis]);
            }
        }
        lhs->exact_constant &= lhs->constant < limit;
        return lhs;
    }
    default:
        return std::nullopt;
    }
}

u32 GlobalAccessWidth(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalU8: case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::WriteGlobalU8: case IR::Opcode::WriteGlobalS8: return 1;
    case IR::Opcode::LoadGlobalU16: case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::WriteGlobalU16: case IR::Opcode::WriteGlobalS16: return 2;
    case IR::Opcode::LoadGlobal128: case IR::Opcode::WriteGlobal128: return 16;
    case IR::Opcode::LoadGlobal32: case IR::Opcode::WriteGlobal32: return 4;
    case IR::Opcode::LoadGlobal64: case IR::Opcode::WriteGlobal64: return 8;
    default: return 0; // Atomics retain the generic unknown-size fallback.
    }
}

ComputeStorageBufferAccessRange CollectAccessRange(StorageBufferAddr pointer, const StorageInfo& info,
                                                  const StructuredSsboBounds* flow = nullptr) {
    ComputeStorageBufferAccessRange result{.known = true};
    for (const auto& access : info.to_replace) {
        if (access.storage_buffer != pointer) continue;
        const auto low{TrackLowAddress(access.inst)};
        const auto width{GlobalAccessWidth(access.inst->GetOpcode())};
        if (!low || low->imm_offset < 0 || !width) return {};
        u32 budget{flow ? 4096U : 512U};
        const auto facts = flow ? flow->At(access.block) : StructuredSsboBounds::Facts{};
        auto range{TrackOffsetRange(low->value, pointer, budget, 0, flow, &facts)};
        if (!range || range->pointer_coefficient != 1) return {};
        range->constant += static_cast<u32>(low->imm_offset) + width;
        result.constant = (std::max)(result.constant, range->constant);
        for (size_t axis{}; axis < 3; ++axis) {
            result.workgroup[axis] = (std::max)(result.workgroup[axis], range->workgroup[axis]);
            result.local[axis] = (std::max)(result.local[axis], range->local[axis]);
        }
    }
    return result;
}

/// Collects the storage buffer used by a global memory instruction and the instruction itself
void CollectStorageBuffers(IR::Block& block, IR::Inst& inst, StorageInfo& info) {
    // NVN puts storage buffers in a specific range, we have to bias towards these addresses to
    // avoid getting false positives
    static constexpr Bias nvn_bias{
        .index = 0,
        .offset_begin = 0x110,
        .offset_end = 0x610,
    };
    // Track the low address of the instruction
    const std::optional<LowAddrInfo> low_addr_info{TrackLowAddress(&inst)};
    if (!low_addr_info) {
        // Failed to track the low address, use NVN fallbacks
        return;
    }
    // First try to find storage buffers in the NVN address
    const IR::U32 low_addr{low_addr_info->value};
    std::optional<StorageBufferAddr> storage_buffer{Track(low_addr, low_addr_info->high, &nvn_bias)};
    if (!storage_buffer) {
        // If it fails, track without a bias
        storage_buffer = Track(low_addr, low_addr_info->high, nullptr);
        if (!storage_buffer) {
            // Preserve preference for existing 16-byte candidates. Admit 8-byte
            // pointers only after their adjacent high word has been confirmed.
            storage_buffer = Track(low_addr, low_addr_info->high, nullptr, 8);
            if (!storage_buffer) return;
        }
    }
    // Collect storage buffer and the instruction
    if (IsGlobalMemoryWrite(inst)) {
        info.writes.insert(*storage_buffer);
    }
    info.set.insert(*storage_buffer);
    info.to_replace.push_back(StorageInst{
        .storage_buffer{*storage_buffer},
        .inst = &inst,
        .block = &block,
    });
}

/// Returns the offset in indices (not bytes) for an equivalent storage instruction
IR::U32 StorageOffset(IR::Block& block, IR::Inst& inst, StorageBufferAddr buffer, u32 alignment) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    IR::U32 offset;
    if (const std::optional<LowAddrInfo> low_addr{TrackLowAddress(&inst)}) {
        offset = low_addr->value;
        if (low_addr->imm_offset != 0) {
            offset = ir.IAdd(offset, ir.Imm32(low_addr->imm_offset));
        }
    } else {
        offset = ir.UConvert(32, IR::U64{inst.Arg(0)});
    }
    // Subtract the least significant 32 bits from the guest offset. The result is the storage
    // buffer offset in bytes.
    IR::U32 low_cbuf{ir.GetCbuf(ir.Imm32(buffer.index), ir.Imm32(buffer.offset))};

    // Align the offset base to match the host alignment requirements
    low_cbuf = ir.BitwiseAnd(low_cbuf, ir.Imm32(~(alignment - 1U)));
    return ir.ISub(offset, low_cbuf);
}

/// Replace a global memory load instruction with its storage buffer equivalent
void ReplaceLoad(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                 const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{&*block.PrependNewInst(it, new_opcode, {storage_index, offset})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory write instruction with its storage buffer equivalent
void ReplaceWrite(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                  const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)});
    inst.Invalidate();
}

/// Replace an atomic operation on global memory instruction with its storage buffer equivalent
void ReplaceAtomic(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                   const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{
        &*block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory instruction with its storage buffer equivalent
void Replace(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
             const IR::U32& offset) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
        return ReplaceLoad(block, inst, storage_index, offset);
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
        return ReplaceWrite(block, inst, storage_index, offset);
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return ReplaceAtomic(block, inst, storage_index, offset);
    default:
        throw InvalidArgument("Invalid global memory opcode {}", inst.GetOpcode());
    }
}
} // Anonymous namespace

void GlobalMemoryToStorageBufferPass(IR::Program& program, const HostTranslateInfo& host_info) {
    StorageInfo info;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsGlobalMemory(inst)) {
                continue;
            }
            CollectStorageBuffers(*block, inst, info);
        }
    }
    std::optional<StructuredSsboBounds> flow;
    for (const StorageBufferAddr& storage_buffer : info.set) {
        const bool hasSize{IsNVNStorageBufferDescriptor(program.stage, storage_buffer.index, storage_buffer.offset)};
        auto range{program.stage == Stage::Compute ? CollectAccessRange(storage_buffer, info) : ComputeStorageBufferAccessRange{}};
        if (program.stage == Stage::Compute && !hasSize && !range.known) {
            if (!flow) flow.emplace(program);
            if (flow->Valid()) range = CollectAccessRange(storage_buffer, info, &*flow);
        }
        program.info.storage_buffers_descriptors.push_back({
            .cbuf_index = storage_buffer.index,
            .cbuf_offset = storage_buffer.offset,
            .count = 1,
            .is_written = info.writes.contains(storage_buffer),
            .has_size = hasSize,
            .access_range = range,
        });
    }
    for (const StorageInst &storage_inst : info.to_replace) {
        const StorageBufferAddr storage_buffer{storage_inst.storage_buffer};
        const auto it{info.set.find(storage_inst.storage_buffer)};
        const IR::U32 index{IR::Value{static_cast<u32>(info.set.index_of(it))}};
        IR::Block *const block{storage_inst.block};
        IR::Inst *const inst{storage_inst.inst};
        const IR::U32 offset{
            StorageOffset(*block, *inst, storage_buffer, host_info.min_ssbo_alignment)};
        Replace(*block, *inst, index, offset);
    }
}

template <typename Descriptors, typename Descriptor, typename Func>
static u32 Add(Descriptors &descriptors, const Descriptor &desc, Func &&pred) {
    // TODO: Handle arrays
    const auto it{ranges::find_if(descriptors, pred)};
    if (it != descriptors.end()) {
        return static_cast<u32>(std::distance(descriptors.begin(), it));
    }
    descriptors.push_back(desc);
    return static_cast<u32>(descriptors.size()) - 1;
}

void JoinStorageInfo(Info& base, Info& source) {
    auto& descriptors = base.storage_buffers_descriptors;
    for (auto& desc : source.storage_buffers_descriptors) {
        auto it{ranges::find_if(descriptors, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count;
        })};
        if (it != descriptors.end()) {
            it->is_written |= desc.is_written;
            continue;
        }
        descriptors.push_back(desc);
    }
}

} // namespace Shader::Optimization
