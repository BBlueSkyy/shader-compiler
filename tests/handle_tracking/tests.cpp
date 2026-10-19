// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <shader_compiler/environment.h>
#include <shader_compiler/frontend/ir/basic_block.h>
#include <shader_compiler/frontend/ir/modifiers.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/ir_opt/passes.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string&) {}
void Error(const std::string&) {}
}
using namespace Shader;
struct TestEnvironment final : Environment {
    std::array<u32, 4> values{0x6ec00021U, 3U, 0xffffffffU, 0xabc12345U};
    u32 last_handle{};
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32 index, u32) override { return values.at(index); }
    TextureType ReadTextureType(u32 handle) override { last_handle = handle; return TextureType::Color2D; }
    CompareFunction ReadTextureCompareFunc(u32 handle) override { last_handle = handle; return CompareFunction::Less; }
    TexturePixelFormat ReadTexturePixelFormat(u32 handle) override { last_handle = handle; return TexturePixelFormat::OTHER; }
    bool IsTexturePixelFormatInteger(u32 handle) override { last_handle = handle; return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32,3> WorkgroupSize() const override { return {1,1,1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32) override { return std::nullopt; }
    void Dump(u64) override {}
};
struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program{};
    TestEnvironment env;
    Fixture() { program.blocks = {&block}; program.post_order_blocks = {&block}; }
    IR::Value op(IR::Opcode opcode, std::initializer_list<IR::Value> args) {
        block.AppendNewInst(opcode,args); return IR::Value{&block.Instructions().back()};
    }
    IR::Value cbuf(u32 index) { return op(IR::Opcode::GetCbufU32,{IR::Value{index},IR::Value{0U}}); }
    IR::Value mask(IR::Value value,u32 bits) { return op(IR::Opcode::BitwiseAnd32,{value,IR::Value{bits}}); }
    IR::Value shl(IR::Value value,u32 bits) { return op(IR::Opcode::ShiftLeftLogical32,{value,IR::Value{bits}}); }
    IR::Value either(IR::Value lhs,IR::Value rhs) { return op(IR::Opcode::BitwiseOr32,{lhs,rhs}); }
    void query(IR::Value handle) {
        auto inst=op(IR::Opcode::BindlessImageQueryDimensions,{handle,IR::Value{0U}});
        IR::TextureInstInfo flags{};flags.type.Assign(TextureType::Color2D);inst.Inst()->SetFlags(flags);
    }
    void run() { HostTranslateInfo host{};host.support_snorm_render_buffer=true;Optimization::TexturePass(env,program,host); }
};
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void test(const char* name,const std::function<IR::Value(Fixture&)>& make,u32 expected) {
    Fixture f;f.query(make(f));f.run();
    if(f.env.last_handle!=expected)throw std::runtime_error(std::string{name}+": expected "+std::to_string(expected)+", got "+std::to_string(f.env.last_handle));
    check(f.program.info.texture_descriptors.size()==1,"query descriptor was not generated");
    const auto& desc=f.program.info.texture_descriptors[0];
    const u32 primary=f.env.ReadCbufValue(desc.cbuf_index,desc.cbuf_offset);
    const u32 secondary=desc.has_secondary ? f.env.ReadCbufValue(desc.secondary_cbuf_index,desc.secondary_cbuf_offset) : 0;
    check(EvaluateTextureHandle(desc,primary,secondary)==expected,"emitted runtime descriptor disagrees with translation query");
}
int main() {
    test("mask clears stale sampler bits before OR",[](Fixture& f){return f.either(f.mask(f.cbuf(0),0xfffffU),f.shl(f.cbuf(1),20));},0x00300021U);
    test("mask after shift is not a shift",[](Fixture& f){return f.mask(f.shl(f.cbuf(3),4),0x0fffff00U);},0x0c123400U);
    test("nested masks intersect",[](Fixture& f){return f.mask(f.mask(f.cbuf(3),0x0fffff00U),0x00fffff0U);},0x00c12300U);
    test("zero mask",[](Fixture& f){return f.mask(f.cbuf(0),0U);},0U);
    test("single source shift",[](Fixture& f){return f.shl(f.cbuf(1),20);},0x00300000U);
    test("nested shifts accumulate",[](Fixture& f){return f.shl(f.shl(f.cbuf(1),5),15);},0x00300000U);
    test("shift applies to both OR branches",[](Fixture& f){return f.shl(f.either(f.mask(f.cbuf(0),0xfffffU),f.shl(f.cbuf(1),20)),1);},0x00600042U);
    test("mask applies to both OR branches",[](Fixture& f){return f.mask(f.either(f.cbuf(0),f.shl(f.cbuf(1),20)),0x00ffffffU);},0x00f00021U);
    test("shift32 defined zero",[](Fixture& f){return f.shl(f.cbuf(2),32);},0U);
    test("nested shift overflow defined zero",[](Fixture& f){return f.shl(f.shl(f.cbuf(2),31),1);},0U);
    test("shift31 retains unsigned top bit",[](Fixture& f){return f.shl(f.cbuf(1),31);},0x80000000U);
    test("immediate AND on left",[](Fixture& f){return f.op(IR::Opcode::BitwiseAnd32,{IR::Value{0xfffffU},f.cbuf(0)});},33U);
    test("mask read from constant cbuf",[](Fixture& f){return f.mask(f.op(IR::Opcode::BitwiseAnd32,{f.cbuf(0),f.cbuf(1)}),0xffffffffU);},1U);
    // The same source with different masks/shifts must not collapse into one descriptor.
    Fixture distinct;
    distinct.query(distinct.mask(distinct.cbuf(0),0xfffffU));
    distinct.query(distinct.mask(distinct.cbuf(0),0xffffffU));
    distinct.query(distinct.shl(distinct.cbuf(0),1));
    distinct.query(distinct.shl(distinct.cbuf(0),2));
    distinct.run();
    check(distinct.program.info.texture_descriptors.size()==4,"different source transforms were merged into one descriptor");
    std::cout<<"PASS real TexturePass masks, shifts, split handles, query/runtime agreement and deduplication\n";
}
