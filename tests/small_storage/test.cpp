// SPDX-License-Identifier: GPL-2.0-or-later
// Tests execute the production IR -> SPIR-V backend, never a model of the emitter.
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
using namespace Shader;
namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string&) {}
void Error(const std::string&) {}
}

static void Emit(const std::filesystem::path& dir, unsigned mask, u32 version, unsigned kind) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program p;
    p.stage = Stage::Compute;
    p.workgroup_size = {256, 1, 1};
    p.blocks = {&block}; p.post_order_blocks = {&block};
    p.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    p.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    p.info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
    p.info.used_storage_buffer_types = IR::Type::U8 | IR::Type::U16 | IR::Type::U32;
    p.info.uses_int8 = p.info.uses_int16 = true;
    p.info.uses_local_invocation_id = true;
    IR::IREmitter ir{block};
    const auto lane = ir.LocalInvocationIdX();
    const auto byte = ir.BitwiseAnd(lane, ir.Imm32(3u));
    const auto half = ir.BitwiseAnd(lane, ir.Imm32(1u));
    const auto word_offset = ir.ShiftLeftLogical(lane, ir.Imm32(2u));
    auto append = [&](IR::Opcode op, const IR::U32& binding, const IR::U32& offset, const IR::U32& value) {
        block.PrependNewInst(block.end(), op, {binding, offset, value});
    };
    auto store = [&](IR::Opcode op, const IR::U32& offset, const IR::U32& value) {
        append(op, ir.Imm32(0u), offset, value);
    };
    if (kind == 0 || kind == 1 || kind == 2) {
        if (kind != 2) {
            store(IR::Opcode::WriteStorageU8, lane, ir.IAdd(byte, ir.Imm32(0x140u)));
            store(IR::Opcode::WriteStorageS8, ir.IAdd(lane, ir.Imm32(256u)), ir.IAdd(byte, ir.Imm32(0xffffff80u)));
            store(IR::Opcode::WriteStorageU8, ir.IAdd(word_offset, ir.Imm32(2049u)), ir.Imm32(0x180u));
            store(IR::Opcode::WriteStorageS8, ir.IAdd(word_offset, ir.Imm32(7171u)), ir.Imm32(0xffffffefu));
        }
        if (kind != 1) {
            const auto offset = ir.ShiftLeftLogical(lane, ir.Imm32(1u));
            store(IR::Opcode::WriteStorageU16, ir.IAdd(offset, ir.Imm32(512u)), ir.IAdd(half, ir.Imm32(0x11234u)));
            store(IR::Opcode::WriteStorageS16, ir.IAdd(offset, ir.Imm32(1024u)), ir.IAdd(half, ir.Imm32(0xffff8000u)));
            store(IR::Opcode::WriteStorageU16, ir.IAdd(word_offset, ir.Imm32(3074u)), ir.Imm32(0x1beefu));
            // Same floor-to-halfword alignment as the existing native path.
            store(IR::Opcode::WriteStorageS16, ir.IAdd(word_offset, ir.Imm32(4099u)), ir.Imm32(0xffff8123u));
        }
    } else if (kind == 3) {
        for (auto [op, offset, output] : {std::tuple{IR::Opcode::LoadStorageU8, 6144u, 0u},
                {IR::Opcode::LoadStorageS8, 6145u, 4u}, {IR::Opcode::LoadStorageU16, 6146u, 8u},
                {IR::Opcode::LoadStorageS16, 6148u, 12u}}) {
            auto it = block.PrependNewInst(block.end(), op, {ir.Imm32(0u), ir.Imm32(offset)});
            store(IR::Opcode::WriteStorage32, ir.IAdd(ir.ShiftLeftLogical(lane, ir.Imm32(4u)), ir.Imm32(output)), IR::U32{IR::Value{&*it}});
        }
    } else if (kind == 4 || kind == 5) {
        for (u32 i = 0; i < (kind == 5 ? Info::MAX_INDIRECT_CBUFS : 1u); ++i)
            p.info.constant_buffer_descriptors.push_back({.index = i, .count = 1});
        p.info.used_constant_buffer_types = IR::Type::U8 | IR::Type::U16;
        p.info.uses_cbuf_indirect = kind == 5;
        p.info.used_indirect_cbuf_types = kind == 5 ? IR::Type::U8 | IR::Type::U16 : IR::Type::Void;
        // Dynamic offset covers both immediate and indirect accessor generation.
        auto off = kind == 5 ? ir.BitwiseAnd(lane, ir.Imm32(0u)) : ir.Imm32(0u);
        for (unsigned bits : {8u,16u}) for (bool sign : {false,true}) {
            const auto value = ir.GetCbuf(ir.Imm32(0u), off, bits, sign);
            store(IR::Opcode::WriteStorage32, ir.IAdd(ir.ShiftLeftLogical(lane, ir.Imm32(4u)), ir.Imm32((bits == 8 ? 0u : 8u) + (sign ? 4u : 0u))), IR::U32{value});
        }
    } else if (kind == 6) {
        // Out-of-range words must not enter the CAS retry loop, even without robustness.
        store(IR::Opcode::WriteStorageU8, ir.Imm32(8192u), ir.Imm32(0xffu));
        store(IR::Opcode::WriteStorageS16, ir.Imm32(0xfffffffeu), ir.Imm32(0xffffu));
    } else if (kind == 7) {
        // Two different bindings and a read-only narrow input must not confuse helper discovery.
        p.info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
        append(IR::Opcode::WriteStorageU8, ir.Imm32(1u), lane, ir.Imm32(0x42u));
        store(IR::Opcode::WriteStorageS16, ir.IAdd(word_offset, ir.Imm32(2u)), ir.Imm32(0x8765u));
    } else if (kind == 8) {
        append(IR::Opcode::WriteStorageU8, lane, ir.Imm32(0u), ir.Imm32(0u));
    } else if (kind == 9 || kind == 10) {
        append(IR::Opcode::WriteStorageU8, ir.Imm32(kind == 9 ? 32u : 31u), ir.Imm32(0u), ir.Imm32(0u));
    }
    Profile profile;
    profile.supported_spirv = version;
    profile.unified_descriptor_binding = true;
    profile.support_int8 = mask & 1; profile.support_int16 = mask & 2;
    profile.support_storage_buffer_8bit = mask & 4;
    profile.support_storage_buffer_16bit = mask & 8;
    profile.support_uniform_buffer_8bit = mask & 16;
    profile.support_uniform_buffer_16bit = mask & 32;
    profile.support_descriptor_aliasing = mask & 64;
    auto words = Backend::SPIRV::EmitSPIRV(profile, p);
    const auto path = dir / (std::to_string(version) + "-" + std::to_string(mask) + "-" + std::to_string(kind) + ".spv");
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
    assert(f.good());
}
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    std::filesystem::create_directories(argv[1]);
    if (argc == 3) { Emit(argv[1],67,0x10000,0); return 0; }
    for (u32 version : {0x10000u, 0x10400u})
        for (unsigned mask = 0; mask < 128; ++mask)
            for (unsigned kind = 0; kind < 8; ++kind) Emit(argv[1], mask, version, kind);
    for (unsigned kind : {8u,9u,10u}) {
        bool rejected{};
        try { Emit(argv[1],64,0x10000,kind); }
        catch (const std::exception&) { rejected=true; }
        assert(rejected);
    }
    std::cout << "Generated 2048 production-backend modules; dynamic/invalid bindings rejected\n";
}
