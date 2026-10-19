// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>

#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/exception.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& message) { std::cerr << message << '\n'; }
void Error(const std::string& message) { std::cerr << message << '\n'; }
}

int main(int argc, char** argv) {
    using namespace Shader;
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    for (u32 buffer = 0; buffer < 2; ++buffer) {
        for (u32 format = 0; format < 8; ++format) {
            Profile profile{};
            profile.supported_spirv = 0x00010300;
            ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            IR::Program program{};
            program.stage = Stage::Compute;
            program.workgroup_size = {1, 1, 1};
            program.blocks = {&block};
            program.post_order_blocks = {&block};
            program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                                   {{}, IR::AbstractSyntaxNode::Type::Return}};
            if (buffer) {
                program.info.uses_image_buffers = true;
                program.info.image_buffer_descriptors.push_back({
                    .format = static_cast<ImageFormat>(format), .is_integer = true, .count = 1});
            } else {
                program.info.image_descriptors.push_back({
                    .type = TextureType::Color2D, .format = static_cast<ImageFormat>(format),
                    .is_integer = true, .count = 1});
            }
            const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
            const auto path = std::filesystem::path{argv[1]} /
                              (std::to_string(buffer) + "_" + std::to_string(format) + ".spv");
            std::ofstream file{path, std::ios::binary};
            file.write(reinterpret_cast<const char*>(words.data()),
                       static_cast<std::streamsize>(words.size() * sizeof(u32)));
        }
    }
#ifdef TEST_TYPELESS_WRITES
    for (bool supported : {false, true}) {
        Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.support_typeless_image_writes = supported;
        ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::Program program{};
        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.uses_typeless_image_writes = true;
        try {
            const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
            if (!supported) return 3;
            const auto path = std::filesystem::path{argv[1]} / "typeless.spv";
            std::ofstream file{path, std::ios::binary};
            file.write(reinterpret_cast<const char*>(words.data()),
                       static_cast<std::streamsize>(words.size() * sizeof(u32)));
        } catch (const NotImplementedException&) {
            if (supported) return 4;
        }
    }
#endif
}
