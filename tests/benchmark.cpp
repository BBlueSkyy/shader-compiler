// SPDX-License-Identifier: GPL-2.0-or-later
#include <bit>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/ir_opt/passes.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string&) {}
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
    void SetStage(Stage s) { stage = s; sph.common4.max_output_vertices.Assign(3); }
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
int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::stoi(argv[1]) : 500;
    for (auto name : {"vertex", "geometry", "passthrough", "partial"}) {
        std::vector<double> samples;
        size_t words{};
        u64 hash{};
        for (int sample = 0; sample < 7; ++sample) {
            Pools p;
            const auto start = std::clock();
            for (int i = 0; i < iterations; ++i) {
                FixtureEnvironment env(OutputTopology::TriangleStrip, std::string(name) == "passthrough" || std::string(name) == "partial", std::string(name) == "partial");
                env.SetStage(std::string(name) == "vertex" ? Stage::VertexB : Stage::Geometry);
                auto program = Translate(p, env, false);
                Profile profile{};
                profile.supported_spirv = 0x00010000;
                RuntimeInfo runtime{};
                runtime.previous_stage_stores.mask.set();
                runtime.input_topology = InputTopology::Triangles;
                Backend::Bindings bindings{};
                auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
                words = spv.size();
                hash = 14695981039346656037ULL;
                for (auto word : spv) { hash ^= word; hash *= 1099511628211ULL; }
                p.inst.ReleaseContents(); p.blocks.ReleaseContents(); p.flow.ReleaseContents();
            }
            samples.push_back((static_cast<double>(std::clock() - start) * 1000000.0 / CLOCKS_PER_SEC) / iterations);
        }
        std::sort(samples.begin(), samples.end());
        std::cout << name << " median_us=" << samples[3] << " min_us=" << samples[0] << " max_us=" << samples[6] << " words=" << words << " hash=" << hash << " iterations=" << iterations << "\n";
    }
}
