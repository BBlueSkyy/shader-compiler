// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>

#include <shader_compiler/exception.h>
#include <shader_compiler/frontend/ir/program.h>
#include <shader_compiler/frontend/maxwell/geometry_passthrough_link.h>

namespace Shader::Maxwell {
namespace {

bool GeometryUsesGeneric(const IR::Program& geometry, u32 index) {
    return geometry.info.loads.Generic(index) || geometry.info.stores.Generic(index) ||
           geometry.info.passthrough.Generic(index);
}

IR::Attribute FindBridgeAttribute(const IR::Program& source, const IR::Program& geometry,
                                  IR::Attribute source_attribute) {
    if (IR::IsGeneric(source_attribute)) {
        const auto index = IR::GenericAttributeIndex(source_attribute);
        if (!GeometryUsesGeneric(geometry, index)) {
            return source_attribute;
        }
    }
    for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
        if (!source.info.stores.Generic(index) && !GeometryUsesGeneric(geometry, index)) {
            return IR::Attribute::Generic0X + index * 4;
        }
    }
    throw RuntimeError("No free varying for geometry passthrough primitive attribute");
}

void LinkPrimitiveAttribute(IR::Program& source, IR::Program& geometry,
                            IR::Attribute attribute) {
    if (!geometry.info.passthrough[attribute]) {
        return;
    }
    const bool emulated_layer = attribute == IR::Attribute::Layer &&
                                source.info.requires_layer_emulation;
    const bool emulated_viewport = attribute == IR::Attribute::ViewportIndex &&
                                   source.info.requires_viewport_index_emulation;
    const auto source_attribute = emulated_layer ? source.info.emulated_layer
                                : emulated_viewport ? source.info.emulated_viewport_index
                                                    : attribute;
    bool has_store{};
    for (IR::Block* const block : source.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == source_attribute) {
                has_store = true;
            }
        }
    }
    // Header-only stores can describe dynamically indexed writes. Redirecting those
    // needs an offset remap; treating their value as absent would silently lose it.
    if (!has_store && source.info.stores_indexed_attributes &&
        source.info.stores[source_attribute]) {
        throw NotImplementedException("Indexed geometry passthrough primitive attribute");
    }
    const std::optional<IR::Attribute> bridge =
        has_store ? std::optional{FindBridgeAttribute(source, geometry, source_attribute)}
                  : std::nullopt;
    if (bridge) {
        for (IR::Block* const block : source.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                    inst.Arg(0).Attribute() == source_attribute) {
                    inst.SetArg(0, IR::Value{*bridge});
                }
            }
        }
        source.info.stores.Set(source_attribute, false);
        source.info.stores.Set(*bridge);
        source.info.stores.Set(attribute, false);
        geometry.info.loads.Set(*bridge);
        if (emulated_layer) {
            source.info.emulated_layer = *bridge;
        } else if (emulated_viewport) {
            source.info.emulated_viewport_index = *bridge;
        }
    }
    for (IR::Block* const block : geometry.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::GetAttribute ||
                inst.Arg(0).Attribute() != attribute) {
                continue;
            }
            if (bridge) {
                inst.SetArg(0, IR::Value{*bridge});
            } else {
                inst.ReplaceUsesWith(IR::Value{0.0f});
            }
        }
    }
    geometry.info.loads.Set(attribute, false);
    geometry.info.passthrough.Set(attribute, false);
}

} // namespace

void LinkGeometryPassthrough(IR::Program& source, IR::Program& geometry) {
    if (geometry.stage != Stage::Geometry || !geometry.was_geometry_passthrough ||
        geometry.is_geometry_passthrough) {
        return;
    }
    LinkPrimitiveAttribute(source, geometry, IR::Attribute::Layer);
    LinkPrimitiveAttribute(source, geometry, IR::Attribute::ViewportIndex);
}

} // namespace Shader::Maxwell
