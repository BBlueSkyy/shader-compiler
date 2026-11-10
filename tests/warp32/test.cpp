// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/backend/spirv/emit_spirv_instructions.h>
#include <shader_compiler/backend/spirv/spirv_emit_context.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include "warp32.h"
using namespace Shader;
using namespace Shader::Backend::SPIRV;
namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string&) {}
void Error(const std::string&) {}
}

constexpr IR::Opcode ops[]{IR::Opcode::ShuffleIndex, IR::Opcode::ShuffleUp,
                           IR::Opcode::ShuffleDown, IR::Opcode::ShuffleButterfly};
constexpr auto emitters = std::array{EmitShuffleIndex, EmitShuffleUp, EmitShuffleDown,
                                    EmitShuffleButterfly};

bool EmitPredicate(EmitContext& ctx, unsigned mode, bool literal, u32 index, u32 clamp, u32 mask,
                   unsigned dynamic_arg = 0) {
    IR::Inst unknown{IR::Opcode::LaneId, 0};
    IR::Inst shuffle{ops[mode], 0};
    shuffle.SetArg(0, literal ? IR::Value{u32{77}} : IR::Value{&unknown});
    shuffle.SetArg(1, dynamic_arg == 1 ? IR::Value{&unknown} : IR::Value{index});
    shuffle.SetArg(2, dynamic_arg == 2 ? IR::Value{&unknown} : IR::Value{clamp});
    shuffle.SetArg(3, dynamic_arg == 3 ? IR::Value{&unknown} : IR::Value{mask});
    IR::Inst flag{IR::Opcode::GetInBoundsFromOp, 0};
    flag.SetArg(0, IR::Value{&shuffle});
    const auto value = ctx.Const(77u);
    const auto result = emitters[mode](ctx, &shuffle, value, ctx.Const(index), ctx.Const(clamp), ctx.Const(mask));
    assert(result.value == value.value); // stable scalar data result
    assert(flag.GetOpcode() == IR::Opcode::Void); // pseudo-operation consumed
    const auto p = flag.Definition<Id>();
    assert(p.value == ctx.true_value.value || p.value == ctx.false_value.value);
    return p.value == ctx.true_value.value;
}

void TestOracle() {
    Warp32 w;
    for (unsigned l = 0; l < 32; ++l) {
        w.values[l] = l * 7 + 1;
        assert(Warp32::Eq(l) == (uint32_t{1} << l));
        assert((Warp32::Lt(l) | Warp32::Eq(l)) == Warp32::Le(l));
        assert((Warp32::Gt(l) | Warp32::Eq(l)) == Warp32::Ge(l));
        assert((Warp32::Lt(l) & Warp32::Ge(l)) == 0);
    }
    assert(Warp32::Gt(0) == 0xfffffffeu); // #430 scalar Gt is deliberately not this
    assert(Warp32::Le(31) == 0xffffffffu);
    std::array<bool, 32> p{};
    assert(!w.Any(p) && !w.All(p) && w.Equal(p));
    p.fill(true);
    w.active = 0x80000005;
    assert(w.Ballot(p) == w.active && w.All(p) && w.Any(p) && w.Equal(p));
    p[31] = false;
    assert(w.Ballot(p) == 5 && !w.All(p) && w.Any(p) && !w.Equal(p));
    assert(!w.Shuffle(Warp32::Mode::Index, 0, 1, 31, 0).value); // inactive source
    const auto out = w.Shuffle(Warp32::Mode::Down, 31, 1, 31, 0);
    assert(!out.in_bounds && out.value == w.values[31]);
    for (uint32_t active : {1u, 0x80000000u, 0x55555555u, 0x80000005u}) {
        w.active = active;
        for (unsigned l = 0; l < 32; ++l) if (active & Warp32::Eq(l)) {
            const auto self = w.Shuffle(Warp32::Mode::Index, l, 17, 3, 31);
            assert(self.in_bounds && self.value == w.values[l]);
        }
    }
    assert(Warp32::FSwizzleAdd(0, 2, 3, 0xe4) == -5);
    assert(Warp32::FSwizzleAdd(1, 2, 3, 0xe4) == -1);
    assert(Warp32::FSwizzleAdd(2, 2, 3, 0xe4) == 1);
    assert(Warp32::FSwizzleAdd(3, 2, 3, 0xe4) == -3);
    for (unsigned selector = 0; selector < 4; ++selector)
    for (unsigned l = 0; l < 32; ++l)
        assert(Warp32::FSwizzleAdd(l, 2, 3, selector * 0x55) ==
               Warp32::FSwizzleAdd(0, 2, 3, selector * 0x55));
}

