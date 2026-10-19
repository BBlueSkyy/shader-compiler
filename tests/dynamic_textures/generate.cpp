// SPDX-License-Identifier: GPL-2.0-or-later
// Executable production-backend kernels for descriptor-indexing Vulkan tests.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/frontend/ir/modifiers.h>
namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& text) { std::cerr << text << '\n'; }
void Error(const std::string& text) { std::cerr << text << '\n'; }
}
int main(int argc, char** argv) {
    using namespace Shader;
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    for (u32 version : {0x10000U, 0x10300U, 0x10400U}) {
        for (u32 operation = 0; operation < 10; ++operation) {
          Backend::Bindings bindings;
          for (Stage stage : {Stage::Compute, Stage::VertexB, Stage::TessellationControl,
                              Stage::TessellationEval, Stage::Geometry, Stage::Fragment}) {
            if (stage == Stage::Compute && operation >= 6 && operation != 9) continue;
            if (stage == Stage::VertexB) bindings = {};
            ObjectPool<IR::Inst> pool;
            IR::Block block{pool}, body{pool}, merge{pool};
            IR::Block* active=&block;
            IR::Program program{};
            program.stage = stage;
            program.workgroup_size = {64, 1, 1};
            program.invocations = 1;
            program.output_vertices = 1;
            program.output_topology = OutputTopology::PointList;
            program.blocks = {&block};
            program.post_order_blocks = {&block};
            program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                                   {{}, IR::AbstractSyntaxNode::Type::Return}};
            program.info.uses_local_invocation_id = stage == Stage::Compute;
            program.info.used_storage_buffer_types = IR::Type::U32;
            program.info.storage_buffers_descriptors.push_back({0, 0, 1, false});
            program.info.storage_buffers_descriptors.push_back({0, 16, 1, true});
            program.info.dynamic_textures.push_back({TextureType::Color2D, false, operation == 1 || operation == 2});
            const auto emit = [&](IR::Opcode opcode, std::initializer_list<IR::Value> args) {
                active->AppendNewInst(opcode, args);
                return IR::Value{&active->Instructions().back()};
            };
            IR::Value lane{0U};
            if (stage == Stage::Compute) {
                const auto local = emit(IR::Opcode::LocalInvocationId, {});
                lane = emit(IR::Opcode::CompositeExtractU32x3, {local, IR::Value{0U}});
            } else if (stage == Stage::VertexB) {
                program.info.loads.Set(IR::Attribute::VertexId, true);
                lane = emit(IR::Opcode::GetAttributeU32,
                            {IR::Value{IR::Attribute::VertexId}, IR::Value{0U}});
            }
            const auto byte_offset = emit(IR::Opcode::ShiftLeftLogical32, {lane, IR::Value{2U}});
            IR::Value handle = emit(IR::Opcode::LoadStorage32, {IR::Value{0U}, byte_offset});
            if (operation == 3) {
                const auto tic = emit(IR::Opcode::BitwiseAnd32, {handle, IR::Value{0xfffffU}});
                const auto sampler = emit(IR::Opcode::ShiftRightLogical32, {handle, IR::Value{20U}});
                const auto shifted = emit(IR::Opcode::ShiftLeftLogical32, {sampler, IR::Value{20U}});
                handle = emit(IR::Opcode::BitwiseOr32, {tic, shifted});
            }
            if (operation == 5) {
                const auto odd = emit(IR::Opcode::BitwiseAnd32, {lane, IR::Value{1U}});
                const IR::U1 condition{emit(IR::Opcode::IEqual, {odd, IR::Value{0U}})};
                const auto original=handle;
                active=&body;
                const auto alternative=emit(IR::Opcode::BitwiseOr32, {original, IR::Value{0U}});
                active=&merge;
                handle=emit(IR::Opcode::Phi, {});
                handle.Inst()->SetFlags(IR::Type::U32);
                handle.Inst()->AddPhiOperand(&block,original);
                handle.Inst()->AddPhiOperand(&body,alternative);
                program.blocks={&block,&body,&merge};
                program.post_order_blocks={&merge,&body,&block};
                using Node=IR::AbstractSyntaxNode;
                program.syntax_list={
                    {{.block=&block},Node::Type::Block},
                    {{.if_node={condition,&body,&merge}},Node::Type::If},
                    {{.block=&body},Node::Type::Block},
                    {{.end_if={&merge}},Node::Type::EndIf},
                    {{.block=&merge},Node::Type::Block},
                    {{},Node::Type::Return}};
            }
            IR::Value result;
            IR::TextureInstInfo info{};
            info.type.Assign(TextureType::Color2D);
            if (operation == 1) {
                result = emit(IR::Opcode::BindlessImageQueryDimensions, {handle, IR::Value{0U}});
            } else if (operation == 2) {
                const auto coords = emit(IR::Opcode::CompositeConstructU32x2, {IR::Value{0U}, IR::Value{0U}});
                result = emit(IR::Opcode::BindlessImageFetch, {handle, coords, IR::Value{}, IR::Value{0U}, IR::Value{}});
            } else {
                const auto coords = emit(IR::Opcode::CompositeConstructF32x2, {IR::Value{0.5F}, IR::Value{0.5F}});
                if (operation == 6 || operation == 8) {
                    info.has_bias.Assign(operation == 8);
                    info.has_lod_clamp.Assign(operation == 8);
                    const auto bias_clamp = operation == 8 ?
                        emit(IR::Opcode::CompositeConstructF32x2, {IR::Value{1.0F}, IR::Value{0.0F}}) : IR::Value{};
                    result = emit(IR::Opcode::BindlessImageSampleImplicitLod, {handle, coords, bias_clamp, IR::Value{}});
                } else if (operation == 7 || operation == 9) {
                    info.num_derivates.Assign(2);
                    info.has_lod_clamp.Assign(operation == 9);
                    const auto grad = emit(IR::Opcode::CompositeConstructF32x4, {IR::Value{0.1F}, IR::Value{0.0F}, IR::Value{0.0F}, IR::Value{0.1F}});
                    result = emit(IR::Opcode::BindlessImageGradient, {handle, coords, grad, IR::Value{}, operation == 9 ? IR::Value{1.0F} : IR::Value{}});
                } else if (operation == 4) {
                    // Dynamic offset is passed into the helper, never referenced from main.
                    const auto offset = emit(IR::Opcode::CompositeConstructU32x2, {lane, IR::Value{0U}});
                    result = emit(IR::Opcode::BindlessImageGather, {handle, coords, offset, IR::Value{}});
                } else {
                    result = emit(IR::Opcode::BindlessImageSampleExplicitLod, {handle, coords, IR::Value{0.0F}, IR::Value{}});
                }
            }
            result.Inst()->SetFlags(info);
            const auto output_base = emit(IR::Opcode::ShiftLeftLogical32, {lane, IR::Value{4U}});
            for (u32 channel = 0; channel < 4; ++channel) {
                auto value = emit(operation == 1 ? IR::Opcode::CompositeExtractU32x4 : IR::Opcode::CompositeExtractF32x4,
                                  {result, IR::Value{channel}});
                if (operation != 1) value = emit(IR::Opcode::BitCastU32F32, {value});
                const auto offset = emit(IR::Opcode::IAdd32, {output_base, IR::Value{channel * 4}});
                emit(IR::Opcode::WriteStorage32, {IR::Value{1U}, offset, value});
            }
            Profile profile{};
            profile.supported_spirv = version;
            profile.support_dynamic_texture_handles = true;
            profile.support_min_lod = true;
            if ((stage == Stage::Fragment && operation == 8) || operation == 9) {
                Profile partial=profile;
                partial.support_min_lod=false;
                bool rejected=false;
                try { (void)Backend::SPIRV::EmitSPIRV(partial,program); }
                catch (const NotImplementedException&) { rejected=true; }
                if (!rejected) throw std::runtime_error("dynamic gradient clamp accepted disabled MinLod feature");
            }
            if (operation == 0 && version == 0x10300) {
                Profile disabled=profile;
                disabled.support_dynamic_texture_handles=false;
                bool rejected=false;
                try { (void)Backend::SPIRV::EmitSPIRV(disabled,program); }
                catch (const NotImplementedException&) { rejected=true; }
                if (!rejected) throw std::runtime_error("dynamic backend accepted disabled profile");
            }
            if (stage == Stage::Fragment && operation == 4) {
                bool rejected=false;
                try { (void)Backend::SPIRV::EmitSPIRV(profile, {}, program, bindings); }
                catch (const NotImplementedException&) { rejected=true; }
                if (!rejected) throw std::runtime_error("fragment dynamic gather accepted divergent implicit derivatives");
                continue;
            }
            const u32 group_base=bindings.dynamic_texture;
            const auto words = Backend::SPIRV::EmitSPIRV(profile, {}, program, bindings);
            if (bindings.dynamic_texture != group_base + program.info.dynamic_textures.size() ||
                result.Inst()->Flags<IR::TextureInstInfo>().descriptor_index != 0) {
                throw std::runtime_error("stage group allocation changed local IR indices");
            }
            const char* names[]{"sample", "query", "fetch", "transform", "offset", "phi", "implicit", "gradient", "implicit_bias", "gradient_clamp"};
            const char* prefixes[]{"vertex_", "tess_control_", "tess_eval_", "geometry_", "fragment_", ""};
            const std::string prefix{prefixes[static_cast<u32>(stage)]};
            const auto name = prefix + std::string{names[operation]} + (version == 0x10300 ? "" : "_" + std::to_string(version)) + ".spv";
            std::ofstream output{std::filesystem::path{argv[1]} / name, std::ios::binary};
            output.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(u32)));
          }
        }
    }
}
