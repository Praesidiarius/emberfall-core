// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// TalentRules.h: the rules of the Emberfall spec and talent trees (docs/design/talents.md §1.3, §3.4)
// as a small pure function over (tree data, rank string, level, budgets). Header-only, standard
// library only, no dependency on the client or the server.
//
// This one file is shared: the client uses it for previews (client/src/data/), and the server module
// mod-emberfall carries a verbatim copy for its authoritative validation (Apache-2.0 code may be
// included in the AGPL-3.0 module; never the other way round, SPEC §2.3). Both run the same JSON test
// vectors (tools/py/tests/data/talent_rules/*.json, written by `python -m emberfall_content.talents
// vectors`, whose Python twin is tools/py/emberfall_content/talents/rules.py), so preview and server
// cannot drift. Change all three together.
//
// Wire form (talents.md §4.3): a tree's rank string has one character per node in `ord` order, `0`-`9`
// the rank of a normal node, `0` or `a`/`b`/`c` the option of a choice node; a loadout is
// `<classRanks>.<specRanks>`.
//
// Checks, the first failure wins: Decode (length, characters, a letter on a normal node); then per node
// of the class tree and then of the spec tree, in `ord` order, for each invested node: Rank (above its
// max), Level (its required level), Path (it has parents and none is full), Gate (a gate at or above
// its row is short of points; detail: the gate's row), Choice (a digit above 0 or an unknown option on
// a choice node); finally BudgetClass, BudgetSpec. Details are node IDs except for Gate.
//
// Free nodes (freeSpecMask has the spec's bit) are full for that spec whatever the string says, skip
// the per-node checks and cost nothing. Gates count every invested rank above them, free ones included;
// budgets count paid ranks only. Since edges point downwards and gates only count rows above them, a
// final state is valid iff every invested node passes on its own, in any order (talents.md §1.3).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ef::talents
{
    enum class NodeKind : uint8_t
    {
        Passive = 1,
        Active = 2,
        Choice = 3,
    };

    enum class RuleError : uint8_t
    {
        Ok,
        Decode,
        Rank,
        Level,
        Path,
        Gate,
        Choice,
        BudgetClass,
        BudgetSpec,
    };

    // The protocol's error codes (talents.md §4.5).
    constexpr const char* errorCode(RuleError e)
    {
        switch (e)
        {
        case RuleError::Ok: return "OK";
        case RuleError::Decode: return "DECODE";
        case RuleError::Rank: return "RANK";
        case RuleError::Level: return "LEVEL";
        case RuleError::Path: return "PATH";
        case RuleError::Gate: return "GATE";
        case RuleError::Choice: return "CHOICE";
        case RuleError::BudgetClass: return "BUDGET_C";
        case RuleError::BudgetSpec: return "BUDGET_S";
        }
        return "?";
    }

    constexpr size_t kMaxParents = 4;
    constexpr uint8_t kMaxWireRank = 9;
    constexpr uint8_t kMaxOptions = 3;

    struct RuleNode
    {
        uint32_t id = 0;
        uint32_t row = 0;
        NodeKind kind = NodeKind::Passive;
        uint8_t maxRank = 1;      // 1 for a choice node
        uint8_t numOptions = 0;   // choice nodes: 2-3
        uint32_t requiredLevel = 0;
        uint32_t freeSpecMask = 0;
        std::array<int32_t, kMaxParents> parents{-1, -1, -1, -1}; // `ord` indices into the same tree, -1 = none
    };

    struct RuleGate
    {
        uint32_t row = 0;
        uint32_t points = 0;
    };

    struct RuleTree
    {
        std::vector<RuleNode> nodes; // in `ord` order
        std::vector<RuleGate> gates;
    };

    // One node's entry in a rank string: the rank (a choice node: 1 when an option is picked) and the
    // option (1-3, choice nodes only; 0 = none).
    struct NodeValue
    {
        uint8_t rank = 0;
        uint8_t option = 0;
    };

    struct RuleResult
    {
        RuleError code = RuleError::Ok;
        uint32_t detail = 0;
        bool ok() const { return code == RuleError::Ok; }
    };

    // The values of a tree's rank string, or nothing when it is malformed.
    inline std::optional<std::vector<NodeValue>> decodeRanks(const RuleTree& tree, std::string_view text)
    {
        if (text.size() != tree.nodes.size())
            return std::nullopt;
        std::vector<NodeValue> out(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c >= '0' && c <= '9')
                out[i].rank = static_cast<uint8_t>(c - '0');
            else if (c >= 'a' && c <= 'c' && tree.nodes[i].kind == NodeKind::Choice)
                out[i] = NodeValue{1, static_cast<uint8_t>(c - 'a' + 1)};
            else
                return std::nullopt;
        }
        return out;
    }

    // The rank string of a tree's values (the inverse of decodeRanks).
    inline std::string encodeRanks(const RuleTree& tree, std::span<const NodeValue> values)
    {
        std::string out(tree.nodes.size(), '0');
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
        {
            if (tree.nodes[i].kind == NodeKind::Choice && values[i].option >= 1 && values[i].option <= kMaxOptions)
                out[i] = static_cast<char>('a' + values[i].option - 1);
            else
                out[i] = static_cast<char>('0' + (values[i].rank > kMaxWireRank ? kMaxWireRank : values[i].rank));
        }
        return out;
    }

    // The rank a node counts with: a free node its max rank, a choice node 1 when anything is picked.
    inline uint32_t effectiveRank(const RuleNode& node, NodeValue value, uint32_t specBit)
    {
        if ((node.freeSpecMask & specBit) != 0)
            return node.maxRank;
        if (node.kind == NodeKind::Choice)
            return value.option != 0 || value.rank != 0 ? 1u : 0u;
        return value.rank;
    }

    inline bool isFull(const RuleNode& node, uint32_t effective) { return effective > 0 && effective >= node.maxRank; }

    // Points invested in the tree on rows above `row` (free ranks included).
    inline uint32_t investedAbove(const RuleTree& tree, std::span<const NodeValue> values, uint32_t row, uint32_t specBit)
    {
        uint32_t sum = 0;
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
            if (tree.nodes[i].row < row)
                sum += effectiveRank(tree.nodes[i], values[i], specBit);
        return sum;
    }

    // Points paid in a tree (free nodes cost nothing).
    inline uint32_t spentPoints(const RuleTree& tree, std::span<const NodeValue> values, uint32_t specBit)
    {
        uint32_t sum = 0;
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
            if ((tree.nodes[i].freeSpecMask & specBit) == 0)
                sum += effectiveRank(tree.nodes[i], values[i], specBit);
        return sum;
    }

    // The per-node rules for one invested node (`index` in `ord` order).
    inline RuleResult checkNode(const RuleTree& tree, std::span<const NodeValue> values, size_t index, uint32_t level,
                                uint32_t specBit)
    {
        const RuleNode& node = tree.nodes[index];
        const NodeValue value = values[index];
        if ((node.freeSpecMask & specBit) != 0 || (value.rank == 0 && value.option == 0))
            return {};
        if (node.kind != NodeKind::Choice && value.rank > node.maxRank)
            return {RuleError::Rank, node.id};
        if (node.requiredLevel > level)
            return {RuleError::Level, node.id};
        bool hasParent = false, parentFull = false;
        for (const int32_t p : node.parents)
        {
            if (p < 0 || static_cast<size_t>(p) >= tree.nodes.size() || static_cast<size_t>(p) >= values.size())
                continue;
            hasParent = true;
            const size_t pi = static_cast<size_t>(p);
            if (isFull(tree.nodes[pi], effectiveRank(tree.nodes[pi], values[pi], specBit)))
                parentFull = true;
        }
        if (hasParent && !parentFull)
            return {RuleError::Path, node.id};
        for (const RuleGate& gate : tree.gates)
            if (gate.row <= node.row && investedAbove(tree, values, gate.row, specBit) < gate.points)
                return {RuleError::Gate, gate.row};
        if (node.kind == NodeKind::Choice && (value.option == 0 || value.option > node.numOptions))
            return {RuleError::Choice, node.id};
        return {};
    }

    // Every node rule of one tree, in `ord` order (no budget).
    inline RuleResult checkTree(const RuleTree& tree, std::span<const NodeValue> values, uint32_t level, uint32_t specBit)
    {
        if (values.size() != tree.nodes.size())
            return {RuleError::Decode, 0};
        for (size_t i = 0; i < tree.nodes.size(); ++i)
            if (const RuleResult r = checkNode(tree, values, i, level, specBit); !r.ok())
                return r;
        return {};
    }

    // A whole loadout `<classRanks>.<specRanks>` for a character of `level` in the spec with `specBit`
    // (1 << (EfSpec sort - 1)) and the budgets of that level.
    inline RuleResult validateLoadout(const RuleTree& classTree, const RuleTree& specTree, std::string_view loadout, uint32_t level,
                                      uint32_t classBudget, uint32_t specBudget, uint32_t specBit)
    {
        const size_t dot = loadout.find('.');
        if (dot == std::string_view::npos || loadout.find('.', dot + 1) != std::string_view::npos)
            return {RuleError::Decode, 0};
        const auto classValues = decodeRanks(classTree, loadout.substr(0, dot));
        const auto specValues = decodeRanks(specTree, loadout.substr(dot + 1));
        if (!classValues || !specValues)
            return {RuleError::Decode, 0};
        if (const RuleResult r = checkTree(classTree, *classValues, level, specBit); !r.ok())
            return r;
        if (const RuleResult r = checkTree(specTree, *specValues, level, specBit); !r.ok())
            return r;
        if (spentPoints(classTree, *classValues, specBit) > classBudget)
            return {RuleError::BudgetClass, 0};
        if (spentPoints(specTree, *specValues, specBit) > specBudget)
            return {RuleError::BudgetSpec, 0};
        return {};
    }
}
