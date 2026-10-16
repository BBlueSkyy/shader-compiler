// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <shader_compiler/common/common_types.h>

namespace Shader {
// Conservative byte extent for an affine compute offset. Coefficients bound the
// non-negative contribution of each invocation coordinate, excluding the pointer.
struct ComputeStorageBufferAccessRange {
    bool known{};
    u64 constant{}; // Includes the access width.
    std::array<u64, 3> workgroup{};
    std::array<u64, 3> local{};

    [[nodiscard]] std::optional<u64> GetSize(const std::array<u32, 3>& groups,
                                            const std::array<u32, 3>& threads) const {
        if (!known) return std::nullopt;
        u64 size{constant};
        constexpr u64 limit{1ULL << 32}; // Offsets in this lowering are U32.
        auto add = [&](u64 coefficient, u32 count) {
            const u64 n{count - 1ULL};
            if (n && coefficient > (limit - size) / n) return false;
            size += coefficient * n;
            return true;
        };
        for (std::size_t axis{}; axis < 3; ++axis) {
            if (!groups[axis] || !threads[axis]) return 0;
            if (size > limit || !add(workgroup[axis], groups[axis]) ||
                !add(local[axis], threads[axis])) return std::nullopt;
        }
        return size;
    }

    auto operator<=>(const ComputeStorageBufferAccessRange&) const = default;
};
} // namespace Shader
