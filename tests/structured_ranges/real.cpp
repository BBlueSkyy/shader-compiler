// Offline replay of captured input. Unexpected callbacks fail closed.
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <shader_compiler/environment.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/backend/spirv/emit_spirv.h>
namespace Shader::Log {
void Debug(const std::string &s) { std::cerr << s << '\n'; }
void Warn(const std::string &s) { std::cerr << s << '\n'; }
void Error(const std::string &s) { std::cerr << s << '\n'; }
} // namespace Shader::Log
class CaptureEnvironment final : public Shader::Environment {
    std::vector<char> code;

  public:
    CaptureEnvironment(const char *path) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            throw std::runtime_error("missing Maxwell");
        code.assign(std::istreambuf_iterator<char>(f), {});
        stage = Shader::Stage::Compute;
        start_address = 0x2F70000;
        is_propietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override {
        if (address < start_address || size_t(address - start_address) + 8 > code.size())
            throw std::runtime_error("uncaptured instruction");
        u64 value;
        std::memcpy(&value, code.data() + address - start_address, 8);
        return value;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        constexpr u32 values[]{0xBC996E30, 0xBE593484, 0x3FC90DA4, 0x40490FDB};
        if (index != 1 || offset % 4 || offset >= 16)
            throw std::runtime_error(fmt::format("missing cbuf {}:{:X}", index, offset));
        std::cerr << fmt::format("CBUF {}:{:X}={:X}\n", index, offset, values[offset / 4]);
        return values[offset / 4];
    }
    Shader::TextureType ReadTextureType(u32) override {
        throw std::runtime_error("missing texture type");
    }
    Shader::CompareFunction ReadTextureCompareFunc(u32) override {
        throw std::runtime_error("missing texture compare");
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        throw std::runtime_error("missing texture format");
    }
    bool IsTexturePixelFormatInteger(u32) override {
        throw std::runtime_error("missing integer texture");
    }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64) override {}
};
int main(int argc, char **argv) try {
    if (argc != 4)
        throw std::runtime_error("usage: replay MAXWELL OUTPUT_PREFIX SSBO_ALIGNMENT");
    CaptureEnvironment env(argv[1]);
    Shader::ObjectPool<Shader::IR::Inst> ip;
    Shader::ObjectPool<Shader::IR::Block> bp;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> fp;
    Shader::Maxwell::Flow::CFG cfg(env, fp, Shader::Maxwell::Location{env.StartAddress()});
    Shader::HostTranslateInfo host{};
    host.support_float16 = true;
    host.support_int64 = false;
    host.support_snorm_render_buffer = true;
    host.support_viewport_index_layer = true;
    host.min_ssbo_alignment = std::stoul(argv[3]);
    Shader::Settings::values = {.renderer_debug = false,
                                .disable_shader_loop_safety_checks = false,
                                .resolution_info = {.active = false}};
    auto program = Shader::Maxwell::TranslateProgram(ip, bp, env, cfg, host);
    const std::string prefix(argv[2]);
    std::ofstream(prefix + ".ir") << Shader::IR::DumpProgram(program);
    std::ofstream desc(prefix + ".descriptors");
    for (const auto &d : program.info.storage_buffers_descriptors) {
        desc << fmt::format("cbuf={} offset=0x{:X} count={} written={}", d.cbuf_index,
                            d.cbuf_offset, d.count, d.is_written);
#ifdef MODERN_SSBO
        desc << fmt::format(" has_size={} known={} constant={} group={},{},{} local={},{},{}",
                            d.has_size, d.access_range.known, d.access_range.constant,
                            d.access_range.workgroup[0], d.access_range.workgroup[1],
                            d.access_range.workgroup[2], d.access_range.local[0],
                            d.access_range.local[1], d.access_range.local[2]);
#endif
        desc << '\n';
    }
    Shader::Profile profile{};
    profile.supported_spirv = 0x10400;
    profile.unified_descriptor_binding = true;
    profile.support_descriptor_aliasing = true;
    profile.support_int16 = true;
    profile.support_float_controls = true;
    profile.support_vote = true;
    profile.support_viewport_index_layer_non_geometry = true;
    profile.support_typeless_image_loads = true;
    profile.support_demote_to_helper_invocation = true;
    profile.support_derivative_control = true;
    profile.warp_size_potentially_larger_than_guest = true;
    profile.max_subgroup_size = 128;
    Shader::Backend::Bindings bindings{};
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, {}, program, bindings);
    std::ofstream f(prefix + ".spv", std::ios::binary);
    f.write(reinterpret_cast<const char *>(spv.data()), spv.size() * 4);
    std::cout << fmt::format("SPIRV bytes={} alignment={}\n", spv.size() * 4,
                             host.min_ssbo_alignment);
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
