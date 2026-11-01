// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <iostream>
#include "ir_opt/global_memory_to_storage_buffer_pass.cpp"
using namespace Shader;
struct Fixture {
    ObjectPool<IR::Inst> inst;
    ObjectPool<IR::Block> blocks;
    IR::Program p;
    IR::Block *entry, *head, *body, *latch, *exit;
    IR::Inst *counter, *index, *nextCounter, *nextIndex, *strideInst, *access, *condition;
    IR::Value ptr;
    IR::Value Add(IR::Block *b, IR::Opcode op, std::initializer_list<IR::Value> args = {}) {
        return IR::Value{&*b->PrependNewInst(b->end(), op, args)};
    }
    void Block(IR::Block *b) {
        p.syntax_list.push_back(
            {.data = {.block = b}, .type = IR::AbstractSyntaxNode::Type::Block});
    }
    Fixture(u32 start = 0, u32 step = 1, u32 threshold = 32, bool known = true, bool split = false,
            bool nested = false) {
        entry = blocks.Create(inst);
        head = blocks.Create(inst);
        body = blocks.Create(inst);
        latch = blocks.Create(inst);
        exit = blocks.Create(inst);
        p.blocks = {entry, head, body, latch, exit};
        p.post_order_blocks = {exit, latch, body, head, entry};
        p.stage = Stage::Compute;
        entry->AddBranch(head);
        head->AddBranch(body);
        IR::Block *innerBody{}, *innerLatch{};
        if (nested) {
            innerBody = blocks.Create(inst);
            innerLatch = blocks.Create(inst);
            p.blocks = {entry, head, body, innerBody, innerLatch, latch, exit};
            p.post_order_blocks = {exit, latch, innerLatch, innerBody, body, head, entry};
            body->AddBranch(innerBody);
            innerBody->AddBranch(innerLatch);
            innerLatch->AddBranch(body);
            innerLatch->AddBranch(latch);
        } else {
            body->AddBranch(latch);
        }
        latch->AddBranch(head);
        latch->AddBranch(exit);
        ptr = Add(entry, IR::Opcode::GetCbufU32, {IR::Value{u32{1}}, IR::Value{u32{0x180}}});
        auto hi = Add(entry, IR::Opcode::GetCbufU32, {IR::Value{u32{1}}, IR::Value{u32{0x184}}});
        counter = Add(head, IR::Opcode::Phi).Inst();
        counter->SetFlags(IR::Type::U32);
        index = Add(head, IR::Opcode::Phi).Inst();
        index->SetFlags(IR::Type::U32);
        nextCounter = Add(latch, IR::Opcode::IAdd32, {IR::Value{counter}, IR::Value{step}}).Inst();
        nextIndex = Add(latch, IR::Opcode::IAdd32, {IR::Value{index}, IR::Value{step}}).Inst();
        counter->AddPhiOperand(entry, IR::Value{start});
        counter->AddPhiOperand(latch, IR::Value{nextCounter});
        index->AddPhiOperand(entry, IR::Value{start});
        index->AddPhiOperand(latch, IR::Value{nextIndex});
        auto source = IR::Value{index};
        if (split)
            source = Add(body, IR::Opcode::BitFieldUExtract,
                         {source, IR::Value{u32{0}}, IR::Value{u32{16}}});
        auto stride = Add(body, IR::Opcode::IMul32, {source, IR::Value{u32{144}}});
        strideInst = stride.Inst();
        if (split) {
            auto high = Add(body, IR::Opcode::BitFieldUExtract,
                            {IR::Value{index}, IR::Value{u32{16}}, IR::Value{u32{16}}});
            high = Add(body, IR::Opcode::IMul32, {high, IR::Value{u32{144}}});
            high = Add(body, IR::Opcode::ShiftLeftLogical32, {high, IR::Value{u32{16}}});
            stride = Add(body, IR::Opcode::IAdd32, {stride, high});
        }
        auto offset = Add(body, IR::Opcode::IAdd32, {stride, IR::Value{u32{128}}});
        auto low = Add(body, IR::Opcode::IAdd32, {ptr, offset});
        auto address = Add(body, IR::Opcode::CompositeConstructU32x2, {low, hi});
        access = Add(body, IR::Opcode::LoadGlobal128, {address}).Inst();
        auto limit =
            known ? IR::Value{threshold}
                  : Add(entry, IR::Opcode::GetCbufU32, {IR::Value{u32{2}}, IR::Value{u32{4}}});
        auto cond = Add(latch, IR::Opcode::ULessThan, {IR::Value{nextCounter}, limit});
        condition = cond.Inst();
        Block(entry);
        Block(head);
        p.syntax_list.push_back(
            {.data = {.loop = {body, latch, exit}}, .type = IR::AbstractSyntaxNode::Type::Loop});
        Block(body);
        if (nested) {
            p.syntax_list.push_back({.data = {.loop = {innerBody, innerLatch, latch}},
                                     .type = IR::AbstractSyntaxNode::Type::Loop});
            Block(innerBody);
            Block(innerLatch);
            // Unknown iteration count; backend safety exit must still be allowed.
            p.syntax_list.push_back({.data = {.repeat = {IR::U1{IR::Value{true}}, body, latch}},
                                     .type = IR::AbstractSyntaxNode::Type::Repeat});
        }
        Block(latch);
        p.syntax_list.push_back({.data = {.repeat = {IR::U1{cond}, head, exit}},
                                 .type = IR::AbstractSyntaxNode::Type::Repeat});
        Block(exit);
        p.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    }
    ComputeStorageBufferAccessRange Run() {
        HostTranslateInfo h{};
        h.min_ssbo_alignment = 4;
        Optimization::GlobalMemoryToStorageBufferPass(p, h);
        assert(p.info.storage_buffers_descriptors.size() == 1);
        return p.info.storage_buffers_descriptors[0].access_range;
    }
};

