// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bitset>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <shader_compiler/frontend/ir/program.h>

namespace Shader::Optimization {

// Compile-time proof only. An unsupported graph or exhausted proof stays unknown.
class StructuredSsboBounds {
  public:
    using Facts = std::map<const IR::Inst *, bool>;
    static constexpr size_t MaxBlocks = 512;
    static constexpr size_t MaxFacts = 64;
    static constexpr size_t MaxSweeps = 64;

    explicit StructuredSsboBounds(const IR::Program &program) {
        if (BuildGraph(program) && Dominators() && SolveFacts()) {
            FindInductions();
            valid = true;
        }
    }
    bool Valid() const { return valid; }
    std::optional<u32> InductionBound(const IR::Inst *phi) const {
        const auto it = inductions.find(phi);
        return valid && it != inductions.end() ? std::optional<u32>{it->second} : std::nullopt;
    }
    Facts At(const IR::Block *block) const {
        const auto it = nodes.find(block);
        return valid && it != nodes.end() && facts[it->second] ? *facts[it->second] : Facts{};
    }
    // Reject cyclic Phis unless the recurrence was proved separately. Facts
    // from a use in one iteration cannot select inputs in a previous iteration.
    bool HasBackedge(const IR::Inst *phi) const {
        const auto it = owners.find(phi);
        if (it == owners.end())
            return true;
        for (const auto &edge : edges)
            if (edge.to == it->second && edge.backedge)
                return true;
        return false;
    }
    std::optional<Facts> Incoming(const IR::Inst *phi, const IR::Block *predecessor,
                                  const Facts &use) const {
        const auto owner = owners.find(phi);
        const auto pred = nodes.find(predecessor);
        if (!valid || owner == owners.end() || pred == nodes.end())
            return std::nullopt;
        for (const auto &edge : edges) {
            if (edge.from != pred->second || edge.to != owner->second)
                continue;
            auto path = EdgeFacts(edge);
            if (!path)
                return std::nullopt;
            Facts combined = use;
            for (const auto &[condition, truth] : *path)
                if (!AddFact(combined, condition, truth))
                    return std::nullopt;
            return combined;
        }
        return std::nullopt;
    }

  private:
    struct Edge {
        size_t from, to;
        std::optional<IR::Value> condition;
        bool truth{}, reset{}, backedge{};
    };
    struct Loop {
        size_t header, latch;
        IR::Value condition;
    };
    struct Recurrence {
        const IR::Inst *phi;
        size_t entry, latch;
        u32 start, step;
        IR::Value next;
    };
    std::map<const IR::Block *, size_t> nodes;
    std::map<const IR::Inst *, size_t> owners;
    std::vector<const IR::Block *> blocks;
    std::vector<Edge> edges;
    std::vector<Loop> loops;
    std::set<size_t> headers;
    std::vector<std::optional<Facts>> facts;
    std::vector<std::bitset<MaxBlocks>> dom;
    std::map<const IR::Inst *, u32> inductions;
    bool valid{};

