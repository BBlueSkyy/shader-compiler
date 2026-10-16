// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <shader_compiler/exception.h>
#include <shader_compiler/frontend/ir/program.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/ir_opt/passes.h>
#include <shader_compiler/shader_info.h>

namespace Shader::Optimization {

static IR::Attribute EmulatedPrimitiveAttribute(const VaryingState& stores) {
    for (u32 i = 0; i < IR::NUM_GENERICS; i++) {
        if (!stores.Generic(i)) {
            return IR::Attribute::Generic0X + (i * 4);
        }
    }
    throw RuntimeError("No free varying for primitive output emulation");
}

static bool PermittedProgramStage(Stage stage) {
    switch (stage) {
    case Stage::VertexA:
    case Stage::VertexB:
    case Stage::TessellationControl:
    case Stage::TessellationEval:
        return true;
    default:
        return false;
    }
}

void LayerPass(IR::Program& program, const HostTranslateInfo& host_info) {
    if (host_info.support_viewport_index_layer || !PermittedProgramStage(program.stage)) {
        return;
    }

    Info& info = program.info;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            const auto builtin = inst.Arg(0).Attribute();
            bool* required;
            IR::Attribute* target;
            switch (builtin) {
            case IR::Attribute::Layer:
                required = &info.requires_layer_emulation;
                target = &info.emulated_layer;
                break;
            case IR::Attribute::ViewportIndex:
                required = &info.requires_viewport_index_emulation;
                target = &info.emulated_viewport_index;
                break;
            default:
                continue;
            }
            if (!*required) {
                *target = EmulatedPrimitiveAttribute(info.stores);
                info.stores.Set(*target);
                *required = true;
            }
            info.stores.Set(builtin, false);
            inst.SetArg(0, IR::Value{*target});
        }
    }
}

} // namespace Shader::Optimization
