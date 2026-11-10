// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>

// Test-only oracle. PTX 6.0 legacy shfl/vote semantics, with explicit active lanes.
// No Vulkan scheduling or host subgroup assumptions enter this model.
struct Warp32 {
    enum class Mode { Index, Up, Down, Butterfly };
    struct Result { std::optional<uint32_t> value; bool in_bounds; };
    std::array<uint32_t, 32> values{};
    uint32_t active{0xffffffffu};

    static uint32_t Eq(unsigned lane) { return uint32_t{1} << lane; }
    static uint32_t Lt(unsigned lane) { return Eq(lane) - 1; }
    static uint32_t Le(unsigned lane) { return Lt(lane) | Eq(lane); }
    static uint32_t Gt(unsigned lane) { return ~Le(lane); }
    static uint32_t Ge(unsigned lane) { return ~Lt(lane); }
    uint32_t Ballot(const std::array<bool, 32>& p) const {
        uint32_t result{};
        for (unsigned l = 0; l < 32; ++l) if (p[l]) result |= active & Eq(l);
        return result;
    }
    bool All(const std::array<bool, 32>& p) const { return Ballot(p) == active; }
    bool Any(const std::array<bool, 32>& p) const { return Ballot(p) != 0; }
    bool Equal(const std::array<bool, 32>& p) const { return All(p) || !Any(p); }
    Result Shuffle(Mode mode, unsigned lane, unsigned b, unsigned clamp, unsigned mask) const {
        b &= 31; clamp &= 31; mask &= 31;
        const int lo = int(lane & mask);
        const int hi = lo | int(clamp & ~mask);
        int source{};
        bool valid{};
        switch (mode) {
        case Mode::Index: source = lo | int(b & ~mask); valid = source <= hi; break;
        case Mode::Up: source = int(lane) - int(b); valid = source >= hi; break;
        case Mode::Down: source = int(lane) + int(b); valid = source <= hi; break;
        case Mode::Butterfly: source = int(lane ^ b); valid = source <= hi; break;
        }
        if (!valid) source = int(lane);
        if (!(active & Eq(lane)) || source < 0 || source >= 32 || !(active & Eq(unsigned(source))))
            return {std::nullopt, valid}; // inactive source is undefined, not zero
        return {values[unsigned(source)], valid};
    }
    static float FSwizzleAdd(unsigned lane, float a, float b, unsigned swizzle) {
        constexpr float lut_a[]{-1, 1, -1, 0};
        constexpr float lut_b[]{-1, -1, 1, -1};
        const auto selector = (swizzle >> (2 * (lane & 3))) & 3;
        return a * lut_a[selector] + b * lut_b[selector];
    }
};
