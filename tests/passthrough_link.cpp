// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>
#include <shader_compiler/exception.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include <shader_compiler/frontend/ir/program.h>
#include <shader_compiler/frontend/maxwell/geometry_passthrough_link.h>

namespace {
using namespace Shader;

struct Fixture {
    ObjectPool<IR::Inst> instructions;
    IR::Block source_block{instructions};
    IR::Block geometry_block{instructions};
    IR::Program source;
    IR::Program geometry;

    Fixture() {
        source.stage = Stage::VertexB;
        source.blocks.push_back(&source_block);
        geometry.stage = Stage::Geometry;
        geometry.was_geometry_passthrough = true;
        geometry.blocks.push_back(&geometry_block);
    }

    IR::Inst& Store(IR::Attribute attr) {
        IR::IREmitter ir{source_block};
        ir.SetAttribute(attr, ir.Imm32(2.0f), ir.Imm32(0));
        source.info.stores.Set(attr);
        return source_block.back();
    }

    IR::Inst& Load(IR::Attribute attr) {
        IR::IREmitter ir{geometry_block};
        const auto value = ir.GetAttribute(attr, ir.Imm32(0));
        IR::Inst& load = *value.Inst();
        ir.SetAttribute(attr, value, ir.Imm32(0));
        geometry.info.loads.Set(attr);
        geometry.info.stores.Set(attr);
        geometry.info.passthrough.Set(attr);
        return load;
    }
};
} // namespace

int TestPassthroughLink() {
    using namespace Shader;
    int failures{};
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    {
        Fixture f;
        auto& layer_store = f.Store(IR::Attribute::Layer);
        auto& viewport_store = f.Store(IR::Attribute::ViewportIndex);
        auto& layer_load = f.Load(IR::Attribute::Layer);
        auto& viewport_load = f.Load(IR::Attribute::ViewportIndex);
        f.source.info.stores.Set(IR::Attribute::Generic0X);
        f.geometry.info.loads.Set(IR::Attribute::Generic1X);
        f.geometry.info.stores.Set(IR::Attribute::Generic2X);
        f.geometry.info.passthrough.Set(IR::Attribute::Generic3X);
        Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        check(layer_store.Arg(0).Attribute() == IR::Attribute::Generic4X,
              "layer bridge avoids both stages' occupied generic slots");
        check(viewport_store.Arg(0).Attribute() == IR::Attribute::Generic5X,
              "viewport bridge has a separate generic slot");
        check(layer_load.Arg(0).Attribute() == layer_store.Arg(0).Attribute(),
              "layer load matches preceding output");
        check(viewport_load.Arg(0).Attribute() == viewport_store.Arg(0).Attribute(),
              "viewport load matches preceding output");
        check(!f.source.info.stores[IR::Attribute::Layer] &&
                  !f.source.info.stores[IR::Attribute::ViewportIndex],
              "redirected source builtin outputs are removed");
        check(!f.geometry.info.loads[IR::Attribute::Layer] &&
                  !f.geometry.info.loads[IR::Attribute::ViewportIndex] &&
                  !f.geometry.info.passthrough[IR::Attribute::Layer] &&
                  !f.geometry.info.passthrough[IR::Attribute::ViewportIndex],
              "geometry interface has no illegal primitive builtin inputs");
        check(f.source.info.stores[IR::Attribute::Generic4X] &&
                  f.geometry.info.loads[IR::Attribute::Generic4X] &&
                  !f.geometry.info.stores[IR::Attribute::Generic4X] &&
                  !f.geometry.info.passthrough[IR::Attribute::Generic4X],
              "bridge is an input varying, not a new geometry output");
    }
    {
        Fixture f;
        f.Store(IR::Attribute::Generic2X);
        f.source.info.requires_layer_emulation = true;
        f.source.info.emulated_layer = IR::Attribute::Generic2X;
        auto& load = f.Load(IR::Attribute::Layer);
        Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        check(load.Arg(0).Attribute() == IR::Attribute::Generic2X,
              "reuse the source's existing layer emulation varying");
    }
    {
        Fixture f;
        auto& store = f.Store(IR::Attribute::Generic2X);
        f.source.info.requires_layer_emulation = true;
        f.source.info.emulated_layer = IR::Attribute::Generic2X;
        f.geometry.info.stores.Set(IR::Attribute::Generic2X);
        auto& load = f.Load(IR::Attribute::Layer);
        Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        check(store.Arg(0).Attribute() == IR::Attribute::Generic0X &&
                  load.Arg(0).Attribute() == IR::Attribute::Generic0X,
              "colliding layer emulation varying is moved in both stages");
    }
    {
        Fixture f;
        f.Store(IR::Attribute::Generic2X);
        f.source.info.requires_viewport_index_emulation = true;
        f.source.info.emulated_viewport_index = IR::Attribute::Generic2X;
        auto& load = f.Load(IR::Attribute::ViewportIndex);
        Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        check(load.GetOpcode() == IR::Opcode::GetAttribute &&
                  load.Arg(0).Attribute() == IR::Attribute::Generic2X,
              "reuse the source's existing viewport emulation varying");
    }
    {
        Fixture f;
        auto& load = f.Load(IR::Attribute::Layer);
        Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        const auto value = IR::Value{&load}.Resolve();
        check(value.IsImmediate() && value.F32() == 0.0f,
              "absent preceding layer output supplies the default zero");
        check(!f.geometry.info.loads[IR::Attribute::Layer] &&
                  !f.geometry.info.passthrough[IR::Attribute::Layer],
              "defaulted builtin is removed from geometry input interface");
    }
    {
        Fixture f;
        f.Store(IR::Attribute::Layer);
        auto& load = f.Load(IR::Attribute::Layer);
        f.geometry.is_geometry_passthrough = true;
        Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        check(load.Arg(0).Attribute() == IR::Attribute::Layer,
              "native passthrough programs are left unchanged");
    }
    {
        Fixture f;
        f.Store(IR::Attribute::Layer);
        f.Load(IR::Attribute::Layer);
        for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
            f.geometry.info.loads.Set(IR::Attribute::Generic0X + index * 4);
        }
        bool rejected{};
        try {
            Maxwell::LinkGeometryPassthrough(f.source, f.geometry);
        } catch (const Shader::Exception&) {
            rejected = true;
        }
        check(rejected, "exhausted shared varying slots are rejected");
    }
    return failures;
}
