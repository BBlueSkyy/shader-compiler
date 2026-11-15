// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <shader_compiler/common/common_types.h>
#include <shader_compiler/program_header.h>
#include <shader_compiler/runtime_info.h>
#include <shader_compiler/shader_info.h>
#include <shader_compiler/stage.h>

namespace Shader {

struct TextureHandleSource {
    u32 cbuf_index, cbuf_offset, shift_left;
    u32 secondary_cbuf_index, secondary_cbuf_offset, secondary_shift_left;
};
enum class TextureQueryKind { Type, PixelFormat, Integer, Compare };

class Environment {
public:
    virtual ~Environment() = default;

    [[nodiscard]] virtual u64 ReadInstruction(u32 address) = 0;

    [[nodiscard]] virtual u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) = 0;

    [[nodiscard]] virtual TextureType ReadTextureType(u32 raw_handle) = 0;

    [[nodiscard]] virtual CompareFunction ReadTextureCompareFunc(u32 raw_handle) = 0;

    [[nodiscard]] virtual TexturePixelFormat ReadTexturePixelFormat(u32 raw_handle) = 0;

    [[nodiscard]] virtual bool IsTexturePixelFormatInteger(u32 raw_handle) = 0;

    // Metadata queries keep handle origins separate from CBUF values folded
    // into code. The default preserves the exact legacy reads and result.
    [[nodiscard]] virtual u32 QueryTexture(const TextureHandleSource& source, TextureQueryKind kind) {
        const u32 lhs{ReadCbufValue(source.cbuf_index, source.cbuf_offset) << source.shift_left};
        const u32 rhs{ReadCbufValue(source.secondary_cbuf_index, source.secondary_cbuf_offset) << source.secondary_shift_left};
        const u32 handle{lhs | rhs};
        switch (kind) {
        case TextureQueryKind::Type: return static_cast<u32>(ReadTextureType(handle));
        case TextureQueryKind::PixelFormat: return static_cast<u32>(ReadTexturePixelFormat(handle));
        case TextureQueryKind::Integer: return IsTexturePixelFormatInteger(handle);
        case TextureQueryKind::Compare: return static_cast<u32>(ReadTextureCompareFunc(handle));
        }
        return 0;
    }

    [[nodiscard]] virtual u32 ReadViewportTransformState() = 0;

    [[nodiscard]] virtual u32 TextureBoundBuffer() const = 0;

    [[nodiscard]] virtual u32 LocalMemorySize() const = 0;

    [[nodiscard]] virtual u32 SharedMemorySize() const = 0;

    [[nodiscard]] virtual std::array<u32, 3> WorkgroupSize() const = 0;

    [[nodiscard]] virtual bool HasHLEMacroState() const = 0;

    [[nodiscard]] virtual std::optional<ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) = 0;

    virtual void Dump(u64 hash) = 0;

    [[nodiscard]] const ProgramHeader& SPH() const noexcept {
        return sph;
    }

    [[nodiscard]] const std::array<u32, 8>& GpPassthroughMask() const noexcept {
        return gp_passthrough_mask;
    }

    [[nodiscard]] Stage ShaderStage() const noexcept {
        return stage;
    }

    [[nodiscard]] u32 StartAddress() const noexcept {
        return start_address;
    }

    [[nodiscard]] bool IsPropietaryDriver() const noexcept {
        return is_propietary_driver;
    }

protected:
    ProgramHeader sph{};
    std::array<u32, 8> gp_passthrough_mask{};
    Stage stage{};
    u32 start_address{};
    bool is_propietary_driver{};
};

} // namespace Shader
