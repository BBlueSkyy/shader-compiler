// SPDX-License-Identifier: GPL-2.0-or-later
// Packed Maxwell instructions are consumed by the production decoder/visitor.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/frontend/maxwell/decode.h>
#include <shader_compiler/frontend/maxwell/translate/impl/impl.h>
#include <shader_compiler/ir_opt/passes.h>
#include <shader_compiler/host_translate_info.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& s) { std::cerr << s << '\n'; }
void Error(const std::string& s) { std::cerr << s << '\n'; }
}
using namespace Shader;
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Env final : public Environment {
public:
    bool integer = true;
    Env() { stage = Stage::Compute; }
    u64 ReadInstruction(u32) override { throw std::runtime_error("unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { return 1; }
    TextureType ReadTextureType(u32) override { throw std::runtime_error("unexpected type read"); }
    CompareFunction ReadTextureCompareFunc(u32) override { throw std::runtime_error("unexpected compare read"); }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { throw std::runtime_error("unexpected format read"); }
    bool IsTexturePixelFormatInteger(u32) override { return integer; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1,1,1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32) override { return {}; }
    void Dump(u64) override {}
};
// Non-overlapping field positions independently documented by envytools gm107.c.
u64 Encode(bool reduction, unsigned op, unsigned size, unsigned type, bool bindless,
           unsigned coord = 10, unsigned operand = 20, unsigned dest = 30,
           unsigned handle = 40, unsigned bound = 0x123, unsigned clamp = 0) {
    const u64 common = (u64(type) << 33) | (u64(coord) << 8) | (u64(clamp) << 49);
    if (reduction)
        return 0xeb50000000070000ULL | common | (u64(op) << 24) | (u64(size) << 20) |
            operand | (bindless ? (u64(handle) << 39) : ((1ULL << 51) | (u64(bound) << 36)));
    return 0xea00000000070000ULL | common | (u64(op) << 29) | (u64(size) << 51) |
        (u64(operand) << 20) | dest |
        (bindless ? ((1ULL << 54) | (u64(handle) << 39)) : (u64(bound) << 36));
}
struct Fixture {
    Env env;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    Maxwell::TranslatorVisitor visitor{env, block};
    IR::Program program;
    Fixture() {
        program.stage = Stage::Compute;
        program.workgroup_size = {1,1,1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block},
                               {.type = IR::AbstractSyntaxNode::Type::Return}};
    }
    void Translate(u64 bits, bool reduction) {
        Check(Maxwell::Decode(bits) == (reduction ? Maxwell::Opcode::SURED : Maxwell::Opcode::SUATOM), "instruction decode");
        if (reduction) visitor.SURED(bits); else visitor.SUATOM(bits);
    }
    IR::Inst* Atomic() {
        for (auto& i : block) {
            if (i.MayHaveSideEffects() && i.GetOpcode() != IR::Opcode::SetRegister) return &i;
        }
        throw std::runtime_error("missing image atomic side effect");
    }
};
void Reg(const IR::Value& value, IR::Reg reg) {
    Check(!value.IsImmediate() && value.Inst()->GetOpcode() == IR::Opcode::GetRegister &&
          value.Inst()->Arg(0).Reg() == reg, "wrong source register");
}
IR::Opcode Expected(bool bindless, unsigned op, unsigned size) {
    constexpr IR::Opcode bound[]{IR::Opcode::BoundImageAtomicIAdd32,
        IR::Opcode::BoundImageAtomicUMin32, IR::Opcode::BoundImageAtomicUMax32,
        IR::Opcode::BoundImageAtomicInc32, IR::Opcode::BoundImageAtomicDec32,
        IR::Opcode::BoundImageAtomicAnd32, IR::Opcode::BoundImageAtomicOr32,
        IR::Opcode::BoundImageAtomicXor32, IR::Opcode::BoundImageAtomicExchange32};
    constexpr IR::Opcode bind[]{IR::Opcode::BindlessImageAtomicIAdd32,
        IR::Opcode::BindlessImageAtomicUMin32, IR::Opcode::BindlessImageAtomicUMax32,
        IR::Opcode::BindlessImageAtomicInc32, IR::Opcode::BindlessImageAtomicDec32,
        IR::Opcode::BindlessImageAtomicAnd32, IR::Opcode::BindlessImageAtomicOr32,
        IR::Opcode::BindlessImageAtomicXor32, IR::Opcode::BindlessImageAtomicExchange32};
    if (size == 1 && op == 1) return bindless ? IR::Opcode::BindlessImageAtomicSMin32 : IR::Opcode::BoundImageAtomicSMin32;
    if (size == 1 && op == 2) return bindless ? IR::Opcode::BindlessImageAtomicSMax32 : IR::Opcode::BoundImageAtomicSMax32;
    return bindless ? bind[op] : bound[op];
}
void Frontend() {
    unsigned count = 0;
    for (bool reduction : {true, false}) for (bool bindless : {false, true})
    for (unsigned size : {0U,1U,6U}) for (unsigned type = 0; type < 6; ++type)
    for (unsigned op = 0; op < (reduction ? 8U : 9U); ++op) {
        Fixture f;
        f.Translate(Encode(reduction, op, size, type, bindless), reduction);
        auto* atomic = f.Atomic();
        if (atomic->GetOpcode() != Expected(bindless, op, size))
            throw std::runtime_error(fmt::format("wrong atomic opcode: reduction={} op={} size={} type={} bindless={}", reduction, op, size, type, bindless));
        Check(atomic->Flags<IR::TextureInstInfo>().image_format == ImageFormat::R32_UINT, "32-bit atomic format");
        constexpr TextureType types[]{TextureType::Color1D, TextureType::Buffer,
            TextureType::ColorArray1D, TextureType::Color2D, TextureType::ColorArray2D, TextureType::Color3D};
        Check(atomic->Flags<IR::TextureInstInfo>().type == types[type], "image dimensionality");
        if (bindless) Reg(atomic->Arg(0), IR::Reg::R40);
        else Check(atomic->Arg(0).U32() == 0x123 * 4, "bound byte offset");
        Reg(atomic->Arg(2), IR::Reg::R20);
        auto coord = atomic->Arg(1);
        if (type < 2) Reg(coord, IR::Reg::R10);
        else {
            const unsigned n = type == 2 || type == 3 ? 2 : 3;
            Check(coord.Inst()->GetOpcode() == (n == 2 ? IR::Opcode::CompositeConstructU32x2 : IR::Opcode::CompositeConstructU32x3), "coordinate vector width");
            for (unsigned index = 0; index < n; ++index) {
                auto v = coord.Inst()->Arg(index);
                if ((type == 2 || type == 4) && index == n - 1) {
                    Check(v.Inst()->GetOpcode() == IR::Opcode::BitFieldUExtract &&
                          v.Inst()->Arg(1).U32() == 0 && v.Inst()->Arg(2).U32() == 16, "array layer low16 zero extension");
                    v = v.Inst()->Arg(0);
                }
                Reg(v, IR::Reg::R10 + int(index));
            }
        }
        unsigned writes = 0;
        for (auto& i : f.block) if (i.GetOpcode() == IR::Opcode::SetRegister) {
            ++writes;
            Check(i.Arg(0).Reg() == IR::Reg::R30 && i.Arg(1).Inst() == atomic, "SUATOM old value destination");
        }
        Check(writes == (reduction ? 0 : 1), "SURED must not write a register");
        ++count;
    }
    for (bool reduction : {true,false}) {
        Fixture edge;
        edge.Translate(Encode(reduction,0,0,4,false,255,255,255,40,0x1fff), reduction);
        Check(edge.Atomic()->Arg(0).U32() == 0x7ffc, "maximum bound byte offset");
        Optimization::SsaRewritePass(edge.program);
        Optimization::ConstantPropagationPass(edge.env, edge.program);
        Optimization::IdentityRemovalPass(edge.program);
        Check(edge.Atomic()->Arg(2).Resolve().U32() == 0, "RZ operand must be zero");
        Check(edge.Atomic()->Arg(1).Inst()->Arg(2).Resolve().U32() == 0,
              "RZ array coordinate must have layer zero");
        Fixture overflow;
        bool rejected = false;
        try { overflow.Translate(Encode(reduction,0,0,4,false,253), reduction); }
        catch (const LogicError&) { rejected = true; }
        Check(rejected, "coordinate register arithmetic overflow must be rejected");
        for (unsigned size : {2U,3U,4U,5U,7U}) {
            Fixture f; bool threw = false;
            try { f.Translate(Encode(reduction,0,size,4,false), reduction); }
            catch (const NotImplementedException&) { threw = true; }
            Check(threw, "unsupported 64-bit/float size must fail closed");
        }
        for (unsigned type : {6U,7U}) {
            Fixture f; bool threw = false;
            try { f.Translate(Encode(reduction,0,0,type,false), reduction); }
            catch (const NotImplementedException&) { threw = true; }
            Check(threw, "unsupported type must fail closed");
        }
        for (unsigned clamp : {1U,2U,3U}) {
            Fixture f; bool threw = false;
            try { f.Translate(Encode(reduction,0,0,4,false,10,20,30,40,0x123,clamp), reduction); }
            catch (const NotImplementedException&) { threw = true; }
            Check(threw, "unsupported clamp must fail closed");
        }
    }
    std::cout << "PASS " << count << " translated instruction cases plus rejected sizes/types/clamps\n";
}
void Spirv(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    unsigned count = 0;
    for (bool reduction : {true,false}) for (bool bindless : {false,true})
    for (unsigned type = 0; type < 6; ++type)
    for (unsigned op : {0U,1U,2U,5U,6U,7U,8U}) {
        if (reduction && op == 8) continue;
        Fixture f;
        for (unsigned reg = 10; reg <= 12; ++reg)
            f.visitor.X(IR::Reg::R0 + int(reg), f.visitor.ir.Imm32(reg == 12 ? 0xffff8001U : 0xfffffffeU));
        f.visitor.X(IR::Reg::R20, f.visitor.ir.Imm32(0x80000000U));
        f.visitor.X(IR::Reg::R40, f.visitor.ir.GetCbuf(f.visitor.ir.Imm32(2U), f.visitor.ir.Imm32(0x48cU)));
        f.Translate(Encode(reduction,op,1,type,bindless), reduction);
        HostTranslateInfo host{};
        Optimization::SsaRewritePass(f.program);
        Optimization::ConstantPropagationPass(f.env, f.program);
        Optimization::TexturePass(f.env, f.program, host);
        Optimization::IdentityRemovalPass(f.program);
        Optimization::DeadCodeEliminationPass(f.program);
        Optimization::CollectShaderInfoPass(f.env, f.program);
        // Optimization must retain the side effect even when its old value is unused.
        auto* atomic = f.Atomic();
        if (type == 2 || type == 4) {
            const auto last = type == 2 ? 1 : 2;
            Check(atomic->Arg(1).Inst()->Arg(last).Resolve().U32() ==
                  (type == 2 ? 65534U : 32769U), "array layer must be unsigned low16 after folding");
        }
        Check(f.program.info.uses_atomic_image_u32, "atomic feature collection");
        if (type == 1) {
            Check(f.program.info.image_buffer_descriptors.size() == 1 &&
                f.program.info.image_buffer_descriptors[0].is_written &&
                f.program.info.image_buffer_descriptors[0].is_read, "image buffer read/write effects");
        } else {
            Check(f.program.info.image_descriptors.size() == 1 &&
                f.program.info.image_descriptors[0].is_written &&
                f.program.info.image_descriptors[0].is_read, "image read/write effects");
        }
        Profile profile{};
        profile.supported_spirv = 0x10400;
        profile.unified_descriptor_binding = true;
        profile.support_descriptor_aliasing = true;
        Backend::Bindings bindings{};
        const auto words = Backend::SPIRV::EmitSPIRV(profile, {}, f.program, bindings);
        const auto name = fmt::format("{}_{}_type{}_op{}", reduction ? "sured" : "suatom",
            bindless ? "bindless" : "bound", type, op);
        std::ofstream file(directory / (name + ".spv"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(words.data()), words.size()*sizeof(u32));
        std::ofstream(directory / (name + ".ir")) << IR::DumpProgram(f.program);
        ++count;
    }
    std::cout << "PASS emitted " << count << " translated/optimized SPIR-V modules\n";
}
int main(int argc, char** argv) try {
    Frontend();
    if (argc > 1) Spirv(argv[1]);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