struct Diamond {
    ObjectPool<IR::Inst> inst;
    ObjectPool<IR::Block> pool;
    IR::Program p;
    IR::Value Add(IR::Block *b, IR::Opcode op, std::initializer_list<IR::Value> args = {}) {
        return IR::Value{&*b->PrependNewInst(b->end(), op, args)};
    }
    void Block(IR::Block *b) {
        p.syntax_list.push_back(
            {.data = {.block = b}, .type = IR::AbstractSyntaxNode::Type::Block});
    }
    Diamond(bool correlated, bool unknownArm = true, bool dag = false) {
        auto *e = pool.Create(inst), *t = pool.Create(inst), *j = pool.Create(inst),
             *u = pool.Create(inst), *x = pool.Create(inst);
        p.blocks = {e, t, j, u, x};
        p.post_order_blocks = {x, u, j, t, e};
        p.stage = Stage::Compute;
        e->AddBranch(t);
        e->AddBranch(j);
        t->AddBranch(j);
        j->AddBranch(u);
        j->AddBranch(x);
        u->AddBranch(x);
        auto ptr = Add(e, IR::Opcode::GetCbufU32, {IR::Value{u32{1}}, IR::Value{u32{0x180}}});
        auto hi = Add(e, IR::Opcode::GetCbufU32, {IR::Value{u32{1}}, IR::Value{u32{0x184}}});
        auto v = Add(e, IR::Opcode::GetCbufU32, {IR::Value{u32{2}}, IR::Value{u32{8}}});
        auto pred = Add(e, IR::Opcode::IEqual, {v, IR::Value{u32{0}}});
        if (dag)
            for (int i = 0; i < 55; ++i)
                pred = Add(e, IR::Opcode::LogicalAnd, {pred, pred});
        auto good = Add(e, IR::Opcode::IAdd32, {ptr, IR::Value{u32{16}}});
        auto bad = unknownArm ? v : Add(e, IR::Opcode::IAdd32, {ptr, IR::Value{u32{64}}});
        auto phi = Add(j, IR::Opcode::Phi);
        phi.Inst()->SetFlags(IR::Type::U32);
        phi.Inst()->AddPhiOperand(e, good);
        phi.Inst()->AddPhiOperand(t, bad);
        auto select = correlated ? Add(j, IR::Opcode::LogicalNot, {pred})
                                 : Add(j, IR::Opcode::IEqual, {v, IR::Value{u32{1}}});
        auto address = Add(u, IR::Opcode::CompositeConstructU32x2, {phi, hi});
        Add(u, IR::Opcode::LoadGlobal32, {address});
        Block(e);
        p.syntax_list.push_back(
            {.data = {.if_node = {IR::U1{pred}, t, j}}, .type = IR::AbstractSyntaxNode::Type::If});
        Block(t);
        p.syntax_list.push_back(
            {.data = {.end_if = {j}}, .type = IR::AbstractSyntaxNode::Type::EndIf});
        Block(j);
        p.syntax_list.push_back({.data = {.if_node = {IR::U1{select}, u, x}},
                                 .type = IR::AbstractSyntaxNode::Type::If});
        Block(u);
        p.syntax_list.push_back(
            {.data = {.end_if = {x}}, .type = IR::AbstractSyntaxNode::Type::EndIf});
        Block(x);
        p.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    }
    ComputeStorageBufferAccessRange Run() {
        HostTranslateInfo h{};
        h.min_ssbo_alignment = 4;
        Optimization::GlobalMemoryToStorageBufferPass(p, h);
        assert(p.info.storage_buffers_descriptors.size() == 1);
        return p.info.storage_buffers_descriptors[0].access_range;
    }
};