void WriteModule(const std::filesystem::path& directory, Stage stage, unsigned variant,
                 bool supported, bool large, bool vote, bool fswz_only = false) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = stage;
    program.workgroup_size = {32, 1, 1};
    program.invocations = 1;
    program.output_vertices = 1;
    program.output_topology = OutputTopology::PointList;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    program.info.uses_fswzadd = true;
    IR::IREmitter ir{block};
    (void)ir.FSwizzleAdd(ir.Imm32(2.0f), ir.Imm32(3.0f), ir.Imm32(0x99u), {});
    if (!fswz_only) {
        program.info.uses_subgroup_invocation_id = true;
        program.info.uses_subgroup_vote = true;
        program.info.uses_subgroup_mask = true;
        program.info.uses_subgroup_shuffles = true;
        const auto lane = ir.LaneId();
        const auto pred = ir.IEqual(lane, ir.Imm32(0u));
        (void)ir.VoteAll(pred); (void)ir.VoteAny(pred); (void)ir.VoteEqual(pred);
        (void)ir.SubgroupBallot(pred);
        (void)ir.SubgroupEqMask(); (void)ir.SubgroupLtMask(); (void)ir.SubgroupLeMask();
        (void)ir.SubgroupGtMask(); (void)ir.SubgroupGeMask();
        for (unsigned mode = 0; mode < 4; ++mode) {
            for (bool dynamic : {false, true}) {
                const auto value = dynamic ? lane : ir.Imm32(77u);
                const auto index = dynamic ? lane : ir.Imm32(0u);
                auto inst = block.PrependNewInst(block.end(), ops[mode],
                    {value, index, ir.Imm32(mode == 1 ? 0u : 31u), ir.Imm32(0u)});
                (void)ir.GetInBoundsFromOp(IR::Value{&*inst});
            }
        }
    }
    Profile profile;
    profile.supported_spirv = 0x00010300;
    profile.support_vote = vote;
    profile.supported_subgroup_stages = supported ? (1u << static_cast<unsigned>(stage)) : 0;
    profile.warp_size_potentially_larger_than_guest = large;
    profile.max_subgroup_size = large ? 64 : 32;
    profile.has_broken_spirv_subgroup_shuffle = variant == 1;
    profile.disable_subgroup_shuffle = variant == 2;
    const auto words = EmitSPIRV(profile, program);
    const auto name = std::string{supported ? "supported-" : "unsupported-"} +
        std::to_string(static_cast<unsigned>(stage)) + '-' + std::to_string(variant) + '-' +
        std::to_string(large) + '-' + std::to_string(vote) + (fswz_only ? "-fswz" : "") + ".spv";
    std::ofstream file{directory / name, std::ios::binary};
    file.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * sizeof(u32)));
    assert(file.good());
}

