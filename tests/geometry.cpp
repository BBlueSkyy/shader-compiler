// SPDX-License-Identifier: GPL-2.0-or-later
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/frontend/maxwell/geometry_passthrough_link.h>
#include <shader_compiler/ir_opt/passes.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& s) { std::cerr << s << '\n'; }
void Error(const std::string& s) { std::cerr << s << '\n'; }
}
using namespace Shader;
namespace {
int failures{};
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct FixtureEnvironment final : Shader::Environment {
    std::vector<u64> instructions;
    FixtureEnvironment(OutputTopology topology, bool pass, bool partial = false, bool attributes = false, bool primitive = false) {
        stage = Stage::Geometry;
        sph.common3.output_topology.Assign(topology);
        sph.common4.max_output_vertices.Assign(0);
        sph.common2.threads_per_input_primitive.Assign(1);
        sph.common0.geometry_passthrough.Assign(pass ? 1 : 0);
        gp_passthrough_mask.fill(~0U);
        auto enable = [&](IR::Attribute attr) { const auto n = static_cast<u32>(attr); gp_passthrough_mask[n / 32] &= ~(1U << (n % 32)); };
        for (u32 c = 0; c < 4; ++c) {
            enable(IR::Attribute::PositionX + c);
            if (!partial || c == 0) enable(IR::Attribute::Generic0X + c);
        }
        if (attributes) { enable(IR::Attribute::ClipDistance0); enable(IR::Attribute::PointSize); }
        // A real Maxwell EXIT (PT predicate, true flow condition), scheduler word at 0.
        instructions = {0};
        if (primitive) {
            // AST attr, RZ with index/vertex RZ, PT; final EXIT follows scheduler slot.
            for (IR::Attribute attr : {IR::Attribute::Layer, IR::Attribute::ViewportIndex, IR::Attribute::Generic0Y}) {
                instructions.push_back(0xeff0000000070000ULL | 255ULL | (255ULL << 8) |
                                       (static_cast<u64>(attr) * 4 << 20) | (255ULL << 39));
            }
            instructions.push_back(0); // scheduler control word
        }
        instructions.push_back(0xe30000000007000fULL);
    }
    void Pass(IR::Attribute attr) {
        const auto n = static_cast<u32>(attr);
        gp_passthrough_mask[n / 32] &= ~(1U << (n % 32));
    }
    u64 ReadInstruction(u32 addr) override { return instructions.at(addr / 8); }
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
struct Pools {
    ObjectPool<IR::Inst> inst;
    ObjectPool<IR::Block> blocks;
    ObjectPool<Maxwell::Flow::Block> flow;
};
IR::Program Translate(Pools& p, FixtureEnvironment& env, bool native) {
    Maxwell::Flow::CFG cfg(env, p.flow, Maxwell::Location{0});
    HostTranslateInfo host{};
    host.support_geometry_shader_passthrough = native;
    return Maxwell::TranslateProgram(p.inst, p.blocks, env, cfg, host);
}
int Count(const IR::Program& program, IR::Opcode op) {
    int n{};
    for (auto* b : program.blocks) for (auto& inst : *b) n += inst.GetOpcode() == op;
    return n;
}
void Write(const std::filesystem::path& root, const std::string& name, IR::Program& program,
           bool native, InputTopology topology, bool broken = false, bool depth = false) {
    Profile profile{};
    profile.supported_spirv = 0x00010000;
    profile.support_geometry_shader_passthrough = native;
    profile.has_broken_spirv_position_input = broken;
    RuntimeInfo runtime{};
    runtime.previous_stage_stores.mask.set();
    runtime.input_topology = topology;
    runtime.convert_depth_mode = depth;
    if (topology == InputTopology::Points) runtime.fixed_state_point_size = 1.0f;
    Backend::Bindings bindings{};
    const auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    std::ofstream out(root / (name + ".spv"), std::ios::binary);
    out.write(reinterpret_cast<const char*>(spv.data()), static_cast<std::streamsize>(spv.size() * 4));
}
}
int TestPassthroughLink();
int TestLayerPass();
int TestReviewGeometry();
int main(int argc, char** argv) {
    const std::filesystem::path out = argc > 1 ? argv[1] : "modules";
    std::filesystem::create_directories(out);
    for (auto [topology, input, count, name] : {
        std::tuple{OutputTopology::PointList, InputTopology::Points, 1, "points"},
        std::tuple{OutputTopology::LineStrip, InputTopology::Lines, 2, "lines"},
        std::tuple{OutputTopology::TriangleStrip, InputTopology::Triangles, 3, "triangles"}}) {
        for (bool native : {false, true}) {
            Pools pools;
            FixtureEnvironment env(topology, true);
            auto program = Translate(pools, env, native);
            Check(program.is_geometry_passthrough == native, "lowered program must cease being native passthrough");
            Check(Count(program, IR::Opcode::EmitVertex) == (native ? 0 : count), "correct vertex count");
            Check(program.invocations == 1, "preserve invocation count");
            Write(out, std::string(name) + (native ? "-native" : "-lowered"), program, native, input);
        }
    }
    {
        Pools p;
        FixtureEnvironment env(OutputTopology::TriangleStrip, true, true);
        auto program = Translate(p, env, false);
        Check(program.info.stores[IR::Attribute::Generic0X], "copy requested component");
        Check(!program.info.stores[IR::Attribute::Generic0Y], "do not copy skipped components");
        Write(out, "partial-lowered", program, false, InputTopology::Triangles);
    }
    {
        Pools p;
        FixtureEnvironment env(OutputTopology::TriangleStrip, true, false, true);
        auto program = Translate(p, env, false);
        Check(program.info.stores[IR::Attribute::ClipDistance0], "preserve clip distance");
        Check(program.info.stores[IR::Attribute::PointSize], "preserve shader point size");
        Write(out, "builtins-lowered", program, false, InputTopology::Triangles);
    }
    {
        Pools p;
        FixtureEnvironment env(OutputTopology::TriangleStrip, true);
        auto program = Translate(p, env, true);
        Write(out, "native-position-block", program, true, InputTopology::Triangles, true);
    }
    {
        Pools p;
        FixtureEnvironment env(OutputTopology::TriangleStrip, true, true, false, true);
        auto program = Translate(p, env, false);
        Write(out, "primitive-lowered", program, false, InputTopology::Triangles);
    }
    // The synthetic GS is an ordinary host GS: it forwards the last stage and
    // maps emulated primitive outputs, without native passthrough declarations.
    for (auto [topology, input, count, name] : {
        std::tuple{OutputTopology::PointList, InputTopology::Points, 1, "points"},
        std::tuple{OutputTopology::LineStrip, InputTopology::Lines, 2, "lines"},
        std::tuple{OutputTopology::TriangleStrip, InputTopology::Triangles, 3, "triangles"}}) {
        Pools p;
        IR::Program source;
        source.stage = Stage::VertexB;
        source.info.requires_layer_emulation = true;
        source.info.emulated_layer = IR::Attribute::Generic1X;
        source.info.stores.Set(IR::Attribute::Generic1X);
        source.info.requires_viewport_index_emulation = true;
        source.info.emulated_viewport_index = IR::Attribute::Generic2X;
        source.info.stores.Set(IR::Attribute::Generic2X);
        for (u32 c = 0; c < 4; ++c) source.info.stores.Set(IR::Attribute::Generic0X + c);
        auto program = Maxwell::GenerateGeometryPassthrough(p.inst, p.blocks, {}, source, topology);
        Check(!program.is_geometry_passthrough && !program.was_geometry_passthrough, "synthetic GS is a conventional host stage");
        Check(Count(program, IR::Opcode::EmitVertex) == count, "synthetic vertex count");
        Write(out, std::string(name) + "-synthetic", program, false, input);
        if (count == 2) Write(out, "lines-adjacency", program, false, InputTopology::LinesAdjacency);
        if (count == 3) Write(out, "triangles-adjacency", program, false, InputTopology::TrianglesAdjacency);
    }
    {
        Pools p;
        FixtureEnvironment env(OutputTopology::TriangleStrip, true, true);
        auto program = Translate(p, env, true);
        Check(!program.is_geometry_passthrough, "partial components require lowering even with native support");
        Write(out, "partial-native-profile", program, true, InputTopology::Triangles);
    }
    {
        Pools p;
        FixtureEnvironment env(OutputTopology::PointList, true);
        env.Pass(IR::Attribute::PointSize);
        auto program = Translate(p, env, true);
        Check(program.is_geometry_passthrough, "native PointSize passthrough remains available");
        Write(out, "native-point-size-input", program, true, InputTopology::Points);
    }
    failures += TestPassthroughLink();
    failures += TestLayerPass();
    failures += TestReviewGeometry();
    std::cout << "geometry regressions: " << failures << " failures\n";
    return failures ? 1 : 0;
}
