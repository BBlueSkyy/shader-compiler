// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <deque>
#include <memory>
#include <iostream>
#include "ir_opt/global_memory_to_storage_buffer_pass.cpp"
#include "baseline.inc"
using namespace Shader;
using namespace Shader::Optimization;
using IR::Opcode;
int main() {
    std::deque<IR::Inst> nodes;
    auto node = [&](Opcode op, std::initializer_list<IR::Value> args) -> IR::Value {
        auto &inst{nodes.emplace_back(op, 0)};
        size_t i{}; for (auto arg : args) inst.SetArg(i++,arg);
        return IR::Value{&inst};
    };
    const IR::Value zero{u32{0}}, offset{u32{0x280}};
    auto ptr{node(Opcode::GetCbufU32,{zero,offset})};
    auto group{node(Opcode::WorkgroupId,{})};
    auto axis{node(Opcode::CompositeExtractU32x3,{group,zero})};
    auto scale{node(Opcode::IMul32,{axis,IR::Value{u32{64}}})};
    auto address{node(Opcode::IAdd32,{ptr,scale})};
    auto high{node(Opcode::GetCbufU32,{zero,IR::Value{u32{0x284}}})};
    auto masked{node(Opcode::BitwiseAnd32,{axis,IR::Value{u32{255}}})};
    auto nonAffine{node(Opcode::IMul32,{axis,axis})};
    std::vector<IR::Value> examples{ptr,address,masked,nonAffine,IR::Value{u32{256}}};
    // Include shared subtrees and a long expression that exhausts the budget.
    auto deep{address}; for(unsigned i{};i<80;++i) deep=node(Opcode::IAdd32,{deep,IR::Value{u32{1}}});
    examples.push_back(deep);
    for(auto expr:examples) {
        u32 oldBudget{512},newBudget{512};std::string failure;
        auto before{BaselineTrackOffsetRange(expr,{0,0x280},oldBudget)};
        auto after{TrackOffsetRange(expr,{0,0x280},newBudget,&failure)};
        assert(bool(before)==bool(after) && oldBudget==newBudget);
        if (before) assert(before->constant==after->constant && before->workgroup==after->workgroup && before->local==after->local && before->pointer_coefficient==after->pointer_coefficient);
        auto vector{node(Opcode::CompositeConstructU32x2,{expr,high})};
        auto access{node(Opcode::LoadGlobal32,{vector})};
        StorageInfo info;info.to_replace.push_back({{0,0x280},access.Inst(),nullptr});
        std::string diagnostic;
        auto original{BaselineCollectAccessRange({0,0x280},info)};
        auto observed{CollectAccessRange({0,0x280},info,&diagnostic)};
        assert(original.known==observed.known && original.constant==observed.constant && original.workgroup==observed.workgroup && original.local==observed.local);
        if(expr==masked) assert(diagnostic.find("unsupported_opcode:BitwiseAnd32")!=std::string::npos);
        if(expr==nonAffine) assert(diagnostic.find("non_affine_product")!=std::string::npos);
        if(expr==deep) assert(diagnostic.find("traversal_budget")!=std::string::npos);
    }
    auto &phi{nodes.emplace_back(Opcode::Phi,static_cast<u32>(IR::Type::U32))};
    phi.AddPhiOperand(nullptr,IR::Value{&phi});
    auto cycle{node(Opcode::CompositeConstructU32x2,{IR::Value{&phi},high})};
    auto access{node(Opcode::LoadGlobal32,{cycle})};
    auto diagnostic{DiagnosticAddress(access.Inst())};
    assert(diagnostic.find("Phi(n")!=std::string::npos && diagnostic.size()<4096);
    // Node cap is explicit, without revisiting/cycling forever.
    auto large{ptr};for(unsigned i{};i<256;++i)large=node(Opcode::IAdd32,{large,IR::Value{u32{1}}});
    auto largeVec{node(Opcode::CompositeConstructU32x2,{large,high})};
    auto largeAccess{node(Opcode::LoadGlobal32,{largeVec})};
    assert(DiagnosticAddress(largeAccess.Inst()).find("dag_truncated=true")!=std::string::npos);
    std::cout << "PASS real IR: unchanged inferred ranges/fallbacks, reasons, shared DAG, Phi cycle, depth/node caps\n";
}
