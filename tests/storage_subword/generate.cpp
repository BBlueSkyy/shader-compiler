// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>

#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/common/log.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& message) { std::cerr << message << '\n'; }
void Error(const std::string& message) { std::cerr << message << '\n'; }
}

int main(int argc, char** argv) {
    using namespace Shader;
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    const IR::Opcode stores[]{IR::Opcode::WriteStorageU8, IR::Opcode::WriteStorageS8,
                              IR::Opcode::WriteStorageU16, IR::Opcode::WriteStorageS16};
    const IR::Opcode loads[]{IR::Opcode::LoadStorageU8, IR::Opcode::LoadStorageS8,
                             IR::Opcode::LoadStorageU16, IR::Opcode::LoadStorageS16};
    for (u32 version : {0x00010000U, 0x00010300U, 0x00010400U}) {
        for (u32 features = 0; features < 32; ++features) {
            for (u32 kind = 0; kind < 6; ++kind) {
                Profile profile{};
                profile.supported_spirv = version;
                profile.support_descriptor_aliasing = (features & 1) != 0;
                profile.support_int8 = (features & 2) != 0;
                profile.support_int16 = (features & 4) != 0;
    #ifdef TEST_STORAGE_FEATURES
                profile.support_storage8 = (features & 8) != 0;
                profile.support_storage16 = (features & 16) != 0;
    #endif
                ObjectPool<IR::Inst> pool;
                IR::Block block{pool};
                IR::Program program{};
                program.stage = Stage::Compute;
                program.workgroup_size = {1, 1, 1};
                program.blocks = {&block};
                program.post_order_blocks = {&block};
                program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                                       {{}, IR::AbstractSyntaxNode::Type::Return}};
                program.info.uses_int8 = kind < 2 || kind >= 4;
                program.info.uses_int16 = kind >= 2;
                program.info.used_storage_buffer_types = kind >= 4 ? IR::Type::U8 | IR::Type::U16
                                                        : kind < 2 ? IR::Type::U8 : IR::Type::U16;
                program.info.storage_buffers_descriptors.push_back({0, 0, 1, kind != 5});
                // Include both constant and dynamically computed byte offsets, and both buffers.
                program.info.storage_buffers_descriptors.push_back({0, 16, 1, kind != 5});
                for (u32 binding = 0; binding < 2; ++binding) {
                    for (u32 offset = 0; offset < 8; ++offset) {
                        for (bool dynamic : {false, true}) {
                            IR::Value address{offset};
                            if (dynamic) {
                                block.AppendNewInst(IR::Opcode::IAdd32, {IR::Value{offset}, IR::Value{0U}});
                                address = IR::Value{&block.Instructions().back()};
                            }
                            const u32 first_kind{kind >= 4 ? 0U : kind};
                            const u32 end_kind{kind >= 4 ? 4U : kind + 1};
                            for (u32 operation = first_kind; operation < end_kind; ++operation) {
                                if (kind != 5) {
                                    block.AppendNewInst(stores[operation], {IR::Value{binding}, address,
                                                                           IR::Value{0xfedcba98U}});
                                }
                                block.AppendNewInst(loads[operation], {IR::Value{binding}, address});
                            }
                        }
                    }
                }
                const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
                const auto path = std::filesystem::path{argv[1]} /
                                  (std::to_string(version) + "_" + std::to_string(features) + "_" + std::to_string(kind) + ".spv");
                std::ofstream file{path, std::ios::binary};
                file.write(reinterpret_cast<const char*>(words.data()),
                           static_cast<std::streamsize>(words.size() * sizeof(u32)));
            }
        }
    }
}
