// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>

#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/frontend/ir/modifiers.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& message) { std::cerr << message << '\n'; }
void Error(const std::string& message) { std::cerr << message << '\n'; }
}

int main(int argc, char** argv) {
    using namespace Shader;
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    struct Shape { TextureType type; u32 spatial; bool array; };
    const Shape shapes[]{
        {TextureType::Color1D, 1, false}, {TextureType::ColorArray1D, 1, true},
        {TextureType::Color2D, 2, false}, {TextureType::ColorArray2D, 2, true},
        {TextureType::Color3D, 3, false}, {TextureType::Buffer, 1, false},
        {TextureType::Color2DRect, 2, false},
    };
    for (u32 version : {0x00010000U, 0x00010300U, 0x00010400U}) {
      for (bool samplerless : {false, true}) {
        for (u32 shape = 0; shape < std::size(shapes); ++shape) {
            if (samplerless && shapes[shape].type == TextureType::Buffer) continue;
            for (u32 offset_kind = 0; offset_kind < 3; ++offset_kind) {
                for (bool sparse : {false, true}) {
                    const auto [type, spatial, array] = shapes[shape];
                    Profile profile{};
                    profile.supported_spirv = version;
                    ObjectPool<IR::Inst> pool;
                    IR::Block block{pool};
                    IR::Program program{};
                    program.stage = Stage::Compute;
                    program.workgroup_size = {1, 1, 1};
                    program.blocks = {&block};
                    program.post_order_blocks = {&block};
                    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                                           {{}, IR::AbstractSyntaxNode::Type::Return}};
                    program.info.uses_sparse_residency = sparse;
                    program.info.uses_sampled_1d = type == TextureType::Color1D || type == TextureType::ColorArray1D;
                    if (type == TextureType::Buffer) {
                        program.info.texture_buffer_descriptors.push_back({.count = 1});
                    } else {
                        program.info.texture_descriptors.push_back({.type = type, .is_samplerless = samplerless, .count = 1});
                    }
                    auto emit = [&](IR::Opcode op, std::initializer_list<IR::Value> args) {
                        block.AppendNewInst(op, args);
                        return IR::Value{&block.Instructions().back()};
                    };
                    auto vector = [&](u32 count, const std::array<IR::Value, 3>& components) {
                        if (count == 1) return components[0];
                        if (count == 2) return emit(IR::Opcode::CompositeConstructU32x2,
                                                   {components[0], components[1]});
                        return emit(IR::Opcode::CompositeConstructU32x3,
                                    {components[0], components[1], components[2]});
                    };
                    std::array<IR::Value, 3> coordinate_parts{IR::Value{3U}, IR::Value{11U}, IR::Value{7U}};
                    if (array) coordinate_parts[spatial] = IR::Value{23U};
                    const IR::Value coordinates{vector(spatial + (array ? 1 : 0), coordinate_parts)};
                    IR::Value offset;
                    if (offset_kind != 0) {
                        std::array<IR::Value, 3> parts{IR::Value{0xfffffffeU}, IR::Value{3U}, IR::Value{0xffffffffU}};
                        if (offset_kind == 2) {
                            for (auto& part : parts) part = emit(IR::Opcode::IAdd32, {part, IR::Value{0U}});
                        }
                        offset = vector(spatial, parts);
                    }
                    // A nonzero LOD catches accidental loss of Lod while removing Offset.
                    const auto fetch = emit(IR::Opcode::ImageFetch,
                                            {IR::Value{0U}, coordinates, offset, IR::Value{2U}, IR::Value{}});
                    IR::TextureInstInfo info{};
                    // Rect descriptors use Dim2D, and fetch IR carries the 2D instruction shape.
                    info.type.Assign(type == TextureType::Color2DRect ? TextureType::Color2D : type);
                    fetch.Inst()->SetFlags(info);
                    if (sparse) emit(IR::Opcode::GetSparseFromOp, {fetch});
                    const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
                    const auto path = std::filesystem::path{argv[1]} /
                        (std::to_string(version) + "_" + std::to_string(shape) + "_" +
                         std::to_string(offset_kind) + "_" + std::to_string(sparse) + "_" +
                         std::to_string(samplerless) + ".spv");
                    std::ofstream file{path, std::ios::binary};
                    file.write(reinterpret_cast<const char*>(words.data()),
                               static_cast<std::streamsize>(words.size() * sizeof(u32)));
                }
            }
        }
      }
      // QueryDimensions shares TextureImage with fetch. QueryLod must retain a sampler.
      for (u32 operation = 0; operation < 3; ++operation) {
        const bool samplerless{operation == 1};
        Profile profile{};
        profile.supported_spirv = version;
        ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::Program program{};
        program.stage = Stage::Fragment;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.texture_descriptors.push_back({.type = TextureType::Color2D,
                                                   .is_samplerless = samplerless, .count = 1});
        IR::TextureInstInfo info{};
        info.type.Assign(TextureType::Color2D);
        if (operation < 2) {
            block.AppendNewInst(IR::Opcode::ImageQueryDimensions, {IR::Value{0U}, IR::Value{2U}});
        } else {
            block.AppendNewInst(IR::Opcode::CompositeConstructF32x2, {IR::Value{0.25F}, IR::Value{0.75F}});
            const IR::Value coords{&block.Instructions().back()};
            block.AppendNewInst(IR::Opcode::ImageQueryLod, {IR::Value{0U}, coords});
        }
        block.Instructions().back().SetFlags(info);
        const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
        const auto path = std::filesystem::path{argv[1]} /
                          ("query_" + std::to_string(version) + "_" + std::to_string(operation) + ".spv");
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(words.data()),
                   static_cast<std::streamsize>(words.size() * sizeof(u32)));
      }
    }
}
