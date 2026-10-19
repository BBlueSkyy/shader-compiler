// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#ifdef TEST_HANDLE_SPIRV
#include <filesystem>
#include <fstream>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#endif
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
    std::array<u32, 8> values{0x6ec00021U, 3U, 0xffffffffU, 0xabc12345U};
    u32 last_handle{};
    u32 cbuf_reads{};
    u32 texture_queries{};
    u32 last_cbuf_offset{};
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32 index, u32 offset) override { ++cbuf_reads; last_cbuf_offset=offset; return values.at(index); }
    TextureType ReadTextureType(u32 handle) override { ++texture_queries; last_handle = handle; return TextureType::Color2D; }
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
    IR::Value cbuf(u32 index, u32 offset=0) { return op(IR::Opcode::GetCbufU32,{IR::Value{index},IR::Value{offset}}); }
    IR::Value identity(IR::Value value) { return op(IR::Opcode::Identity,{value}); }
    IR::Value storage(IR::Value offset, u32 binding=0) {
        return op(IR::Opcode::LoadStorage32,{IR::Value{binding},offset});
    }
    IR::Value mask(IR::Value value,u32 bits) { return op(IR::Opcode::BitwiseAnd32,{value,IR::Value{bits}}); }
    IR::Value shl(IR::Value value,u32 bits) { return op(IR::Opcode::ShiftLeftLogical32,{value,IR::Value{bits}}); }
    IR::Value either(IR::Value lhs,IR::Value rhs) { return op(IR::Opcode::BitwiseOr32,{lhs,rhs}); }
    IR::Value query(IR::Value handle) {
        auto inst=op(IR::Opcode::BindlessImageQueryDimensions,{handle,IR::Value{0U}});
        IR::TextureInstInfo flags{};flags.type.Assign(TextureType::Color2D);inst.Inst()->SetFlags(flags);
        return inst;
    }
    IR::Value sample(IR::Value handle) {
        const auto coords=op(IR::Opcode::CompositeConstructF32x2,{IR::Value{0.0f},IR::Value{0.0f}});
        auto inst=op(IR::Opcode::BindlessImageSampleExplicitLod,{handle,coords,IR::Value{0.0f},IR::Value{}});
        IR::TextureInstInfo flags{};flags.type.Assign(TextureType::Color2D);inst.Inst()->SetFlags(flags);
        return inst;
    }
    void run() { HostTranslateInfo host{};host.support_snorm_render_buffer=true;Optimization::TexturePass(env,program,host); }
};
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
#ifdef TEST_HANDLE_SPIRV
std::filesystem::path module_directory;
u32 module_case{};
void emit_modules(Fixture& f) {
    if(module_directory.empty()) return;
    f.program.stage=Stage::Compute;
    f.program.workgroup_size={1,1,1};
    f.program.syntax_list={{{.block=&f.block},IR::AbstractSyntaxNode::Type::Block},
                          {{},IR::AbstractSyntaxNode::Type::Return}};
    // Keep the real input expressions as well as the lowered texture operations.
    for(u32 index=0;index<8;++index) f.program.info.constant_buffer_descriptors.push_back({index,1});
    f.program.info.used_constant_buffer_types=IR::Type::U32 | IR::Type::U32x2;
    for(u32 version: {0x10000U,0x10300U,0x10400U}) {
        Profile profile{};profile.supported_spirv=version;
        const auto words=Backend::SPIRV::EmitSPIRV(profile,f.program);
        const auto path=module_directory/(std::to_string(module_case)+"_"+std::to_string(version)+".spv");
        std::ofstream output{path,std::ios::binary};
        output.write(reinterpret_cast<const char*>(words.data()),static_cast<std::streamsize>(words.size()*sizeof(u32)));
        check(output.good(),"failed to write handle tracking module");
    }
    ++module_case;
}
#else
void emit_modules(Fixture&) {}
#endif
void test(const char* name,const std::function<IR::Value(Fixture&)>& make,u32 expected) {
    Fixture f;f.query(make(f));f.run();
    if(f.env.last_handle!=expected)throw std::runtime_error(std::string{name}+": expected "+std::to_string(expected)+", got "+std::to_string(f.env.last_handle));
    check(f.program.info.texture_descriptors.size()==1,"query descriptor was not generated");
    const auto& desc=f.program.info.texture_descriptors[0];
    const u32 primary=f.env.ReadCbufValue(desc.cbuf_index,desc.cbuf_offset);
    const u32 secondary=desc.has_secondary ? f.env.ReadCbufValue(desc.secondary_cbuf_index,desc.secondary_cbuf_offset) : 0;
    check(EvaluateTextureHandle(desc,primary,secondary)==expected,"emitted runtime descriptor disagrees with translation query");
    emit_modules(f);
}
void unsupported(const char* name,const std::function<IR::Value(Fixture&)>& make,
                 u32 expected_static_mask_reads=0) {
    Fixture f;
    // Rejection must happen in the collection phase, before rewriting even a valid access.
    const auto valid=f.query(f.cbuf(3));
    const auto handle=make(f);
    const auto invalid=f.query(handle);
    TextureDescriptor existing{};
    existing.type=TextureType::Color2D;
    existing.cbuf_index=3;
    existing.cbuf_offset=128;
    existing.mask=0xabcU;
    existing.count=1;
    f.program.info.texture_descriptors.push_back(existing);
    bool rejected=false;
    try { f.run(); } catch(const NotImplementedException& error) {
        rejected=true;
        std::cout<<"Rejected "<<name<<": "<<error.what()<<'\n';
    }
    check(rejected,(std::string{name}+": address/value provenance was accepted").c_str());
    check(f.env.texture_queries==0,"unsupported handle queried an invented texture descriptor");
    check(f.env.cbuf_reads==expected_static_mask_reads,"unexpected static CBUF mask reads");
    check(valid.Inst()->GetOpcode()==IR::Opcode::BindlessImageQueryDimensions &&
          invalid.Inst()->GetOpcode()==IR::Opcode::BindlessImageQueryDimensions &&
          invalid.Inst()->Arg(0)==handle,"failed tracking partially rewrote texture accesses");
    const auto& descriptors=f.program.info.texture_descriptors;
    check(descriptors.size()==1 && descriptors[0].cbuf_index==3 &&
          descriptors[0].cbuf_offset==128 && descriptors[0].mask==0xabcU,
          "failed tracking changed an existing descriptor");
}
int main(int argc,char** argv) {
#ifdef TEST_HANDLE_SPIRV
    if(argc==2) { module_directory=argv[1];std::filesystem::create_directories(module_directory); }
#else
    (void)argc;(void)argv;
#endif
    test("direct CBUF",[](Fixture& f){return f.cbuf(0);},0x6ec00021U);
    test("identities preserve values",[](Fixture& f){return f.identity(f.identity(f.cbuf(1)));},3U);
    test("selected composite lane",[](Fixture& f){
        const auto pair=f.op(IR::Opcode::CompositeConstructU32x2,{f.cbuf(0),f.cbuf(1)});
        return f.op(IR::Opcode::CompositeExtractU32x2,{pair,IR::Value{1U}});
    },3U);
    for(u32 lane: {0U,1U}) {
        Fixture f;
        const auto pair=f.op(IR::Opcode::GetCbufU32x2,{IR::Value{0U},IR::Value{64U}});
        f.query(f.op(IR::Opcode::CompositeExtractU32x2,{f.identity(pair),IR::Value{lane}}));
        f.run();
        check(f.program.info.texture_descriptors[0].cbuf_offset==64+lane*4 &&
              f.env.last_cbuf_offset==64+lane*4,"wrong word selected from CBUF pair");
        emit_modules(f);
    }
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
    emit_modules(distinct);
    unsupported("CBUF is only a storage address",[](Fixture& f){return f.storage(f.cbuf(0,608));});
    unsupported("captured storage load OR CBUF",[](Fixture& f){
        f.env.values[0]=0x6ec15200U;f.env.values[6]=0x10100000U;
        const auto address=f.identity(f.cbuf(0,608)); // %127 -> %133
        const auto aligned=f.mask(f.cbuf(0,608),0xfffffffcU); // %138 -> %139
        const auto offset=f.op(IR::Opcode::ISub32,{address,aligned}); // %140
        const auto loaded=f.identity(f.storage(offset)); // %141 -> %142
        const auto handle=f.identity(f.either(f.identity(loaded),f.cbuf(6))); // %212 -> %219
        f.sample(handle); // %220, the actual captured texture opcode
        return handle;
    });
    unsupported("CBUF OR storage load",[](Fixture& f){return f.either(f.cbuf(1),f.storage(f.cbuf(0)));});
    unsupported("variable storage offset",[](Fixture& f){
        const auto tid=f.op(IR::Opcode::InvocationId,{});
        return f.storage(f.op(IR::Opcode::IAdd32,{f.cbuf(0),tid}));
    });
    unsupported("two loads from aliased addresses",[](Fixture& f){
        const auto address=f.cbuf(0);
        return f.either(f.storage(address),f.storage(f.identity(address),1));
    });
    unsupported("AND around storage value",[](Fixture& f){return f.mask(f.storage(f.cbuf(0)),0xfffffU);});
    unsupported("storage value AND CBUF mask",[](Fixture& f){return f.op(IR::Opcode::BitwiseAnd32,{f.storage(f.cbuf(0)),f.cbuf(1)});});
    unsupported("CBUF mask AND storage value",[](Fixture& f){return f.op(IR::Opcode::BitwiseAnd32,{f.cbuf(1),f.storage(f.cbuf(0))});});
    // Valid static siblings may read their CBUF1 masks during collection; no
    // texture descriptor lookup or GPU-memory read is permitted on rejection.
    unsupported("valid masked sibling OR storage value",[](Fixture& f){
        const auto valid=f.op(IR::Opcode::BitwiseAnd32,{f.cbuf(0),f.cbuf(1)});
        return f.either(valid,f.storage(f.cbuf(0)));
    },1);
    unsupported("shift around storage value",[](Fixture& f){return f.shl(f.storage(f.cbuf(0)),20);});
    unsupported("atomic result is not its address",[](Fixture& f){
        return f.op(IR::Opcode::StorageAtomicIAdd32,{IR::Value{0U},f.cbuf(0),IR::Value{1U}});
    });
    unsupported("unmodelled add is not transparent",[](Fixture& f){return f.op(IR::Opcode::IAdd32,{f.cbuf(0),IR::Value{1U}});});
    unsupported("invalid OR must not keep only one branch",[](Fixture& f){return f.either(f.cbuf(0),IR::Value{1U});});
    unsupported("immediate handle has no descriptor source",[](Fixture&){return IR::Value{123U};});
    for(const auto opcode: {IR::Opcode::LoadStorageU8,IR::Opcode::LoadStorageS8,
                           IR::Opcode::LoadStorageU16,IR::Opcode::LoadStorageS16}) {
        unsupported("subword load is not its address",[opcode](Fixture& f){
            return f.op(opcode,{IR::Value{0U},f.cbuf(0)});
        });
    }
    unsupported("vector load lane is not its address",[](Fixture& f){
        const auto load=f.op(IR::Opcode::LoadStorage64,{IR::Value{0U},f.cbuf(0)});
        return f.op(IR::Opcode::CompositeExtractU32x2,{load,IR::Value{1U}});
    });
    unsupported("shared load is not its address",[](Fixture& f){return f.op(IR::Opcode::LoadSharedU32,{f.cbuf(0)});});
    unsupported("global load is not its address",[](Fixture& f){
        const auto pair=f.op(IR::Opcode::CompositeConstructU32x2,{f.cbuf(0),f.cbuf(1)});
        const auto address=f.op(IR::Opcode::PackUint2x32,{pair});
        return f.op(IR::Opcode::LoadGlobal32,{address});
    });
    unsupported("local load is not its address",[](Fixture& f){return f.op(IR::Opcode::LoadLocal,{f.cbuf(0)});});
    unsupported("phi must not choose an incoming CBUF",[](Fixture& f){
        const auto lhs=f.cbuf(0),rhs=f.cbuf(3);
        const auto phi=f.op(IR::Opcode::Phi,{});
        phi.Inst()->SetFlags(IR::Type::U32);
        phi.Inst()->AddPhiOperand(&f.block,lhs);
        phi.Inst()->AddPhiOperand(&f.block,rhs);
        return phi;
    });
    unsupported("unmodelled right shift",[](Fixture& f){return f.op(IR::Opcode::ShiftRightLogical32,{f.cbuf(0),IR::Value{2U}});});
    unsupported("CBUF pair lane must not wrap byte offset",[](Fixture& f){
        const auto pair=f.op(IR::Opcode::GetCbufU32x2,{IR::Value{0U},IR::Value{0xfffffffcU}});
        return f.op(IR::Opcode::CompositeExtractU32x2,{pair,IR::Value{1U}});
    });
    Fixture dynamic_cbuf;
    const auto dynamic_offset=dynamic_cbuf.op(IR::Opcode::IAdd32,{IR::Value{64U},dynamic_cbuf.shl(dynamic_cbuf.op(IR::Opcode::InvocationId,{}),3)});
    dynamic_cbuf.query(dynamic_cbuf.op(IR::Opcode::GetCbufU32,{IR::Value{0U},dynamic_offset}));
    dynamic_cbuf.run();
    check(dynamic_cbuf.program.info.texture_descriptors[0].count==8 &&
          dynamic_cbuf.program.info.texture_descriptors[0].cbuf_offset==64,
          "existing bounded CBUF descriptor array changed");
    Fixture bound;
    bound.op(IR::Opcode::BoundImageQueryDimensions,{IR::Value{128U},IR::Value{0U}});
    bound.run();
    check(bound.program.info.texture_descriptors[0].cbuf_index==0 &&
          bound.program.info.texture_descriptors[0].cbuf_offset==128,
          "bound texture descriptor changed");
    Fixture selected;
    const auto pair=selected.op(IR::Opcode::CompositeConstructU32x2,{selected.storage(selected.cbuf(0)),selected.cbuf(3)});
    selected.query(selected.op(IR::Opcode::CompositeExtractU32x2,{pair,IR::Value{1U}}));
    selected.run();
    check(selected.program.info.texture_descriptors[0].cbuf_index==3,
          "unselected memory value poisoned a static selected lane");
    // Unused loads and aliasing writes elsewhere must not poison independent static handles.
    Fixture aliases;
    const auto address=aliases.cbuf(0);
    aliases.storage(address);
    aliases.op(IR::Opcode::WriteStorage32,{IR::Value{0U},address,IR::Value{123U}});
    for(u32 access=0;access<256;++access) aliases.query(aliases.cbuf(3));
    aliases.run();
    check(aliases.program.info.texture_descriptors.size()==1,"independent accesses no longer share their descriptor");
    std::cout<<"PASS real TexturePass masks, shifts, split handles, query/runtime agreement and deduplication\n";
}