int TestEmitters() {
    TestOracle();
    IR::Program program;
    program.stage = Stage::VertexB;
    Profile profile;
    profile.supported_subgroup_stages = 0;
    Shader::Backend::Bindings bindings;
    RuntimeInfo runtime;
    EmitContext ctx{profile, runtime, program, bindings};
    assert(EmitLaneId(ctx).value == ctx.u32_zero_value.value);
    assert(EmitVoteAll(ctx, ctx.false_value).value == ctx.false_value.value);
    assert(EmitVoteAny(ctx, ctx.true_value).value == ctx.true_value.value);
    assert(EmitVoteEqual(ctx, ctx.false_value).value == ctx.true_value.value);
    for (u32 active : {1u, 0x80000000u, 0x55555555u, 0x80000005u}) {
        Warp32 w;
        w.active = active;
        for (unsigned l = 0; l < 32; ++l) w.values[l] = l;
        for (unsigned mode = 0; mode < 4; ++mode) {
            const u32 mask = mode == 0 ? 31u : 0u;
            const u32 clamp = mode == 1 ? 0u : 31u;
            assert(EmitPredicate(ctx, mode, false, 0, clamp, mask));
            for (unsigned l = 0; l < 32; ++l) if (active & Warp32::Eq(l)) {
                const auto r = w.Shuffle(static_cast<Warp32::Mode>(mode), l, 0, clamp, mask);
                assert(r.in_bounds && r.value == w.values[l]);
            }
        }
    }
    for (u32 active : {0x80000001u, 0x80000000u}) {
        Warp32 w;
        w.values.fill(77);
        w.active = active;
        assert(EmitPredicate(ctx, 0, true, 0, 31, 0));
        const auto r = w.Shuffle(Warp32::Mode::Index, 31, 0, 31, 0);
        assert(r.in_bounds);
        if (active & 1) assert(r.value == 77);
        else assert(!r.value); // undefined source is never claimed to be defined
    }
    // RED on #430: predicate false although each lane reads its own data.
    if (!EmitPredicate(ctx, 2, false, 0, 31, 0)) {
        std::cerr << "identity ShuffleDown must return true validity for all 32 lanes\n";
        return 1;
    }
    uint64_t cases{};
    for (unsigned mode = 0; mode < 4; ++mode)
    for (bool literal : {false, true})
    for (unsigned b = 0; b < 32; ++b)
    for (unsigned c = 0; c < 32; ++c)
    for (unsigned mask = 0; mask < 32; ++mask) {
        Warp32 w;
        for (unsigned l = 0; l < 32; ++l) w.values[l] = literal ? 77 : l;
        bool exact_true = true;
        for (unsigned l = 0; l < 32; ++l) {
            const auto r = w.Shuffle(static_cast<Warp32::Mode>(mode), l, b, c, mask);
            exact_true &= r.in_bounds && r.value == w.values[l];
        }
        const bool actual = EmitPredicate(ctx, mode, literal, b, c, mask);
        if (actual != exact_true) {
            std::cerr << "shuffle proof mismatch " << mode << ' ' << literal << ' '
                      << b << ' ' << c << ' ' << mask << '\n';
            return 1;
        }
        ++cases;
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned arg = 1; arg < 4; ++arg)
            assert(!EmitPredicate(ctx, mode, true, 0, 31, 0, arg));
        assert(!EmitPredicate(ctx, mode, true, 32, 31, 0));
        assert(!EmitPredicate(ctx, mode, true, 0, 32, 0));
        assert(!EmitPredicate(ctx, mode, true, 0, 31, 32));
    }
    std::cout << "Warp32 oracle + actual SPIR-V shuffle emitters: " << cases << " cases passed\n";
    return 0;
}

int main(int argc, char** argv) {
    assert(argc == 2 || (argc == 3 && std::string{argv[2]} == "--emit-only"));
    if (argc == 2) {
        if (const int result = TestEmitters(); result != 0) return result;
    }
    const std::filesystem::path directory{argv[1]};
    std::filesystem::create_directories(directory);
    for (unsigned stage = 0; stage < 6; ++stage) {
        const auto s = static_cast<Stage>(stage);
        for (bool large : {false, true}) {
            for (unsigned variant = 0; variant < 3; ++variant) {
                for (bool vote : {false, true}) WriteModule(directory, s, variant, false, large, vote);
                // The unchanged supported disable-shuffle path has separate constraints
                // on dynamic broadcast IDs. Do not claim coverage of that configuration.
                if (variant != 2) WriteModule(directory, s, variant, true, large, true);
            }
        }
        WriteModule(directory, s, 0, false, false, false, true);
    }
}
