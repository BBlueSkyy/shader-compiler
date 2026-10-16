// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>
#include <shader_compiler/exception.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include <shader_compiler/frontend/ir/program.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/ir_opt/passes.h>

int TestLayerPass() {
    using namespace Shader;
    int failures{};
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    const auto run = [&](Stage stage, bool supported, bool viewport, bool full) {
        ObjectPool<IR::Inst> instructions;
        IR::Block block{instructions};
        IR::Program program;
        program.stage = stage;
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        if (full) {
            for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
                program.info.stores.Set(IR::Attribute::Generic0X + index * 4);
            }
        } else {
            program.info.stores.Set(IR::Attribute::Generic0X);
        }
        IR::IREmitter ir{block};
        ir.SetAttribute(IR::Attribute::Layer, ir.Imm32(1.0f), ir.Imm32(0));
        IR::Inst& layer_store = block.back();
        program.info.stores.Set(IR::Attribute::Layer);
        IR::Inst* viewport_store{};
        if (viewport) {
            ir.SetAttribute(IR::Attribute::ViewportIndex, ir.Imm32(2.0f), ir.Imm32(0));
            viewport_store = &block.back();
            program.info.stores.Set(IR::Attribute::ViewportIndex);
        }
        HostTranslateInfo host{};
        host.support_viewport_index_layer = supported;
        bool rejected{};
        try {
            Optimization::LayerPass(program, host);
        } catch (const Shader::Exception&) {
            rejected = true;
        }
        if (supported || stage == Stage::Geometry) {
            check(!rejected && layer_store.Arg(0).Attribute() == IR::Attribute::Layer &&
                      !program.info.requires_layer_emulation &&
                      !program.info.requires_viewport_index_emulation,
                  "supported stages retain primitive builtin stores");
            return;
        }
        if (full) {
            check(rejected, "primitive output emulation rejects exhausted varyings");
            return;
        }
        check(!rejected && program.info.requires_layer_emulation &&
                  layer_store.Arg(0).Attribute() == IR::Attribute::Generic1X &&
                  program.info.emulated_layer == IR::Attribute::Generic1X &&
                  !program.info.stores[IR::Attribute::Layer] &&
                  program.info.stores[IR::Attribute::Generic1X],
              "layer emulation reserves an unused varying");
        if (viewport_store) {
            check(program.info.requires_viewport_index_emulation &&
                      viewport_store->Arg(0).Attribute() == IR::Attribute::Generic2X &&
                      program.info.emulated_viewport_index == IR::Attribute::Generic2X &&
                      !program.info.stores[IR::Attribute::ViewportIndex] &&
                      program.info.stores[IR::Attribute::Generic2X],
                  "viewport emulation reserves a distinct unused varying");
        }
    };
    run(Stage::VertexB, false, true, false);
    run(Stage::TessellationEval, false, true, false);
    run(Stage::VertexB, false, false, true);
    run(Stage::VertexB, true, true, true);
    run(Stage::Geometry, false, true, true);
    {
        ObjectPool<IR::Inst> instructions;
        IR::Block block{instructions};
        IR::Program program;
        program.stage = Stage::VertexB;
        program.post_order_blocks.push_back(&block);
        program.info.stores.mask.set();
        bool rejected{};
        try {
            Optimization::LayerPass(program, HostTranslateInfo{});
        } catch (const Shader::Exception&) {
            rejected = true;
        }
        check(!rejected && !program.info.requires_layer_emulation &&
                  !program.info.requires_viewport_index_emulation,
              "no primitive writes require no emulation varying");
    }
    return failures;
}