ComputeStorageBufferAccessRange AffinePointer(u32 offset) {
    ObjectPool<IR::Inst> ip;
    ObjectPool<IR::Block> bp;
    auto *b = bp.Create(ip);
    IR::Program p;
    p.blocks = {b};
    p.post_order_blocks = {b};
    p.stage = Stage::Compute;
    auto add = [&](IR::Opcode op, std::initializer_list<IR::Value> args = {}) {
        return IR::Value{&*b->PrependNewInst(b->end(), op, args)};
    };
    auto ptr = add(IR::Opcode::GetCbufU32, {IR::Value{u32{0}}, IR::Value{offset}});
    auto hi = add(IR::Opcode::GetCbufU32, {IR::Value{u32{0}}, IR::Value{offset + 4}});
    auto g = add(IR::Opcode::WorkgroupId);
    g = add(IR::Opcode::CompositeExtractU32x3, {g, IR::Value{u32{0}}});
    auto l = add(IR::Opcode::LocalInvocationId);
    l = add(IR::Opcode::CompositeExtractU32x3, {l, IR::Value{u32{0}}});
    g = add(IR::Opcode::IMul32, {g, IR::Value{u32{128}}});
    l = add(IR::Opcode::IMul32, {l, IR::Value{u32{4}}});
    auto sum = add(IR::Opcode::IAdd32, {ptr, g});
    sum = add(IR::Opcode::IAdd32, {sum, l});
    auto addr = add(IR::Opcode::CompositeConstructU32x2, {sum, hi});
    add(IR::Opcode::LoadGlobal32, {addr});
    HostTranslateInfo h{};
    h.min_ssbo_alignment = 4;
    Optimization::GlobalMemoryToStorageBufferPass(p, h);
    assert(p.info.storage_buffers_descriptors.size() == 1 &&
           !p.info.storage_buffers_descriptors[0].has_size);
    return p.info.storage_buffers_descriptors[0].access_range;
}
bool Unknown(const ComputeStorageBufferAccessRange &r) { return !r.GetSize({1, 1, 1}, {1, 1, 1}); }
int main() {
    {
        Fixture f(0, 1, 32, true, false, true);
        Optimization::StructuredSsboBounds flow(f.p);
        assert(flow.Valid() && flow.At(f.body).empty() && flow.At(f.latch).empty());
        assert(f.Run().constant == 4608); // Outer induction stays bounded across inner repeats.
    }
    {
        Fixture f(0, 1, 32, true, false, true);
        auto *phi = &*f.body->PrependNewInst(f.body->begin(), IR::Opcode::Phi);
        phi->SetFlags(IR::Type::U32);
        auto *innerLatch = f.p.blocks[4];
        auto next = f.Add(innerLatch, IR::Opcode::IAdd32, {IR::Value{phi}, IR::Value{u32{1}}});
        phi->AddPhiOperand(f.head, IR::Value{u32{0}});
        phi->AddPhiOperand(innerLatch, next);
        f.strideInst->SetArg(0, IR::Value{phi});
        assert(Unknown(f.Run())); // Unknown inner recurrence cannot inherit the outer bound.
    }
    {
        Fixture f(0, 1, 2);
        auto low = f.access->Arg(0).Inst()->Arg(0).Inst();
        low->ReplaceOpcode(IR::Opcode::IMul32);
        low->SetArg(0, f.ptr);
        low->SetArg(1, IR::Value{f.index});
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        auto r = f.Run();
        assert(r.known && r.constant == 4608);
    }
    for (u32 off : {8U, 16U})
        assert(AffinePointer(off).GetSize({1536, 1, 1}, {32, 1, 1}) == 196608);
    {
        Fixture f;
        auto val =
            f.Add(f.body, IR::Opcode::CompositeConstructU32x4,
                  {IR::Value{u32{0}}, IR::Value{u32{0}}, IR::Value{u32{0}}, IR::Value{u32{0}}});
        f.access->ReplaceOpcode(IR::Opcode::WriteGlobal128);
        f.access->SetArg(1, val);
        assert(f.Run().constant == 4608 && f.p.info.storage_buffers_descriptors[0].is_written);
    }
    {
        Fixture f;
        f.access->ReplaceOpcode(IR::Opcode::GlobalAtomicIAdd32);
        f.access->SetArg(1, IR::Value{u32{1}});
        assert(Unknown(f.Run()));
    }
    {
        Fixture f(0, 1, 32, true, true);
        assert(f.Run().constant == 4608);
    }
    {
        Fixture f(0, 1, 32, false);
        assert(Unknown(f.Run()));
    }
    {
        Fixture f(UINT32_MAX, 1, 32);
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        f.nextIndex->SetArg(0, IR::Value{f.counter});
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        f.counter->SetArg(0, f.ptr);
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        f.strideInst->SetArg(1, IR::Value{UINT32_MAX});
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        f.index->AddPhiOperand(f.head, IR::Value{u32{0}});
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        Optimization::StructuredSsboBounds flow(f.p);
        assert(flow.Valid());
        assert(flow.At(f.head).empty());
        assert(flow.At(f.exit).empty());
    }
    {
        Fixture f;
        f.condition->ReplaceOpcode(IR::Opcode::SGreaterThanEqual);
        auto cond = f.Add(f.latch, IR::Opcode::LogicalNot, {IR::Value{f.condition}});
        for (auto &n : f.p.syntax_list)
            if (n.type == IR::AbstractSyntaxNode::Type::Repeat)
                n.data.repeat.cond = IR::U1{cond};
        assert(f.Run().constant == 4608);
    }
    {
        Fixture f(0x80000000, 1, 32);
        f.condition->ReplaceOpcode(IR::Opcode::SLessThan);
        assert(Unknown(f.Run()));
    }
    {
        Fixture f;
        auto cond = f.Add(f.latch, IR::Opcode::IEqual, {IR::Value{f.index}, IR::Value{u32{7}}});
        for (int i = 0; i < 60; ++i)
            cond = f.Add(f.latch, IR::Opcode::LogicalAnd, {cond, cond});
        for (auto &n : f.p.syntax_list)
            if (n.type == IR::AbstractSyntaxNode::Type::Repeat)
                n.data.repeat.cond = IR::U1{cond};
        assert(Unknown(f.Run()));
    }
    {
        Diamond f(true);
        assert(f.Run().constant == 20);
    }
    {
        Diamond f(false);
        assert(Unknown(f.Run()));
    }
    {
        Diamond f(false, false);
        assert(f.Run().constant == 68);
    }
    {
        Diamond f(true, true, true);
        auto r = f.Run();
        assert(!r.known || r.constant >= 20);
    }
    for (u32 start : {0U, 2U, 10U})
        for (u32 step : {1U, 2U, 3U})
            for (u32 limit : {4U, 10U, 32U}) {
                Fixture f(start, step, limit);
                auto range = f.Run();
                u64 i = start, maximum{};
                do {
                    maximum = std::max(maximum, i * 144 + 128 + 16);
                    i += step;
                } while (i < limit);
                assert(range.known && range.constant >= maximum);
            }
    for (auto [opcode, width] :
         std::initializer_list<std::pair<IR::Opcode, u32>>{{IR::Opcode::LoadGlobalU8, 1},
                                                           {IR::Opcode::LoadGlobalU16, 2},
                                                           {IR::Opcode::LoadGlobal32, 4},
                                                           {IR::Opcode::LoadGlobal64, 8},
                                                           {IR::Opcode::LoadGlobal128, 16}}) {
        Fixture f;
        f.access->ReplaceOpcode(opcode);
        assert(f.Run().constant == 31 * 144 + 128 + width);
    }
    std::cout << "PASS structured ranges: induction/lockstep, exhaustive loop maxima, "
                 "unknowns/wrap/overflow, vector widths, correlated and unrelated Phi, union, "
                 "Repeat safety facts, malformed graph, bounded boolean DAG\n";
}