    static IR::Value Unwrap(IR::Value value) {
        for (unsigned n{}; n < 64 && !value.IsImmediate(); ++n) {
            const auto *inst = value.InstRecursive();
            if (inst->GetOpcode() != IR::Opcode::ConditionRef)
                return IR::Value{const_cast<IR::Inst *>(inst)};
            value = inst->Arg(0);
        }
        return value;
    }
    static bool AddFact(Facts &into, const IR::Inst *condition, bool truth) {
        const auto it = into.find(condition);
        if (it != into.end())
            return it->second == truth;
        if (into.size() < MaxFacts)
            into.emplace(condition, truth);
        return true; // Dropping a fact weakens the proof.
    }
    static bool Assume(Facts &into, IR::Value value, bool truth, unsigned depth, u32 &budget) {
        if (depth > 64 || !budget)
            return true;
        --budget;
        value = Unwrap(value);
        if (value.IsImmediate())
            return value.Type() != IR::Type::U1 || value.U1() == truth;
        const auto *inst = value.InstRecursive();
        const auto op = inst->GetOpcode();
        if (op == IR::Opcode::LogicalNot)
            return Assume(into, inst->Arg(0), !truth, depth + 1, budget);
        if (!AddFact(into, inst, truth))
            return false;
        if ((op == IR::Opcode::LogicalAnd && truth) || (op == IR::Opcode::LogicalOr && !truth))
            return Assume(into, inst->Arg(0), truth, depth + 1, budget) &&
                   Assume(into, inst->Arg(1), truth, depth + 1, budget);
        return true;
    }
    bool AddEdge(size_t from, const IR::Block *to, std::optional<IR::Value> condition = {},
                 bool truth = false, bool reset = false, bool backedge = false) {
        const auto it = nodes.find(to);
        if (it == nodes.end())
            return false;
        for (const auto &edge : edges)
            if (edge.from == from && edge.to == it->second)
                return false;
        edges.push_back({from, it->second, condition, truth, reset, backedge});
        return true;
    }
    bool BuildGraph(const IR::Program &program) {
        if (program.blocks.empty() || program.blocks.size() > MaxBlocks ||
            program.syntax_list.empty() || program.syntax_list.size() > 4 * MaxBlocks ||
            program.syntax_list.front().type != IR::AbstractSyntaxNode::Type::Block ||
            program.syntax_list.front().data.block != program.blocks.front())
            return false;
        size_t instructions{}, arguments{};
        const auto boundedIdentity = [](IR::Value value) {
            for (unsigned n{}; n < 64; ++n) {
                if (!value.IsIdentity())
                    return true;
                value = value.Inst()->Arg(0);
            }
            return false;
        };
        for (const auto *block : program.blocks) {
            if (!nodes.emplace(block, blocks.size()).second)
                return false;
            blocks.push_back(block);
            for (const auto &inst : block->Instructions()) {
                if (++instructions > 32768)
                    return false;
                for (size_t i{}; i < inst.NumArgs(); ++i)
                    if (++arguments > 65536 || !boundedIdentity(inst.Arg(i)))
                        return false;
                owners.emplace(&inst, blocks.size() - 1);
            }
        }
        std::optional<size_t> current;
        // Match backend Traverse, including its explicit fall-through branches.
        for (const auto &node : program.syntax_list) {
            using T = IR::AbstractSyntaxNode::Type;
            if (node.type == T::Block) {
                const auto it = nodes.find(node.data.block);
                if (it == nodes.end() || (current && !AddEdge(*current, node.data.block)))
                    return false;
                current = it->second;
                continue;
            }
            if (!current)
                return false;
            const size_t from = *current;
            switch (node.type) {
            case T::If:
                if (!boundedIdentity(node.data.if_node.cond))
                    return false;
                if (!AddEdge(from, node.data.if_node.body, node.data.if_node.cond, true) ||
                    !AddEdge(from, node.data.if_node.merge, node.data.if_node.cond, false))
                    return false;
                break;
            case T::EndIf:
                if (!AddEdge(from, node.data.end_if.merge))
                    return false;
                break;
            case T::Loop:
                headers.insert(from);
                if (!AddEdge(from, node.data.loop.body))
                    return false;
                break;
            case T::Repeat: {
                if (!boundedIdentity(node.data.repeat.cond))
                    return false;
                const auto it = nodes.find(node.data.repeat.loop_header);
                if (it == nodes.end())
                    return false;
                loops.push_back({it->second, from, node.data.repeat.cond});
                // The backend adds a persistent safety counter. Either exit
                // can discard facts; exiting does NOT prove !original_cond.
                if (!AddEdge(from, node.data.repeat.loop_header, {}, false, true, true) ||
                    !AddEdge(from, node.data.repeat.merge, {}, false, true))
                    return false;
                break;
            }
            case T::Break:
                if (!boundedIdentity(node.data.break_node.cond))
                    return false;
                if (!AddEdge(from, node.data.break_node.merge, node.data.break_node.cond, true) ||
                    !AddEdge(from, node.data.break_node.skip, node.data.break_node.cond, false))
                    return false;
                break;
            case T::Return:
            case T::Unreachable:
                break;
            default:
                return false;
            }
            current.reset();
        }
        for (size_t i{}; i < blocks.size(); ++i) {
            std::set<const IR::Block *> succ, pred;
            for (const auto &e : edges) {
                if (e.from == i)
                    succ.insert(blocks[e.to]);
                if (e.to == i)
                    pred.insert(blocks[e.from]);
            }
            if (succ != std::set<const IR::Block *>(blocks[i]->ImmSuccessors().begin(),
                                                    blocks[i]->ImmSuccessors().end()) ||
                pred != std::set<const IR::Block *>(blocks[i]->ImmPredecessors().begin(),
                                                    blocks[i]->ImmPredecessors().end()))
                return false;
            for (const auto &inst : blocks[i]->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::Phi)
                    continue;
                std::set<const IR::Block *> inputs;
                for (size_t arg{}; arg < inst.NumArgs(); ++arg)
                    inputs.insert(inst.PhiBlock(arg));
                if (inputs != pred || inputs.size() != inst.NumArgs())
                    return false;
            }
        }
        if (!blocks.front()->ImmPredecessors().empty())
            return false;
        std::set<size_t> reached{0};
        std::vector<size_t> pending{0};
        while (!pending.empty()) {
            const auto from = pending.back();
            pending.pop_back();
            for (const auto &edge : edges)
                if (edge.from == from && reached.insert(edge.to).second)
                    pending.push_back(edge.to);
        }
        if (reached.size() != blocks.size())
            return false;
        // Every cycle must cross a Repeat edge, whose facts are discarded.
        std::vector<size_t> degrees(blocks.size());
        pending.clear();
        for (const auto &edge : edges)
            if (!edge.backedge)
                ++degrees[edge.to];
        for (size_t i{}; i < blocks.size(); ++i)
            if (!degrees[i])
                pending.push_back(i);
        size_t removed{};
        while (!pending.empty()) {
            const auto from = pending.back();
            pending.pop_back();
            ++removed;
            for (const auto &edge : edges)
                if (!edge.backedge && edge.from == from && --degrees[edge.to] == 0)
                    pending.push_back(edge.to);
        }
        if (removed != blocks.size())
            return false;
        return true;
    }
    bool Dominators() {
        std::bitset<MaxBlocks> all;
        for (size_t i{}; i < blocks.size(); ++i)
            all.set(i);
        dom.assign(blocks.size(), all);
        dom[0].reset();
        dom[0].set(0);
        for (size_t sweep{}; sweep < MaxSweeps; ++sweep) {
            bool changed{};
            for (size_t i = 1; i < blocks.size(); ++i) {
                auto next = all;
                bool found{};
                for (const auto &e : edges)
                    if (e.to == i) {
                        next &= dom[e.from];
                        found = true;
                    }
                if (!found)
                    return false;
                next.set(i);
                if (next != dom[i]) {
                    dom[i] = next;
                    changed = true;
                }
            }
            if (!changed)
                return true;
        }
        return false;
    }
    std::optional<Facts> EdgeFacts(const Edge &edge) const {
        if (!facts[edge.from])
            return std::nullopt;
        Facts result = edge.reset ? Facts{} : *facts[edge.from];
        u32 budget{128};
        if (edge.condition && !Assume(result, *edge.condition, edge.truth, 0, budget))
            return std::nullopt;
        return result;
    }
    bool SolveFacts() {
        facts.resize(blocks.size());
        facts[0] = Facts{};
        for (size_t sweep{}; sweep < MaxSweeps; ++sweep) {
            bool changed{};
            for (size_t i = 1; i < blocks.size(); ++i) {
                std::optional<Facts> next;
                for (const auto &e : edges)
                    if (e.to == i) {
                        auto input = EdgeFacts(e);
                        if (!input)
                            continue;
                        if (!next) {
                            next = std::move(input);
                            continue;
                        }
                        for (auto it = next->begin(); it != next->end();) {
                            const auto match = input->find(it->first);
                            if (match == input->end() || match->second != it->second)
                                it = next->erase(it);
                            else
                                ++it;
                        }
                    }
                if (next != facts[i]) {
                    facts[i] = std::move(next);
                    changed = true;
                }
            }
            if (!changed)
                return true;
        }
        return false;
    }
    std::optional<Recurrence> RecurrenceOf(const IR::Inst &phi, const Loop &loop) const {
        if (phi.GetOpcode() != IR::Opcode::Phi || phi.Type() != IR::Type::U32 || phi.NumArgs() != 2)
            return {};
        size_t back = 2;
        for (size_t i{}; i < 2; ++i)
            if (phi.PhiBlock(i) == blocks[loop.latch])
                back = i;
        if (back == 2)
            return {};
        const auto initial = Unwrap(phi.Arg(1 - back)), next = Unwrap(phi.Arg(back));
        if (!initial.IsImmediate() || initial.Type() != IR::Type::U32 || next.IsImmediate())
            return {};
        const auto entry = nodes.find(phi.PhiBlock(1 - back));
        if (entry == nodes.end())
            return {};
        const auto *add = next.InstRecursive();
        if (add->GetOpcode() != IR::Opcode::IAdd32)
            return {};
        for (size_t i{}; i < 2; ++i) {
            const auto self = Unwrap(add->Arg(i)), step = Unwrap(add->Arg(1 - i));
            if (self.IsImmediate() || self.InstRecursive() != &phi || !step.IsImmediate() ||
                step.Type() != IR::Type::U32 || !step.U32())
                continue;
            return Recurrence{&phi, entry->second, loop.latch, initial.U32(), step.U32(), next};
        }
        return {};
    }
    // Extract only a condition necessary to take the Repeat edge.
    static std::optional<std::pair<u32, bool>> UpperLimit(IR::Value value, IR::Value next,
                                                          bool truth, unsigned depth, u32 &budget) {
        if (depth > 64 || !budget)
            return {};
        --budget;
        value = Unwrap(value);
        if (value.IsImmediate())
            return {};
        const auto *inst = value.InstRecursive();
        const auto op = inst->GetOpcode();
        if (op == IR::Opcode::LogicalNot)
            return UpperLimit(inst->Arg(0), next, !truth, depth + 1, budget);
        if ((op == IR::Opcode::LogicalAnd && truth) || (op == IR::Opcode::LogicalOr && !truth)) {
            if (auto l = UpperLimit(inst->Arg(0), next, truth, depth + 1, budget))
                return l;
            return UpperLimit(inst->Arg(1), next, truth, depth + 1, budget);
        }
        const bool signedCmp = op == IR::Opcode::SLessThan || op == IR::Opcode::SGreaterThanEqual;
        if (!((truth && (op == IR::Opcode::SLessThan || op == IR::Opcode::ULessThan)) ||
              (!truth &&
               (op == IR::Opcode::SGreaterThanEqual || op == IR::Opcode::UGreaterThanEqual))))
            return {};
        const auto lhs = Unwrap(inst->Arg(0)), rhs = Unwrap(inst->Arg(1));
        if (lhs != next || !rhs.IsImmediate() || rhs.Type() != IR::Type::U32 || !rhs.U32() ||
            (signedCmp && rhs.U32() > u32{INT32_MAX}))
            return {};
        return std::pair{rhs.U32() - 1, signedCmp};
    }
    void FindInductions() {
        for (const auto &loop : loops) {
            if (!headers.contains(loop.header) || !dom[loop.latch].test(loop.header))
                continue;
            std::set<size_t> predecessors;
            size_t backedges{};
            for (const auto &e : edges)
                if (e.to == loop.header) {
                    predecessors.insert(e.from);
                    backedges += e.backedge;
                }
            if (predecessors.size() != 2 || backedges != 1)
                continue;
            std::set<size_t> natural{loop.header, loop.latch};
            std::vector<size_t> pending{loop.latch};
            while (!pending.empty()) {
                auto node = pending.back();
                pending.pop_back();
                for (const auto &e : edges)
                    if (e.to == node && natural.insert(e.from).second && e.from != loop.header)
                        pending.push_back(e.from);
            }
            std::vector<Recurrence> recurrences;
            for (const auto &inst : blocks[loop.header]->Instructions()) {
                auto rec = RecurrenceOf(inst, loop);
                if (rec && !natural.contains(rec->entry))
                    recurrences.push_back(*rec);
            }
            for (const auto &rec : recurrences) {
                u32 budget{128};
                const auto limit = UpperLimit(loop.condition, rec.next, true, 0, budget);
                if (!limit)
                    continue;
                const u64 maximum = std::max(rec.start, limit->first);
                const u64 typeMax = limit->second ? u64{INT32_MAX} : u64{UINT32_MAX};
                if (maximum + rec.step > typeMax)
                    continue;
                for (const auto &other : recurrences)
                    if (other.entry == rec.entry && other.start == rec.start &&
                        other.step == rec.step)
                        inductions.emplace(other.phi, static_cast<u32>(maximum));
            }
        }
    }
};
} // namespace Shader::Optimization
