// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Shader::IR {
struct Program;
}

namespace Shader::Maxwell {

// Supply primitive attributes through ordinary varyings before either stage is emitted.
void LinkGeometryPassthrough(IR::Program& source, IR::Program& geometry);

} // namespace Shader::Maxwell
