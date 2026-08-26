// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <shader_compiler/common/bit_field.h>
#include <shader_compiler/common/common_types.h>
#include <shader_compiler/shader_info.h>

namespace Shader::IR {

enum class FmzMode : u8 {
    DontCare, // Not specified for this instruction
    FTZ,      // Flush denorms to zero, NAN is propagated (D3D11, NVN, GL, VK)
    FMZ,      // Flush denorms to zero, x * 0 == 0 (D3D9)
    None,     // Denorms are not flushed, NAN is propagated (nouveau)
};

enum class FpRounding : u8 {
    DontCare, // Not specified for this instruction
    RN,       // Round to nearest even,
    RM,       // Round towards negative infinity
    RP,       // Round towards positive infinity
    RZ,       // Round towards zero
};

struct FpControl {
    bool no_contraction{false};
    FpRounding rounding{FpRounding::DontCare};
    FmzMode fmz_mode{FmzMode::DontCare};
};
static_assert(sizeof(FpControl) <= sizeof(u32));

enum class TextureCompareFunc : u32 {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessOrEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterOrEqual = 6,
    Always = 7,
};
union TextureInstInfo {
    u32 raw;
    BitField<0, 16, u32> descriptor_index;
    BitField<16, 3, TextureType> type;
    BitField<19, 1, u32> is_depth;
    BitField<20, 1, u32> has_bias;
    BitField<21, 1, u32> has_lod_clamp;
    BitField<22, 1, u32> relaxed_precision;
    BitField<23, 2, u32> gather_component;
    BitField<25, 2, u32> num_derivates;
    BitField<27, 3, ImageFormat> image_format;
    // Dref instructions do not consume image_format, so the top bit of that field can be
    // shared to preserve all eight sampler comparison functions without growing this union.
    BitField<29, 3, TextureCompareFunc> compare_func;
};
static_assert(sizeof(TextureInstInfo) <= sizeof(u32));

} // namespace Shader::IR
