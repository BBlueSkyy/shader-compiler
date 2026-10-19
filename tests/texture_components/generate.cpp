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
    for (u32 version : {0x00010000U, 0x00010300U, 0x00010400U}) {
        for (u32 operation = 0; operation < 6; ++operation) {
            for (bool sparse : {false, true}) {
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
                program.info.uses_sparse_residency = sparse;
                auto emit = [&](IR::Opcode op, std::initializer_list<IR::Value> args) {
                    block.AppendNewInst(op, args);
                    return IR::Value{&block.Instructions().back()};
                };
                // All three scalar types coexist to catch a buffer type shared across descriptors.
                for (u32 component = 0; component < 3; ++component) {
                    const auto component_type{static_cast<TextureComponentType>(component)};
                    if (operation == 0) {
                        program.info.texture_buffer_descriptors.push_back({.component_type = component_type, .count = 1});
                    } else {
                        program.info.texture_descriptors.push_back({.type = TextureType::Color2D,
                            .is_samplerless = operation == 1, .component_type = component_type, .count = 1});
                    }
                    const IR::Value index{0U};
                    const IR::Value coords{emit(IR::Opcode::CompositeConstructF32x2,
                                               {IR::Value{0.25F}, IR::Value{0.75F}})};
                    IR::Value sample;
                    switch (operation) {
                    case 0:
                        sample = emit(IR::Opcode::ImageFetch, {index, IR::Value{3U}, IR::Value{}, IR::Value{0U}, IR::Value{}});
                        break;
                    case 1: {
                        const auto integer_coords{emit(IR::Opcode::CompositeConstructU32x2, {IR::Value{3U}, IR::Value{4U}})};
                        sample = emit(IR::Opcode::ImageFetch, {index, integer_coords, IR::Value{}, IR::Value{0U}, IR::Value{}});
                        break;
                    }
                    case 2:
                        sample = emit(IR::Opcode::ImageSampleImplicitLod, {index, coords, IR::Value{}, IR::Value{}});
                        break;
                    case 3:
                        sample = emit(IR::Opcode::ImageSampleExplicitLod, {index, coords, IR::Value{1.0F}, IR::Value{}});
                        break;
                    case 4:
                        sample = emit(IR::Opcode::ImageGather, {index, coords, IR::Value{}, IR::Value{}});
                        break;
                    default: {
                        const auto gradients{emit(IR::Opcode::CompositeConstructF32x4,
                            {IR::Value{0.125F}, IR::Value{0.0F}, IR::Value{0.0F}, IR::Value{0.125F}})};
                        sample = emit(IR::Opcode::ImageGradient, {index, coords, gradients, IR::Value{}, IR::Value{}});
                        break;
                    }
                    }
                    IR::TextureInstInfo info{};
                    info.type.Assign(operation == 0 ? TextureType::Buffer : TextureType::Color2D);
                    info.descriptor_index.Assign(component);
                    info.num_derivates.Assign(2);
                    sample.Inst()->SetFlags(info);
                    if (sparse) emit(IR::Opcode::GetSparseFromOp, {sample});
                    // Exercise the existing float-typed IR consumer after the integer image result.
                    emit(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0U}});
                }
                const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
                const auto path = std::filesystem::path{argv[1]} /
                    (std::to_string(version) + "_" + std::to_string(operation) + "_" + std::to_string(sparse) + ".spv");
                std::ofstream file{path, std::ios::binary};
                file.write(reinterpret_cast<const char*>(words.data()),
                           static_cast<std::streamsize>(words.size() * sizeof(u32)));
            }
        }
    }
}
