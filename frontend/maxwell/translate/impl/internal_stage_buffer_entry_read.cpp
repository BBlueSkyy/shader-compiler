// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <shader_compiler/common/bit_field.h>
#include <shader_compiler/common/common_types.h>
#include <shader_compiler/frontend/maxwell/translate/impl/impl.h>
#include <shader_compiler/common/log.h>

namespace Shader::Maxwell {
namespace {
enum class Mode : u64 {
    Default,
    Patch,
    Prim,
    Attr,
};

enum class Shift : u64 {
    Default,
    U16,
    B32,
};

enum class ReadSize : u64 {
    U8,
    U16,
    U32,
    F32,
};

} // Anonymous namespace

void TranslatorVisitor::ISBERD(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_reg;
        BitField<24, 8, u32> imm;
        BitField<31, 1, u64> skew;
        BitField<32, 1, u64> o;
        BitField<33, 2, Mode> mode;
        BitField<36, 4, ReadSize> size;
        BitField<47, 2, Shift> shift;
    } const isberd{insn};

    if (isberd.o != 0) {
        // .O reads global memory using a 32-bit byte index, as in Eden.
        IR::U32 index{ir.Imm32(isberd.imm.Value())};
        if (isberd.src_reg != IR::Reg::RZ) {
            IR::U32 source{X(isberd.src_reg)};
            switch (isberd.shift) {
            case Shift::Default:
                break;
            case Shift::U16:
                source = ir.ShiftLeftLogical(source, ir.Imm32(1));
                break;
            case Shift::B32:
                source = ir.ShiftLeftLogical(source, ir.Imm32(2));
                break;
            default:
                throw NotImplementedException("Shift {}", isberd.shift.Value());
            }
            index = ir.IAdd(source, index);
        }
        if (isberd.skew != 0) {
            IR::U32 skew{ir.LaneId()};
            switch (isberd.size) {
            case ReadSize::U8:
                break;
            case ReadSize::U16:
                skew = ir.ShiftLeftLogical(skew, ir.Imm32(1));
                break;
            case ReadSize::U32:
            case ReadSize::F32:
                skew = ir.ShiftLeftLogical(skew, ir.Imm32(2));
                break;
            default:
                throw NotImplementedException("Read size {}", isberd.size.Value());
            }
            index = ir.IAdd(index, skew);
        }
        // Zero-extend only after the 32-bit index arithmetic has wrapped.
        const IR::U64 address{ir.UConvert(64, index)};
        switch (isberd.size) {
        case ReadSize::U8:
            X(isberd.dest_reg, ir.LoadGlobalU8(address));
            break;
        case ReadSize::U16:
            X(isberd.dest_reg, ir.LoadGlobalU16(address));
            break;
        case ReadSize::U32:
        case ReadSize::F32:
            X(isberd.dest_reg, ir.LoadGlobal32(address));
            break;
        default:
            throw NotImplementedException("Read size {}", isberd.size.Value());
        }
        return;
    }

    if (isberd.skew != 0) {
        throw NotImplementedException("SKEW");
    }
    if (isberd.mode != Mode::Default) {
        throw NotImplementedException("Mode {}", isberd.mode.Value());
    }
    if (isberd.shift != Shift::Default) {
        throw NotImplementedException("Shift {}", isberd.shift.Value());
    }
    LOG_WARNING(Shader, "(STUBBED) called");
    X(isberd.dest_reg, X(isberd.src_reg));
}

} // namespace Shader::Maxwell
