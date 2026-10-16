// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <stdexcept>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/host_translate_info.h>

namespace {
using namespace Shader;
struct ReviewEnvironment final : Environment {
    std::vector<u64> code{0, 0xe30000000007000fULL};
    ReviewEnvironment() {
        stage = Stage::Geometry;
        sph.common0.geometry_passthrough.Assign(1);
        sph.common2.threads_per_input_primitive.Assign(1);
        sph.common3.output_topology.Assign(OutputTopology::TriangleStrip);
        gp_passthrough_mask.fill(~0U);
        for (u32 c = 0; c < 4; ++c) Pass(IR::Attribute::PositionX + c);
    }
    void Pass(IR::Attribute attr) {
        const auto n = static_cast<u32>(attr);
        gp_passthrough_mask[n / 32] &= ~(1U << (n % 32));
    }
    void Store(IR::Attribute attr) {
        code = {0, 0xeff0000000070000ULL | 255ULL | (255ULL << 8) |
                   (static_cast<u64>(attr) * 4 << 20) | (255ULL << 39),
                0xe30000000007000fULL};
    }
    u64 ReadInstruction(u32 addr) override { return code.at(addr / 8); }
    u32 ReadCbufValue(u32,u32) override { throw std::runtime_error("unexpected cbuf"); }
    TextureType ReadTextureType(u32) override { throw std::runtime_error("unexpected texture"); }
    CompareFunction ReadTextureCompareFunc(u32) override { return CompareFunction::Always; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32,3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32) override { return {}; }
    void Dump(u64) override {}
};
struct ReviewPools {
    ObjectPool<IR::Inst> inst;
    ObjectPool<IR::Block> block;
    ObjectPool<Maxwell::Flow::Block> flow;
    IR::Program Translate(ReviewEnvironment& env) {
        Maxwell::Flow::CFG cfg{env, flow, Maxwell::Location{0}};
        HostTranslateInfo host{};
        host.support_geometry_shader_passthrough = true;
        return Maxwell::TranslateProgram(inst, block, env, cfg, host);
    }
};
}

int TestReviewGeometry() {
    int failures{};
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    for (u32 c = 0; c < 8; ++c) {
        ReviewPools p;
        ReviewEnvironment env;
        env.Pass(IR::Attribute::ClipDistance0 + c);
        auto program = p.Translate(env);
        check(!program.is_geometry_passthrough, "a single forwarded clip distance requires lowering");
    }
    {
        ReviewPools p;
        ReviewEnvironment env;
        for (u32 c = 0; c < 8; ++c) env.Pass(IR::Attribute::ClipDistance0 + c);
        auto program = p.Translate(env);
        check(program.is_geometry_passthrough, "a fully forwarded clip-distance array can stay native");
    }
    for (auto attr : {IR::Attribute::ColorFrontDiffuseR, IR::Attribute::FogCoordinate,
                      IR::Attribute::FixedFncTexture9Q}) {
        ReviewPools p;
        ReviewEnvironment env;
        env.Pass(attr);
        auto program = p.Translate(env);
        check(!program.is_geometry_passthrough, "legacy passthrough requires lowering");
    }
    {
        ReviewPools p;
        ReviewEnvironment env;
        env.Store(IR::Attribute::Generic1X);
        auto program = p.Translate(env);
        check(program.is_geometry_passthrough && program.info.stores[IR::Attribute::Generic1X],
              "native geometry keeps computed fully skipped generic output");
    }
    {
        ReviewPools p;
        ReviewEnvironment env;
        env.Store(IR::Attribute::PositionZ);
        auto program = p.Translate(env);
        check(!program.is_geometry_passthrough, "guest position store requires lowering");
    }
    {
        ReviewPools p;
        ReviewEnvironment env;
        env.Pass(IR::Attribute::ColorFrontDiffuseR);
        env.Store(IR::Attribute::ColorFrontDiffuseG);
        auto program = p.Translate(env);
        RuntimeInfo runtime{};
        runtime.previous_stage_stores.mask.set();
        runtime.input_topology = InputTopology::Triangles;
        Maxwell::ConvertLegacyToGeneric(program, runtime);
        Profile profile{};
        profile.supported_spirv = 0x00010000;
        Backend::Bindings bindings{};
        const auto words = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        check(!words.empty(), "mixed forwarded and computed legacy output compiles after remapping");
    }
    return failures;
}
