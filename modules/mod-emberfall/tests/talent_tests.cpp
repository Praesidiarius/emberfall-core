/*
 * This file is part of Emberfall's AzerothCore fork (modules/mod-emberfall). See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Standalone tests of mod-emberfall's talent code that needs no core (tests/run_tests.sh builds and runs it;
// it is not part of the worldserver build, which compiles src/ only):
//
//   1. the rules vectors (Emberfall's tools/py/tests/data/talent_rules/example.json, a copy in tests/data/) through
//      the module's copy of TalentRules.h: every case's first error and detail, as the client's Catch2 test does;
//   2. EfTalentModel over the vectors' trees: wire strings <-> stored pairs round trips, Remap;
//   3. the EFT protocol (EftProtocol.h): parse, chunking both ways, names, stored pairs, rate limit; and a
//      decode fuzz: random and mutated bodies never crash and always give a defined result;
//   4. saved loadouts and per-spec bars (EfLoadouts.h): new loadout IDs and the limit, a spec's first bars;
//   5. the class layout's strip: the player buys it in order (no server fill since 2026-10-26), and the abilities a
//      purchase gives (EfTalentModel::NewAbilities, for the bar).
//
//   talent_tests VECTORS.json...    exit status 0 when everything passed

#include "EfLoadouts.h"
#include "EfTalentModel.h"
#include "EftProtocol.h"
#include "TalentRules.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <variant>

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    // ---- a minimal JSON reader (objects, arrays, strings, integers, true/false/null) -----------------------
    struct Json;
    using JsonObject = std::map<std::string, Json>;
    using JsonArray = std::vector<Json>;
    struct Json
    {
        std::variant<std::nullptr_t, bool, long long, std::string, std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>> v;
        long long num() const { return std::get<long long>(v); }
        std::string const& str() const { return std::get<std::string>(v); }
        JsonArray const& arr() const { return *std::get<std::shared_ptr<JsonArray>>(v); }
        JsonObject const& obj() const { return *std::get<std::shared_ptr<JsonObject>>(v); }
        Json const& operator[](std::string const& k) const { return obj().at(k); }
    };

    struct Reader
    {
        std::string const& s;
        size_t i = 0;
        void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
        [[noreturn]] void fail(char const* what) { throw std::runtime_error(std::string("JSON: ") + what + " at " + std::to_string(i)); }
        Json value()
        {
            ws();
            if (i >= s.size()) fail("end of input");
            char const c = s[i];
            if (c == '{')
            {
                auto o = std::make_shared<JsonObject>();
                ++i; ws();
                if (s[i] == '}') { ++i; return Json{o}; }
                while (true)
                {
                    ws();
                    std::string k = string();
                    ws(); if (s[i++] != ':') fail("':'");
                    (*o)[k] = value();
                    ws();
                    if (s[i] == ',') { ++i; continue; }
                    if (s[i] == '}') { ++i; return Json{o}; }
                    fail("',' or '}'");
                }
            }
            if (c == '[')
            {
                auto a = std::make_shared<JsonArray>();
                ++i; ws();
                if (s[i] == ']') { ++i; return Json{a}; }
                while (true)
                {
                    a->push_back(value());
                    ws();
                    if (s[i] == ',') { ++i; continue; }
                    if (s[i] == ']') { ++i; return Json{a}; }
                    fail("',' or ']'");
                }
            }
            if (c == '"') return Json{string()};
            if (s.compare(i, 4, "true") == 0) { i += 4; return Json{true}; }
            if (s.compare(i, 5, "false") == 0) { i += 5; return Json{false}; }
            if (s.compare(i, 4, "null") == 0) { i += 4; return Json{nullptr}; }
            size_t start = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (start == i) fail("a value");
            return Json{std::stoll(s.substr(start, i - start))};
        }
        std::string string()
        {
            if (s[i++] != '"') fail("'\"'");
            std::string out;
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\')
                {
                    ++i;
                    char const e = s[i++];
                    switch (e)
                    {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'u': out += '?'; i += 4; break; // not in our vectors
                        default: out += e;
                    }
                }
                else
                    out += s[i++];
            }
            ++i;
            return out;
        }
    };

    using namespace ef::talents;
    namespace eft = ef::eft;

    // A vectors tree's pick groups ("groups": the slot thresholds per group; none when absent).
    std::vector<RuleGroup> groupsOf(Json const& t)
    {
        std::vector<RuleGroup> out;
        if (t.obj().count("groups"))
            for (Json const& g : t["groups"].arr())
            {
                out.emplace_back();
                out.back().earned = g["earned"].num() != 0;
                for (Json const& at : g["slots"].arr())
                    out.back().slots.push_back(static_cast<uint32_t>(at.num()));
            }
        return out;
    }

    RuleTree ruleTree(Json const& t)
    {
        RuleTree tree;
        for (Json const& n : t["nodes"].arr())
        {
            RuleNode r;
            r.id = static_cast<uint32_t>(n["id"].num());
            r.kind = static_cast<NodeKind>(n["kind"].num());
            r.maxRank = static_cast<uint8_t>(n["maxRank"].num());
            r.numOptions = static_cast<uint8_t>(n["numOptions"].num());
            r.requiredLevel = static_cast<uint32_t>(n["requiredLevel"].num());
            r.gate = static_cast<uint32_t>(n["gate"].num());
            for (Json const& a : n["auto"].arr())
                if (r.autoCount < kMaxWireRank)
                    r.autoAt[r.autoCount++] = static_cast<uint32_t>(a.num());
            r.parent = static_cast<int32_t>(n["parent"].num());
            r.group = static_cast<uint8_t>(n["group"].num());
            r.reqNode = static_cast<uint32_t>(n["reqNode"].num());
            r.reqOption = static_cast<uint8_t>(n["reqOption"].num());
            tree.nodes.push_back(r);
        }
        tree.groups = groupsOf(t);
        return tree;
    }

    // An EfTalentModel of one class (1) with one spec (91) over the vectors' trees; node spells are made up (node ID x 10 +
    // rank / option).
    std::unique_ptr<EfTalentModel> model(Json const& classTree, Json const& specTree)
    {
        auto m = std::make_unique<EfTalentModel>();
        EfSpec spec;
        spec.id = 91;
        spec.classId = 1;
        spec.sort = 1;
        spec.baseline = {{91901, 10}, {91902, 20}};
        m->specs[spec.id] = spec;
        for (auto const& [json, specId] : {std::pair{&classTree, 0u}, std::pair{&specTree, 91u}})
        {
            EfTree t;
            t.id = static_cast<uint32_t>((*json)["id"].num());
            t.classId = 1;
            t.specId = specId;
            std::vector<uint32_t> ids;
            for (Json const& n : (*json)["nodes"].arr())
                ids.push_back(static_cast<uint32_t>(n["id"].num()));
            uint32_t ord = 0;
            for (Json const& n : (*json)["nodes"].arr())
            {
                EfNode e;
                e.id = static_cast<uint32_t>(n["id"].num());
                e.tree = t.id;
                e.ord = ord++;
                e.kind = static_cast<NodeKind>(n["kind"].num());
                e.maxRank = static_cast<uint32_t>(n["maxRank"].num());
                e.requiredLevel = static_cast<uint32_t>(n["requiredLevel"].num());
                e.gate = static_cast<uint32_t>(n["gate"].num());
                for (Json const& a : n["auto"].arr())
                    e.autoAt.push_back(static_cast<uint32_t>(a.num()));
                if (n["parent"].num() >= 0)
                    e.parent = ids[static_cast<size_t>(n["parent"].num())];
                e.group = static_cast<uint32_t>(n["group"].num());
                e.reqNode = static_cast<uint32_t>(n["reqNode"].num());
                e.reqOption = static_cast<uint32_t>(n["reqOption"].num());
                if (e.kind == NodeKind::Choice)
                    for (long long o = 1; o <= n["numOptions"].num(); ++o)
                        e.options.push_back({e.id * 10 + static_cast<uint32_t>(o)});
                else
                    for (uint32_t r = 1; r <= e.maxRank; ++r)
                        e.ranks.push_back({e.id * 10 + r});
                t.nodes.push_back(e);
            }
            t.groups = groupsOf(*json);
            m->trees[t.id] = t;
        }
        return m;
    }

    // Pick groups (Emberfall's talents.md §1.3 rule 7, the Wrathbringer's row abilities since 2026-10-26) on the vectors'
    // pick tree 830: group 1 (8303, 8305; slots at 2 and 4 points spent), group 2 (8307, 8309; at 7 and 9). The model
    // stores picks like paid nodes, teaches a pick's spell once picked, hashes the groups, and Remap picks the ability of
    // a row whose modifiers a loadout from before (a granted ability) holds.
    void pickTests(std::map<long long, Json> const& treeJson)
    {
        auto m = model(treeJson.at(810), treeJson.at(830));
        m->points[0][60] = {99, 99};
        std::vector<std::string> problems;
        m->Finish(problems);
        check(problems.empty(), "pick model: " + (problems.empty() ? std::string() : problems[0]));
        EfSpec const& spec = *m->Spec(91);
        check(m->trees[830].rules.groups == std::vector<RuleGroup>{{false, {2, 4}}, {false, {7, 9}}}, "pick model: the groups' slots");
        std::string const json = m->CanonicalJson();
        check(json.find("\"groups\":[{\"earned\":0,\"slots\":[2,4]},{\"earned\":0,\"slots\":[7,9]}]") != std::string::npos && json.find("\"group\":2") != std::string::npos,
              "pick model: the groups are hashed");
        std::string const cls(m->trees[810].nodes.size(), '0');
        auto picked = m->PairsOf(spec, cls + ".02100000000");
        check(picked && picked->count(8303) && picked->at(8303).rank == 1 && picked->count(8302), "pick model: a pick is stored");
        check(picked && m->Expected(spec, *picked, 60).count(83031) && !m->Expected(spec, {}, 60).count(83031),
              "pick model: a pick teaches its spell once picked");
        // a loadout from before the pick groups: the rows' modifiers without their abilities (they were granted)
        bool changed = true;
        eft::Pairs const old = {{8302, {3, 0}}, {8304, {2, 0}}, {8306, {1, 0}}};
        eft::Pairs const remapped = m->Remap(spec, old, changed);
        check(!changed && remapped.count(8303) && remapped.count(8305) && remapped.size() == 5, "pick model: Remap picks the rows' abilities");
        check(m->Validate(spec, m->LoadoutString(spec, remapped), 60).ok(), "pick model: the remapped loadout fits its slots");
        eft::Pairs const tooMany = {{8302, {1, 0}}, {8304, {1, 0}}, {8306, {1, 0}}};
        RuleResult const r = m->Validate(spec, m->LoadoutString(spec, m->Remap(spec, tooMany, changed)), 60);
        check(r.code == RuleError::Slot, "pick model: a remapped loadout whose picks do not fit is refused (reset)");
        // tree 831: group 1 by points EARNED (the spec budget at the level: 3 and 5), the Wrathbringer's way. A loadout from
        // before with both rows bought keeps them where the level opens both slots, and is reset (SLOT) where it does not.
        auto e = model(treeJson.at(810), treeJson.at(831));
        e->points[0] = {{1, {99, 3}}, {2, {99, 5}}};
        problems.clear();
        e->Finish(problems);
        check(problems.empty() && e->trees[831].rules.groups[0].earned, "earned pick model");
        EfSpec const& es = *e->Spec(91);
        eft::Pairs const before = {{8304, {1, 0}}, {8306, {1, 0}}};
        eft::Pairs const kept = e->Remap(es, before, changed);
        check(kept.count(8303) && kept.count(8305), "earned picks: Remap picks both rows' abilities");
        check(e->Validate(es, e->LoadoutString(es, kept), 2).ok(), "earned picks: both slots open at 5 points earned");
        check(e->Validate(es, e->LoadoutString(es, kept), 1).code == RuleError::Slot, "earned picks: one slot at 3: reset");
        check(e->Validate(es, cls + ".00100000000", 1).ok(), "earned picks: a pick with nothing spent");
        // picks cost a point (2026-10-26, Nathanael; free before): Nathanael's level-38 Wrathbringer (15 spec points, 2 picks
        // and 14 ranks) in small. Two picks and 4 ranks fit the 5 points earned at level 2 only while picks were free: such
        // a stored loadout fails now (BUDGET_S), so the login's Revalidate resets it, the first time with the data notice.
        eft::Pairs const freePicks = {{8303, {1, 0}}, {8304, {2, 0}}, {8305, {1, 0}}, {8306, {2, 0}}};
        check(e->Validate(es, e->LoadoutString(es, freePicks), 2).code == RuleError::BudgetSpec,
              "picks cost a point: two picks and 4 ranks are over 5 points (reset at login)");
        eft::Pairs fits = freePicks;
        fits[8306] = {1, 0};
        check(e->Validate(es, e->LoadoutString(es, fits), 2).ok(), "picks cost a point: two picks and 2 + 1 ranks are 5");
        check(e->PaidIn(e->trees[831], freePicks) == 6, "picks cost a point: the paid points count the picks");
        check(std::string(RevalidateResetReason(0)) == "DATA" && std::string(RevalidateResetReason(kSpecFlagPicksPaid)) == "LEVEL",
              "picks cost a point: the first revalidation's reset says the data changed, later ones the level");
        std::cout << "picks: groups, storage, Remap\n";
    }

    // T6 on the example's spec tree (Emberfall's tools/py/emberfall_content/talents/vector_trees.py): the granted ability
    // shard (9101, at once) and the granted passive winterhide (9107: rank 1 at once, rank 2 at 4 points spent); the
    // pre-spec state on the class tree (its strip keen -> surge -> ward at levels 2, 3, 4).
    void grantedTests(std::map<long long, Json> const& treeJson)
    {
        auto m = model(treeJson.at(810), treeJson.at(811));
        m->points[0][1] = {23, 36};
        std::vector<std::string> problems;
        m->Finish(problems);
        check(problems.empty(), "granted model: " + (problems.empty() ? std::string() : problems[0]));
        EfSpec const& spec = *m->Spec(91);
        SpellSet const none = m->Expected(spec, {}, 60);
        check(none.count(91011) && none.count(91071) && !none.count(91072), "granted at once: shard and winterhide rank 1");
        // 4 paid points: splinter 2, linger 2 -> winterhide rank 2
        eft::Pairs const four = {{9102, {2, 0}}, {9103, {2, 0}}};
        check(m->PaidIn(m->trees[811], four) == 4, "PaidIn counts paid ranks");
        SpellSet const at4 = m->Expected(spec, four, 60);
        check(at4.count(91072) && !at4.count(91071), "winterhide ranks up at 4 points");
        std::string const wire = m->LoadoutString(spec, four);
        check(m->Validate(spec, wire, 60).ok(), "the 4-point build is valid");
        // a granted node's digit in a wire string is not stored
        std::string given = wire;
        given[given.find('.') + 1] = '1'; // shard
        auto pairs = m->PairsOf(spec, given);
        check(pairs && !pairs->count(9101) && *pairs == four, "a granted node's digit is never stored");
        // a granted replacing node replaces at once; the class-wide map takes its spells back to the replaced spell
        for (EfNode& n : m->trees[811].nodes)
            if (n.id == 9101)
                n.replaces = 777;
        auto const reps = m->Replacements(spec, {}, 60, [](uint32_t) { return false; });
        check(reps.size() == 1 && reps[0].replaced == 777 && reps[0].by == 91011, "a granted replacing node is invested at once");
        check(m->ReplacedBy(1).at(91011) == 777, "ReplacedBy maps the node's spell back");
        check(!m->Expected(spec, {}, 60).count(777), "the replaced spell is not expected");
        // a granted node with a required level: nothing below it, granted from it
        m->trees[811].rules.nodes[0].requiredLevel = 20;
        check(!m->Expected(spec, {}, 19).count(91011) && m->Expected(spec, {}, 20).count(91011), "a granted node waits for its level");
        check(m->Replacements(spec, {}, 19, [](uint32_t) { return false; })[0].by == 0
                  && m->Replacements(spec, {}, 20, [](uint32_t) { return false; })[0].by == 91011,
              "a granted replacing node replaces from its level");
        m->trees[811].rules.nodes[0].requiredLevel = 0;
        // the pre-spec state: the class tree only
        EfSpec const* pre = m->PreSpec(1);
        check(pre && pre->id == 0 && m->SpecOrPre(0, 1) == pre && m->SpecOrPre(0, 2) == nullptr, "PreSpec / SpecOrPre");
        if (pre)
        {
            eft::Pairs const cls = {{9001, {1, 0}}, {9002, {1, 0}}, {9003, {1, 0}}}; // the strip
            std::string const pw = m->LoadoutString(*pre, cls);
            check(!pw.empty() && pw.back() == '.', "a pre-spec loadout has an empty spec part");
            m->points[0][4] = {3, 0};
            check(m->Validate(*pre, pw, 4).ok(), "pre-spec: three class points at level 4");
            m->points[0][4] = {2, 0};
            check(m->Validate(*pre, pw, 4).code == RuleError::BudgetClass, "pre-spec budgets");
            check(m->PairsOf(*pre, pw) == cls, "pre-spec pairs");
            SpellSet const pe = m->Expected(*pre, cls, 4);
            check(pe.count(90011) && pe.count(90031) && !pe.count(91011), "pre-spec: the class tree's spells, no spec tree");
        }
        std::cout << "granted nodes, pre-spec\n";
    }

    // Requirements (Emberfall talents.md §1.3 rule 9, 2026-11-15, feedback #584: Solenne's Mercy only for a paladin who chose
    // Sunmend) on the vectors' tree 850 over the class tree 810: the granted mercy (8501) needs the class choice 9006 at
    // option 1 (mend), its row mercy_mod (8502, 2 ranks) -> mercy_end (8503) hangs under it; bulwark (8504) needs the class
    // tree's barrier (9004); gated (8507) waits for 3 points spent. The vectors' drops through dropUnheld, the grant and the
    // take-back (Expected), and the login's Refit of a stored loadout that chose kneel holding mercy's row: a clean partial
    // refund (the row and what its points held go, everything else stays, the strip too), RESET REQUIRES naming mercy.
    void requireTests(Json const& doc, std::map<long long, RuleTree> const& trees, std::map<long long, Json> const& treeJson)
    {
        size_t drops = 0;
        for (Json const& d : doc["drops"].arr())
        {
            ++drops;
            RuleTree const& ct = trees.at(d["classTree"].num());
            RuleTree const& st = trees.at(d["specTree"].num());
            std::string const loadout = d["loadout"].str();
            size_t const dot = loadout.find('.');
            auto cls = decodeRanks(ct, loadout.substr(0, dot));
            auto spec = decodeRanks(st, loadout.substr(dot + 1));
            check(cls && spec, d["name"].str() + ": decodes");
            if (!cls || !spec)
                continue;
            dropUnheld(ct, *cls, st, *spec, static_cast<uint32_t>(d["level"].num()));
            std::string const after = encodeRanks(ct, *cls) + "." + encodeRanks(st, *spec);
            check(after == d["expect"].str(), d["name"].str() + ": expected " + d["expect"].str() + ", got " + after);
        }
        check(drops >= 4, "requirement drops in the vectors");

        auto m = model(treeJson.at(810), treeJson.at(850));
        m->points[0][1] = {99, 99};
        std::vector<std::string> problems;
        m->Finish(problems);
        check(problems.empty(), "require model: " + (problems.empty() ? std::string() : problems[0]));
        EfSpec const& spec = *m->Spec(91);
        eft::Pairs const strip = {{9001, {1, 0}}, {9002, {1, 0}}, {9003, {1, 0}}, {9004, {1, 0}}};
        eft::Pairs mend = strip, kneel = strip;
        mend[9006] = {1, 1};
        kneel[9006] = {1, 2};
        // the grant follows the class choice: mercy's spell with mend, not with kneel (nor before any choice)
        check(m->Expected(spec, mend, 30).count(85011) && !m->Expected(spec, kneel, 30).count(85011) && !m->Expected(spec, strip, 30).count(85011),
              "a granted node is taught only while its requirement is held");
        // a replacing node shut by its requirement replaces nothing: the replaced spell stays
        for (EfNode& n : m->trees[850].nodes)
            if (n.id == 8501)
                n.replaces = 90061; // mend's spell (node 9006 option 1)
        check(!m->Expected(spec, mend, 30).count(90061) && m->Expected(spec, mend, 30).count(85011),
              "with mend, mercy takes mend's place");
        check(m->Replacements(spec, kneel, 30, [](uint32_t) { return false; })[0].by == 0, "with kneel, mercy replaces nothing");
        // the validator: kneel with mercy's row fails (PATH); the model's Refit keeps everything but the row and the gate
        // its points held, and names mercy
        eft::Pairs stored = kneel;
        stored[8502] = {2, 0};
        stored[8503] = {1, 0};
        stored[8505] = {1, 0};
        stored[8507] = {1, 0};
        check(m->Validate(spec, m->LoadoutString(spec, stored), 30).code == RuleError::Path, "kneel with mercy's row fails");
        uint32_t shut = 0;
        eft::Pairs const refit = m->Refit(spec, stored, 30, &shut);
        eft::Pairs want = kneel;
        want[8505] = {1, 0};
        check(refit == want, "Refit: the row and the gate it held are refunded, the class tree and the rest kept");
        check(shut == 8501, "Refit names the node whose requirement is not held");
        check(m->Validate(spec, m->LoadoutString(spec, refit), 30).ok(), "the refit loadout is valid");
        check(m->PaidIn(m->trees[850], stored) - m->PaidIn(m->trees[850], refit) == 4, "Refit: the row's 3 points and gated's 1 come back");
        // the same loadout with mend is valid: nothing to refit; Refit of a level failure still keeps the strip (as before)
        eft::Pairs withMend = stored;
        withMend[9006] = {1, 1};
        check(m->Validate(spec, m->LoadoutString(spec, withMend), 30).ok(), "mend with mercy's row is valid");
        check(!m->DropUnheld(spec, withMend, 30).has_value(), "DropUnheld: nothing while the requirement is held");
        uint32_t none = 1;
        eft::Pairs const low = m->Refit(spec, withMend, 3, &none);
        check(none == 0 && !low.count(9006) && !low.count(8502), "a level loss refits as before (WithKept), no REQUIRES");
        // the model refuses a requirement of another class's or spec's tree
        auto bad = model(treeJson.at(810), treeJson.at(850));
        bad->points[0][1] = {99, 99};
        for (EfNode& n : bad->trees[850].nodes)
            if (n.id == 8504)
                n.reqNode = 4242;
        problems.clear();
        bad->Finish(problems);
        check(!problems.empty(), "a requirement on an unknown node is a structural problem");
        std::string const json = m->CanonicalJson();
        check(json.find("\"req_node\":9006,\"req_option\":1") != std::string::npos, "the requirement is hashed");
        std::cout << "requirements: drops, grant, Refit\n";
    }

    void runVectors(std::string const& path)
    {
        std::ifstream in(path);
        check(static_cast<bool>(in), "open " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string const text = ss.str();
        Reader reader{text};
        Json const doc = reader.value();
        std::map<long long, RuleTree> trees;
        std::map<long long, Json> treeJson;
        for (Json const& t : doc["trees"].arr())
        {
            trees[t["id"].num()] = ruleTree(t);
            treeJson[t["id"].num()] = t;
        }
        size_t cases = 0;
        for (Json const& c : doc["cases"].arr())
        {
            ++cases;
            RuleTree const& ct = trees.at(c["classTree"].num());
            RuleTree const& st = trees.at(c["specTree"].num());
            RuleResult const r = validateLoadout(ct, st, c["loadout"].str(), static_cast<uint32_t>(c["level"].num()),
                                                 static_cast<uint32_t>(c["classBudget"].num()), static_cast<uint32_t>(c["specBudget"].num()));
            check(errorCode(r.code) == c["code"].str() && r.detail == static_cast<uint32_t>(c["detail"].num()),
                  path + ": " + c["name"].str() + ": expected " + c["code"].str() + " " + std::to_string(c["detail"].num()) + ", got " +
                      errorCode(r.code) + " " + std::to_string(r.detail));

            // The model gives the same answer, and a valid loadout survives wire -> pairs -> wire.
            // the pre-spec state (spec tree 0, T6): a model over the class tree with any spec tree, its PreSpec
            bool const pre = c["specTree"].num() == 0;
            long long specTreeId = c["specTree"].num();
            if (pre)
                for (auto const& [id, t] : treeJson)
                    if (id != 0 && id != c["classTree"].num())
                        specTreeId = id;
            auto m = model(treeJson.at(c["classTree"].num()), treeJson.at(specTreeId));
            m->points[0][1] = {static_cast<uint32_t>(c["classBudget"].num()), static_cast<uint32_t>(c["specBudget"].num())};
            std::vector<std::string> problems;
            m->Finish(problems);
            check(problems.empty(), c["name"].str() + ": model problems: " + (problems.empty() ? "" : problems[0]));
            EfSpec const& spec = pre ? *m->PreSpec(1) : *m->Spec(91);
            RuleResult const mr = m->Validate(spec, c["loadout"].str(), static_cast<uint32_t>(c["level"].num()));
            check(mr.code == r.code && mr.detail == r.detail, c["name"].str() + ": the model's Validate differs");
            if (r.ok())
            {
                auto pairs = m->PairsOf(spec, c["loadout"].str());
                check(pairs.has_value(), c["name"].str() + ": PairsOf");
                if (pairs)
                {
                    // a granted node's digit reads back as 0: compare through the pairs
                    std::string const again = m->LoadoutString(spec, *pairs);
                    auto pairs2 = m->PairsOf(spec, again);
                    check(pairs2 && *pairs2 == *pairs, c["name"].str() + ": wire -> pairs -> wire");
                    auto stored = ef::eft::decodePairs(ef::eft::encodePairs(*pairs));
                    check(stored && *stored == *pairs, c["name"].str() + ": pairs -> DB text -> pairs");
                    bool changed = true;
                    check(m->Remap(spec, *pairs, changed) == *pairs && !changed, c["name"].str() + ": Remap of current data changes nothing");
                    SpellSet const spells = m->Expected(spec, *pairs, static_cast<uint32_t>(c["level"].num()));
                    for (auto const& [node, p] : *pairs)
                    {
                        EfNode const* n = m->Node(node);
                        uint32_t const want = n->kind == NodeKind::Choice ? node * 10 + p.option : node * 10 + std::min<uint32_t>(p.rank, n->maxRank);
                        check(spells.count(want) == 1, c["name"].str() + ": node " + std::to_string(node) + " teaches " + std::to_string(want));
                    }
                }
            }
        }
        check(cases > 0, path + ": no cases");
        std::cout << path << ": " << cases << " cases\n";

        grantedTests(treeJson);
        pickTests(treeJson);
        requireTests(doc, trees, treeJson);
        // Remap after a data change: a node removed, a max rank lowered, an option removed.
        auto m = model(treeJson.at(810), treeJson.at(811));
        m->points[0][1] = {99, 99};
        std::vector<std::string> problems;
        m->Finish(problems);
        eft::Pairs const pairs = {{9008, {2, 0}}, {9102, {2, 0}}, {9006, {1, 2}}, {12345, {1, 0}}};
        bool changed = false;
        eft::Pairs const kept = m->Remap(*m->Spec(91), pairs, changed);
        check(changed && !kept.count(12345) && kept.size() == 3, "Remap drops an unknown node");
        m->trees[811].nodes[1].maxRank = 1;
        m->trees[811].nodes[1].ranks.resize(1);
        m->trees[810].nodes[5].options.resize(1);
        m->Finish(problems); // the choice node has one option now: a problem, but Remap still works
        eft::Pairs const kept2 = m->Remap(*m->Spec(91), pairs, changed);
        check(changed && kept2.at(9102).rank == 1 && !kept2.count(9006), "Remap clamps ranks and drops a lost option");

        // Replacing nodes (M6): the class tree's 9004 replaces the baseline spell 91901.
        auto r = model(treeJson.at(810), treeJson.at(811));
        r->points[0][1] = {99, 99};
        r->Finish(problems);
        EfTree& ct = r->trees[810];
        for (EfNode& n : ct.nodes)
            if (n.id == 9004)
                n.replaces = 91901;
        EfSpec const& lamp = *r->Spec(91);
        auto const notPassive = [](uint32_t) { return false; };
        eft::Pairs const without = {{9008, {2, 0}}};
        eft::Pairs const with = {{9008, {2, 0}}, {9004, {1, 0}}};
        SpellSet const before = r->Expected(lamp, without, 20), after = r->Expected(lamp, with, 20);
        check(before.count(91901) && !after.count(91901) && after.count(90041), "an invested replacing node takes the baseline spell's place");
        auto const reps = r->Replacements(lamp, with, 20, notPassive);
        check(reps.size() == 1 && reps[0].replaced == 91901 && reps[0].by == 90041 && reps[0].nodeSpells == std::vector<uint32_t>{90041},
              "Replacements: replaced, by, the node's spells");
        auto const idle = r->Replacements(lamp, without, 20, notPassive);
        check(idle.size() == 1 && idle[0].by == 0, "Replacements: a node that is not invested replaces nothing");
        check(r->Replaced(lamp, with, 20, notPassive) == std::set<uint32_t>{91901} && r->Replaced(lamp, without, 20, notPassive).empty(), "Replaced");
        // a passive first spell of the rank: the ability after it goes on the bars
        for (EfNode& n : ct.nodes)
            if (n.id == 9004)
                n.ranks[0] = {77, 78};
        auto const passiveFirst = r->Replacements(lamp, with, 20, [](uint32_t s) { return s == 77; });
        check(passiveFirst.size() == 1 && passiveFirst[0].by == 78, "Replacements: the first non-passive spell goes on the bars");
    }

    void protocolTests()
    {
        auto r = eft::parse("1;12;APPLY;91;1/1;1120010.21a10");
        check(r.ok() && r.request.seq == 12 && r.request.cmd == "APPLY" && r.request.fields.size() == 3 && r.request.fields[2] == "1120010.21a10",
              "parse an APPLY");
        check(eft::parse("2;7;HELLO;x").error == eft::ParseError::Version && eft::parse("2;7;HELLO;x").seq == 7, "another version");
        check(eft::parse("1;10000;HELLO").error == eft::ParseError::Decode, "seq above 9999");
        check(eft::parse("1;5;hello").error == eft::ParseError::Decode && eft::parse("1;5;hello").seq == 5, "lower-case command");
        check(eft::parse("").error == eft::ParseError::Decode, "empty body");
        check(eft::parse("1;1").error == eft::ParseError::Decode, "no command");
        check(eft::parse(std::string(252, '1')).error == eft::ParseError::Decode, "body above 251 bytes");
        check(eft::parse(std::string("1;1;HELLO;\x01")).error == eft::ParseError::Decode, "a control character");
        check(eft::parse("1;3;HELLO;;").ok() && eft::parse("1;3;HELLO;;").request.fields.size() == 2, "empty fields are kept");

        check(eft::partOf("1/1") == std::make_pair(1u, 1u) && eft::partOf("8/8").has_value(), "part fields");
        check(!eft::partOf("0/1") && !eft::partOf("2/1") && !eft::partOf("1/9") && !eft::partOf("1") && !eft::partOf("a/b"), "bad part fields");

        // Outgoing chunks: every body at most 251 bytes, the payload in order.
        std::string const payload(600, '3');
        auto bodies = eft::chunked(0, "LOADOUT", {"100", "1", "1", "Default"}, payload);
        check(bodies.size() == 3, "600 bytes in 3 parts");
        std::string joined;
        eft::Reassembly re;
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            check(bodies[i].size() <= eft::kMaxBody, "chunk size");
            auto p = eft::parse(bodies[i]);
            check(p.ok() && p.request.fields.size() == 6, "chunk parses");
            auto part = eft::partOf(p.request.fields[4]);
            auto st = re.feed(1, part->first, part->second, p.request.fields[5], 1000);
            check(st == (i + 1 == bodies.size() ? eft::Reassembly::Status::Complete : eft::Reassembly::Status::Incomplete), "reassembly status");
        }
        check(re.payload() == payload, "reassembled payload");
        check(eft::chunked(0, "LOADOUT", {}, std::string(3000, '1')).empty(), "too long for 8 parts");
        check(eft::chunked(0, "OK", {}, "").size() == 1, "an empty payload is one part");

        // Reassembly errors: out of order, another seq, timeout.
        eft::Reassembly a;
        check(a.feed(5, 2, 2, "x", 0) == eft::Reassembly::Status::Error, "part 2 first");
        check(a.feed(5, 1, 2, "x", 0) == eft::Reassembly::Status::Incomplete, "part 1");
        check(a.feed(6, 2, 2, "x", 10) == eft::Reassembly::Status::Error, "part 2 of another seq");
        check(a.feed(5, 1, 2, "x", 0) == eft::Reassembly::Status::Incomplete && a.feed(5, 2, 3, "x", 0) == eft::Reassembly::Status::Error,
              "another total");
        check(a.feed(5, 1, 2, "x", 0) == eft::Reassembly::Status::Incomplete && a.feed(5, 2, 2, "y", 6000) == eft::Reassembly::Status::Error,
              "a set times out after 5 s");
        check(a.feed(5, 1, 1, "z", 7000) == eft::Reassembly::Status::Complete && a.payload() == "z", "a single part");

        // Names.
        check(eft::encodeName("A;b,c%d") == "A%3Bb%2Cc%25d", "encode name");
        check(eft::decodeName(eft::encodeName("Ünïcode; name")) == std::string("Ünïcode; name"), "name round trip");
        check(!eft::decodeName("%4") && !eft::decodeName("%zz") && !eft::decodeName("%"), "bad escapes");
        check(eft::validName("Default") && eft::validName("Äxe") && !eft::validName("") && !eft::validName(std::string(25, 'a')) &&
              !eft::validName("a\tb"), "valid names");

        // Stored pairs.
        auto pairs = eft::decodePairs("50012:2,50020:b");
        check(pairs && pairs->at(50012).rank == 2 && pairs->at(50020).option == 2 && eft::encodePairs(*pairs) == "50012:2,50020:b", "pairs");
        check(eft::decodePairs("") && eft::decodePairs("")->empty(), "no pairs");
        for (char const* bad : {"1", "1:", ":1", "1:0", "1:e", "1:2,1:3", "0:1", "1:22", "x:1", "1:1,"})
            check(!eft::decodePairs(bad), std::string("bad pairs '") + bad + "'");

        // Rate limit: 20 per 10 s.
        eft::RateLimit limit(20, 10000);
        int allowed = 0;
        for (int i = 0; i < 30; ++i)
            allowed += limit.allow(100 + static_cast<uint64_t>(i));
        check(allowed == 20 && limit.allow(10200), "rate limit");
    }

    // Random and mutated bodies: parse never crashes; every result is one of the defined ones, and a parsed
    // request is printable ASCII within the limits. The decoders of the payloads get the same treatment.
    void fuzz()
    {
        std::mt19937 rng(20261011);
        std::vector<std::string> seeds = {"1;12;APPLY;91;1/1;1120010.21a10", "1;1;HELLO;3f9c0a17d2e4b801;12340", "1;2;SPEC;100",
                                          "1;3;RESET;100;cs", "1;4;APPLY;100;1/2;0000", "1;5;APPLY;100;2/2;.0000"};
        std::string alphabet = "0123456789;./abcdefABCDEFHELOSPYRTx%:,\t\n\x01\xff ";
        size_t parsed = 0, decodeErrors = 0, versionErrors = 0;
        eft::Reassembly re;
        for (int i = 0; i < 200000; ++i)
        {
            std::string body;
            if (i % 2)
            {
                body = seeds[rng() % seeds.size()];
                int const edits = 1 + static_cast<int>(rng() % 4);
                for (int e = 0; e < edits; ++e)
                {
                    size_t const pos = body.empty() ? 0 : rng() % body.size();
                    switch (rng() % 3)
                    {
                        case 0: if (!body.empty()) body.erase(pos, 1); break;
                        case 1: body.insert(body.begin() + static_cast<long>(pos), alphabet[rng() % alphabet.size()]); break;
                        default: if (!body.empty()) body[pos] = alphabet[rng() % alphabet.size()];
                    }
                }
            }
            else
            {
                size_t const n = rng() % 300;
                for (size_t k = 0; k < n; ++k)
                    body += alphabet[rng() % alphabet.size()];
            }
            auto const r = eft::parse(body);
            switch (r.error)
            {
                case eft::ParseError::None:
                {
                    ++parsed;
                    bool fine = r.request.version == 1 && r.request.seq <= 9999 && !r.request.cmd.empty() && body.size() <= eft::kMaxBody;
                    for (char c : body)
                        fine = fine && eft::isPrintable(c);
                    check(fine, "fuzz: an accepted body breaks the format: " + body);
                    for (std::string const& f : r.request.fields)
                    {
                        (void)eft::partOf(f);
                        (void)eft::decodePairs(f);
                        (void)eft::decodeName(f);
                    }
                    if (r.request.fields.size() >= 3)
                        if (auto part = eft::partOf(r.request.fields[1]))
                            (void)re.feed(r.request.seq, part->first, part->second, r.request.fields[2], static_cast<uint64_t>(i));
                    break;
                }
                case eft::ParseError::Decode: ++decodeErrors; break;
                case eft::ParseError::Version: ++versionErrors; break;
            }
            check(r.seq <= 9999, "fuzz: seq in range");
        }
        check(parsed > 1000 && decodeErrors > 1000, "fuzz: both outcomes seen");
        std::cout << "fuzz: 200000 bodies, " << parsed << " parsed, " << decodeErrors << " DECODE, " << versionErrors << " VERSION\n";

        // Rank strings into the validator: random strings of the right and wrong lengths never crash.
        RuleTree tree;
        for (uint32_t k = 0; k < 6; ++k)
        {
            RuleNode n;
            n.id = 100 + k;
            n.kind = k == 4 ? NodeKind::Choice : NodeKind::Passive;
            n.maxRank = k == 4 ? 1 : 3;
            n.numOptions = k == 4 ? 2 : 0;
            if (k >= 2)
                n.parent = static_cast<int32_t>(k - 2);
            n.gate = k >= 4 ? 4 : 0;
            tree.nodes.push_back(n);
        }
        std::string const chars = "0123456789abcd.x";
        for (int i = 0; i < 100000; ++i)
        {
            std::string s;
            size_t const n = rng() % 16;
            for (size_t k = 0; k < n; ++k)
                s += chars[rng() % chars.size()];
            RuleResult const r = validateLoadout(tree, tree, s, rng() % 90, rng() % 20, rng() % 20);
            check(r.code <= RuleError::BudgetSpec, "fuzz: rules result in range");
        }
    }
}

namespace
{
    void loadoutTests()
    {
        using namespace ef::talents;
        check(NewLoadoutId({}, 5) == 1, "loadouts: the first ID is 1");
        check(NewLoadoutId({1, 2, 4}, 5) == 3, "loadouts: the smallest unused ID");
        check(NewLoadoutId({1, 2, 3, 4}, 5) == 5, "loadouts: the fifth of five");
        check(NewLoadoutId({1, 2, 3, 4, 5}, 5) == 0, "loadouts: past the limit -> 0 (ERR LIMIT)");
        check(NewLoadoutId({2, 3, 4, 5, 6}, 5) == 0, "loadouts: the limit counts loadouts, not IDs");
        check(NewLoadoutId({1}, 1) == 0, "loadouts: a limit of 1");

        auto known = [](uint32_t spell) { return spell != 200 && spell != 999; };
        std::vector<BarButton> const current{{0, 100, 0}, {1, 200, 0}, {2, 5, 0x80}, {3, 300, 0x40}, {13, 999, 0}};
        // spell 200 and 999 unknown: their buttons go; the item (0x80) and macro (0x40) buttons stay; baseline 400
        // into the first empty button of the first bar (1), 100 is on a button already, 999 is unknown
        std::vector<BarButton> const bars = FirstBars(current, known, {100, 400, 999});
        std::vector<BarButton> const want{{0, 100, 0}, {1, 400, 0}, {2, 5, 0x80}, {3, 300, 0x40}};
        check(bars == want, "bars: first bars = current without unknown spells + baseline in the first empty button");
        std::vector<BarButton> full;
        for (uint32_t b = 0; b < kFirstBarButtons; ++b)
            full.push_back({b, 1000 + b, 0});
        check(FirstBars(full, known, {400}).size() == kFirstBarButtons, "bars: a full first bar takes no baseline spell");
        check(FirstBars({}, known, {400, 401}) == std::vector<BarButton>{{0, 400, 0}, {1, 401, 0}}, "bars: empty bars get the baseline in order");
        check(FirstBars({{200, 1, 0}}, known, {}).empty(), "bars: a button past 143 is dropped");
    }

    // The class layout's strip (2026-10-26, Nathanael: the player buys it, one point a slot, in order): each slot needs
    // the one before (its parent), nothing is bought by itself; EfTalentModel::NewAbilities names the abilities a purchase
    // gives (they go on the bar).
    void stripTests()
    {
        using namespace ef::talents;
        EfTalentModel m;
        m.points[0] = {{1, {0, 0}}, {2, {1, 0}}, {3, {2, 0}}, {4, {3, 0}}, {5, {4, 0}}, {10, {4, 1}}};
        auto node = [](uint32_t id, uint32_t ord, NodeKind kind, uint32_t parent, uint32_t level) {
            EfNode n;
            n.id = id;
            n.tree = 1;
            n.ord = ord;
            n.kind = kind;
            n.requiredLevel = level;
            n.parent = parent;
            if (kind == NodeKind::Choice)
                n.options = {{id * 10 + 1}, {id * 10 + 2}};
            else
                n.ranks.push_back({id * 10 + 1});
            return n;
        };
        EfTree ct;
        ct.id = 1;
        ct.classId = 1;
        // the strip 11 (level 2, passive) -> 12 (3, active) -> 13 (4, passive), then a choice row node 21
        ct.nodes = {node(11, 0, NodeKind::Passive, 0, 2), node(12, 1, NodeKind::Active, 11, 3),
                    node(13, 2, NodeKind::Passive, 12, 4), node(21, 3, NodeKind::Choice, 0, 0)};
        m.trees[1] = ct;
        EfTree st;
        st.id = 2;
        st.classId = 1;
        st.specId = 91;
        st.nodes = {node(31, 0, NodeKind::Passive, 0, 0)};
        st.nodes[0].tree = 2;
        m.trees[2] = st;
        EfSpec spec;
        spec.id = 91;
        spec.classId = 1;
        spec.sort = 1;
        m.specs[91] = spec;
        std::vector<std::string> problems;
        m.Finish(problems);
        check(problems.empty(), "strip: model problems: " + (problems.empty() ? std::string() : problems[0]));
        EfSpec const& pre = *m.PreSpec(1);
        auto valid = [&](eft::Pairs const& p, uint32_t level) { return m.Validate(pre, m.LoadoutString(pre, p), level).ok(); };
        check(valid({}, 4), "strip: unspent points are valid (nothing is bought by itself)");
        check(valid({{11, {1, 0}}}, 2), "strip: the first slot at level 2");
        check(!valid({{12, {1, 0}}}, 3), "strip: the second slot needs the first (PATH)");
        check(!valid({{11, {1, 0}}, {12, {1, 0}}}, 2), "strip: the second slot not before its level");
        check(valid({{11, {1, 0}}, {12, {1, 0}}, {13, {1, 0}}}, 4), "strip: one point a slot, in order");
        check(!valid({{11, {1, 0}}, {13, {1, 0}}}, 4), "strip: no slot skipped");

        auto passive = [](uint32_t spell) { return spell == 111; };
        check(m.NewAbilities({{11, {1, 0}}}, {{11, {1, 0}}, {12, {1, 0}}}, passive) == std::vector<uint32_t>{121},
              "new abilities: a bought active node's spell");
        check(m.NewAbilities({}, {{11, {1, 0}}, {13, {1, 0}}}, passive).empty(), "new abilities: passive nodes give none");
        check(m.NewAbilities({{12, {1, 0}}}, {{12, {1, 0}}}, passive).empty(), "new abilities: an owned node gives none");
        check(m.NewAbilities({}, {{21, {1, 2}}}, passive) == std::vector<uint32_t>{212}, "new abilities: a picked option");
        check(m.NewAbilities({{21, {1, 1}}}, {{21, {1, 2}}}, passive) == std::vector<uint32_t>{212},
              "new abilities: a switched option");
        check(m.NewAbilities({{12, {1, 0}}}, {}, passive).empty(), "new abilities: a refund gives none");
    }

    // The strip stays once learned (Emberfall 2026-10-28, Nathanael; talents.md §1.3 rule 8): a class tree's paid node with
    // no gate below the spec level is permanent (TalentRules.h isPermanent); EfTalentModel::Kept / CheckKept / Overlay /
    // WithKept are what APPLY, SAVE, LOAD, RESET and a spec switch go by (EfTalents.cpp), Refit what a login does with a
    // stored loadout that fails. Since 2026-10-29 the rows below the strip wait for its points (one row gate, no level).
    void permanentTests()
    {
        using namespace ef::talents;
        EfTalentModel m;
        m.specLevel = 10;
        m.points[0] = {{1, {0, 0}}, {2, {1, 0}}, {3, {2, 0}}, {4, {3, 0}}, {10, {4, 1}}, {12, {5, 2}}};
        auto node = [](uint32_t id, uint32_t tree, uint32_t ord, NodeKind kind, uint32_t parent, uint32_t level) {
            EfNode n;
            n.id = id;
            n.tree = tree;
            n.ord = ord;
            n.kind = kind;
            n.requiredLevel = level;
            n.parent = parent;
            if (kind == NodeKind::Choice)
                n.options = {{id * 10 + 1}, {id * 10 + 2}};
            else
                n.ranks.push_back({id * 10 + 1});
            return n;
        };
        EfTree ct;
        ct.id = 1;
        ct.classId = 1;
        // the strip 11 -> 12 -> 13 (levels 2-4), a row ability 21 (a choice) and a stat talent 22, both behind the strip's
        // 3 points (the compiler's row gate, folded into the nodes)
        ct.nodes = {node(11, 1, 0, NodeKind::Passive, 0, 2), node(12, 1, 1, NodeKind::Active, 11, 3),
                    node(13, 1, 2, NodeKind::Passive, 12, 4), node(21, 1, 3, NodeKind::Choice, 0, 0),
                    node(22, 1, 4, NodeKind::Passive, 0, 0)};
        ct.nodes[3].gate = ct.nodes[4].gate = 3;
        m.trees[1] = ct;
        for (uint32_t const specId : {91u, 92u})
        {
            EfTree st;
            st.id = specId - 89; // trees 2 and 3
            st.classId = 1;
            st.specId = specId;
            st.nodes = {node(specId * 10, st.id, 0, NodeKind::Passive, 0, 0)}; // 910 / 920 (2 ranks, level 0)
            st.nodes[0].maxRank = 2;
            st.nodes[0].ranks.push_back({specId * 100 + 2});
            m.trees[st.id] = st;
            EfSpec spec;
            spec.id = specId;
            spec.classId = 1;
            spec.sort = specId - 90;
            m.specs[specId] = spec;
        }
        std::vector<std::string> problems;
        m.Finish(problems);
        check(problems.empty(), "permanent: model problems: " + (problems.empty() ? std::string() : problems[0]));
        check(m.Permanent(*m.Node(11)) && m.Permanent(*m.Node(13)), "permanent: the strip");
        check(!m.Permanent(*m.Node(21)) && !m.Permanent(*m.Node(22)), "permanent: not the rows below the strip (their gate)");
        check(!m.Permanent(*m.Node(910)), "permanent: no spec tree node, whatever its level");

        EfSpec const& a = m.specs[91];
        eft::Pairs const live = {{11, {1, 0}}, {12, {1, 0}}, {21, {1, 2}}, {910, {1, 0}}};
        eft::Pairs const kept = m.Kept(live);
        check(kept == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}}, "Kept: the strip's slots of a loadout");
        // APPLY / SAVE of the live loadout: no held slot goes
        RuleResult const lost = m.CheckKept(kept, {{11, {1, 0}}, {910, {2, 0}}});
        check(lost.code == RuleError::Permanent && lost.detail == 12, "CheckKept: a refunded strip slot (PERMANENT, the node)");
        check(m.CheckKept(kept, {{11, {1, 0}}, {12, {1, 0}}, {13, {1, 0}}}).ok(), "CheckKept: a row node may go, a slot be added");
        check(std::string(errorCode(RuleError::Permanent)) == "PERMANENT", "PERMANENT is the protocol's code");
        // SAVE to another loadout, LOAD, a spec switch: the strip goes in
        check(m.Overlay({{22, {1, 0}}}, kept) == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}, {22, {1, 0}}}, "Overlay");
        eft::Pairs const strip3 = {{11, {1, 0}}, {12, {1, 0}}, {13, {1, 0}}};
        auto with = m.WithKept(a, {{22, {1, 0}}, {910, {1, 0}}}, strip3, 12);
        check(with && *with == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}, {13, {1, 0}}, {22, {1, 0}}, {910, {1, 0}}},
              "WithKept: room for the strip");
        with = m.WithKept(a, {{22, {1, 0}}, {910, {1, 0}}}, kept, 12);
        check(with && *with == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}, {910, {1, 0}}},
              "WithKept: a row without the full strip goes (GATE)");
        // 4 class points at 10: the strip's 3 and two more do not fit
        with = m.WithKept(a, {{21, {1, 1}}, {22, {1, 0}}, {910, {1, 0}}}, strip3, 10);
        check(with && *with == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}, {13, {1, 0}}, {910, {1, 0}}},
              "WithKept: no room: the strip and the spec tree, the other class nodes go");
        with = m.WithKept(a, {{910, {2, 0}}}, strip3, 10);
        check(with && *with == strip3, "WithKept: then the strip alone");
        check(!m.WithKept(a, {}, strip3, 3), "WithKept: nothing below the strip's levels");
        check(m.WithKept(m.specs[92], {}, kept, 12) == std::optional<eft::Pairs>(kept), "WithKept: another spec's first loadout gets the strip");

        // 2026-10-29: the rows wait for the full strip, whatever the level; a loadout bought past an unfinished strip (the
        // level-10 rule of 2026-10-28) fails GATE, and the login's Revalidate refits it: the row points come back, the strip
        // slots and the spec tree's points stay
        eft::Pairs const skipped = {{11, {1, 0}}, {12, {1, 0}}, {21, {1, 1}}, {22, {1, 0}}, {910, {1, 0}}};
        RuleResult const gate = m.Validate(a, m.LoadoutString(a, skipped), 12);
        check(gate.code == RuleError::Gate && gate.detail == 3, "the rows behind the strip: GATE, the strip's 3 points");
        check(m.Validate(a, m.LoadoutString(a, {{11, {1, 0}}, {12, {1, 0}}, {13, {1, 0}}, {21, {1, 1}}, {22, {1, 0}}}), 12).ok(),
              "the rows once the strip is full");
        check(m.Refit(a, skipped, 12) == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}, {910, {1, 0}}},
              "Refit: the row points refunded, the strip and the spec tree kept");
        check(m.Refit(a, {{11, {1, 0}}, {12, {1, 0}}, {22, {1, 0}}, {910, {2, 0}}}, 10) == eft::Pairs{{11, {1, 0}}, {12, {1, 0}}},
              "Refit: the strip alone when the spec tree no longer fits either");
        check(m.Refit(a, skipped, 2).empty(), "Refit: nothing when even the strip fails (a level below it)");
    }
}

namespace
{
    // A spec tree's granted rows on the first bar (Emberfall 2026-11-20, #655's open item: a new Marksman knew Full Draw,
    // granted with `auto: [0]`, but found it on no button). EfTalentModel::GrantedAbilities names a granted ACTIVE node's
    // spells (not a replacing one's: it takes its replaced spell's button; not a passive's, not a pick's), and FirstBars
    // puts each into a free button of the first bar, never over a button the player placed and never twice.
    void grantedTests()
    {
        using namespace ef::talents;
        EfTalentModel m;
        m.points[0] = {{1, {0, 0}}, {10, {0, 1}}, {14, {0, 5}}};
        auto node = [](uint32_t id, uint32_t tree, uint32_t ord, NodeKind kind) {
            EfNode n;
            n.id = id;
            n.tree = tree;
            n.ord = ord;
            n.kind = kind;
            n.requiredLevel = tree == 2 ? 10 : 0; // the spec's rows from the spec choice
            n.ranks.push_back({id * 10 + 1});
            return n;
        };
        EfTree ct;
        ct.id = 1;
        ct.classId = 1;
        ct.nodes = {node(11, 1, 0, NodeKind::Passive)};
        m.trees[1] = ct;
        EfTree st;
        st.id = 2;
        st.classId = 1;
        st.specId = 91;
        // 41 a granted replacing builder, 42 a granted spender (Full Draw), 43 a granted passive at 4 points, 44 a pick,
        // 45 a granted node whose spell is passive, 46 a granted active at 4 points spent
        st.nodes = {node(41, 2, 0, NodeKind::Active), node(42, 2, 1, NodeKind::Active), node(43, 2, 2, NodeKind::Passive),
                    node(44, 2, 3, NodeKind::Active), node(45, 2, 4, NodeKind::Active), node(46, 2, 5, NodeKind::Active),
                    node(47, 2, 6, NodeKind::Passive)};
        st.nodes[0].autoAt = {0};
        st.nodes[0].replaces = 999;
        st.nodes[1].autoAt = {0};
        st.nodes[2].autoAt = {4};
        st.nodes[3].group = 1;
        st.nodes[4].autoAt = {0};
        st.nodes[5].autoAt = {4};
        st.nodes[6].maxRank = 5;
        st.nodes[6].ranks = {{471}, {472}, {473}, {474}, {475}};
        st.groups = {RuleGroup{true, {1}}};
        m.trees[2] = st;
        EfSpec spec;
        spec.id = 91;
        spec.classId = 1;
        spec.sort = 1;
        spec.treeId = 2;
        m.specs[91] = spec;
        std::vector<std::string> problems;
        m.Finish(problems);
        check(problems.empty(), "granted: model problems: " + (problems.empty() ? std::string() : problems[0]));
        auto passive = [](uint32_t spell) { return spell == 451; };
        EfSpec const& s = m.specs[91];
        check(m.GrantedAbilities(s, {}, 10, passive) == std::vector<uint32_t>{421},
              "granted: the granted active rows' abilities (no replacing node, no passive, no pick, none not reached yet)");
        check(m.GrantedAbilities(s, {{44, {1, 0}}}, 10, passive) == std::vector<uint32_t>{421}, "granted: a pick is no granted row");
        check(m.GrantedAbilities(s, {{47, {4, 0}}}, 14, passive) == (std::vector<uint32_t>{421, 461}),
              "granted: a row granted at 4 points spent comes with them");
        check(m.GrantedAbilities(s, {}, 9, passive).empty(), "granted: nothing below the spec's rows' level");

        // the first spec choice: the baseline, then the granted rows, each into a free button of the first bar
        auto known = [](uint32_t) { return true; };
        std::vector<BarButton> const placed{{0, 7001, 0}, {1, 7002, 0}, {3, 5, 0x80}};
        check(FirstBars(placed, known, {7101, 421}) == (std::vector<BarButton>{{0, 7001, 0}, {1, 7002, 0}, {2, 7101, 0}, {3, 5, 0x80}, {4, 421, 0}}),
              "granted: into the free buttons, the player's buttons kept");
        check(FirstBars({{0, 7001, 0}, {60, 421, 0}}, known, {421}) == (std::vector<BarButton>{{0, 7001, 0}, {60, 421, 0}}),
              "granted: a spell on any button already is not placed again");
        std::vector<BarButton> full;
        for (uint32_t b = 0; b < kFirstBarButtons; ++b)
            full.push_back({b, 8000 + b, 0});
        check(FirstBars(full, known, {421}) == full, "granted: a full first bar keeps every button (the spell stays in the book)");
        std::cout << "granted rows: the first bar\n";
    }

    // #655's data change (2026-11-19) on a Marksman-shaped tree: Full Draw (3203) granted with the spec, Set On (3204) a
    // pick of group 1 in its place (Set On granted and Full Draw that pick before). What a login's Revalidate does to the
    // loadouts stored before (EfTalentModel::Remap, then Validate, Refit only when that fails), as small jobs 20 checked
    // live: a level-12 Marksman who picked Full Draw and bought Broadhead, and the spec bench's level-45 plan (Full Draw,
    // Arrowfall, Tangle Trap and Held Breath picked, Set On's row bought).
    void marksmanRemapTests()
    {
        using namespace ef::talents;
        EfTalentModel m;
        // the Hunter's schedule: class 2-9, spec 10, then class on the odd and spec on the even levels until 22 class
        // points (37), then spec only: 22 + 37 at 60
        uint32_t c = 0, sp = 0;
        for (uint32_t level = 1; level <= 60; ++level)
        {
            if (level >= 2 && level <= 9)
                ++c;
            else if (level == 10)
                ++sp;
            else if (level > 10 && level % 2 == 1 && c < 22)
                ++c;
            else if (level > 10)
                ++sp;
            m.points[0][level] = {c, sp};
        }
        check(m.points[0][12] == std::pair<uint32_t, uint32_t>{9, 2} && m.points[0][45] == std::pair<uint32_t, uint32_t>{22, 22} &&
              m.points[0][60] == std::pair<uint32_t, uint32_t>{22, 37}, "marksman remap: the Hunter's point schedule");
        auto node = [](uint32_t id, uint32_t tree, NodeKind kind, uint32_t parent, uint32_t ranks = 1) {
            EfNode n;
            n.id = id;
            n.tree = tree;
            n.kind = kind;
            n.parent = parent;
            n.maxRank = ranks;
            for (uint32_t r = 1; r <= ranks; ++r)
                n.ranks.push_back({id * 10 + r});
            return n;
        };
        EfTree ct;
        ct.id = 301;
        ct.classId = 3;
        for (uint32_t i = 0; i < 8; ++i) // the strip, levels 2-9, each opened by the one before
        {
            ct.nodes.push_back(node(3030 + i, 301, i == 1 ? NodeKind::Active : NodeKind::Passive, i ? 3029 + i : 0));
            ct.nodes.back().requiredLevel = 2 + i;
            ct.nodes.back().ord = i;
        }
        EfTree st;
        st.id = 303;
        st.classId = 3;
        st.specId = 32;
        auto row = [&](std::vector<std::pair<uint32_t, uint32_t>> const& nodes) { // (id, ranks); the first is the ability
            for (size_t i = 0; i < nodes.size(); ++i)
                st.nodes.push_back(node(nodes[i].first, 303, i ? NodeKind::Passive : NodeKind::Active, i ? nodes[i - 1].first : 0,
                                        nodes[i].second));
        };
        row({{3201, 1}, {3239, 1}, {3208, 2}, {3214, 1}, {3234, 1}});   // Patient Shot (granted, replaces)
        row({{3202, 1}, {3220, 1}, {3209, 2}, {3210, 1}, {3235, 1}});   // Hard Draw (granted, replaces)
        row({{3203, 1}, {3213, 1}, {3216, 2}, {3212, 1}, {3229, 1}});   // Full Draw (granted since #655)
        row({{3204, 1}, {3211, 1}, {3231, 2}, {3225, 1}, {3222, 1}});   // Set On (a pick since #655)
        row({{3206, 1}, {3226, 1}, {3223, 2}, {3219, 1}, {3215, 1}});   // Arrowfall (a pick)
        row({{3218, 1}, {3221, 1}, {3236, 1}, {3237, 1}});              // Tangle Trap (a pick)
        row({{3224, 1}, {3230, 1}, {3227, 2}, {3228, 1}});              // Held Breath (group 2)
        st.nodes.push_back(node(3205, 303, NodeKind::Passive, 0));      // Steady Eye, granted at 4 points
        for (uint32_t i = 0; i < st.nodes.size(); ++i)
            st.nodes[i].ord = i;
        auto at = [&](uint32_t id) -> EfNode& { return *std::find_if(st.nodes.begin(), st.nodes.end(), [&](EfNode const& n) { return n.id == id; }); };
        for (uint32_t const id : {3201u, 3202u, 3203u})
            at(id).autoAt = {0};
        at(3201).replaces = 93001;
        at(3202).replaces = 93002;
        at(3205).autoAt = {4};
        for (uint32_t const id : {3204u, 3206u, 3218u})
            at(id).group = 1;
        at(3224).group = 2;
        st.groups = {RuleGroup{true, {1, 6, 11}}, RuleGroup{true, {21}}};
        m.trees[301] = ct;
        m.trees[303] = st;
        EfSpec spec;
        spec.id = 32;
        spec.classId = 3;
        spec.sort = 2;
        spec.treeId = 303;
        m.specs[32] = spec;
        std::vector<std::string> problems;
        m.Finish(problems);
        check(problems.empty(), "marksman remap: model problems: " + (problems.empty() ? std::string() : problems[0]));
        EfSpec const& s = m.specs[32];
        eft::Pairs strip;
        for (uint32_t i = 0; i < 8; ++i)
            strip[3030 + i] = {1, 0};
        auto with = [&](eft::Pairs p) { p.insert(strip.begin(), strip.end()); return p; };
        auto knows = [&](eft::Pairs const& p, uint32_t level, uint32_t node) { return m.Expected(s, p, level).count(node * 10 + 1) != 0; };

        // level 12: Full Draw picked at 10, Broadhead bought at 12 (Set On was granted, never stored)
        bool changed = false;
        eft::Pairs const old12 = with({{3203, {1, 0}}, {3213, {1, 0}}});
        eft::Pairs const new12 = m.Remap(s, old12, changed);
        check(changed && !new12.count(3203) && new12.count(3213) && !new12.count(3204),
              "marksman remap 12: the Full Draw pick goes (granted now), Broadhead stays, Set On is not picked (its row empty)");
        check(m.Validate(s, m.LoadoutString(s, new12), 12).ok(), "marksman remap 12: valid as remapped (no Refit)");
        check(m.Refit(s, new12, 12) == new12, "marksman remap 12: Refit keeps a valid loadout as it is");
        check(m.PaidIn(m.trees[303], new12) == 1, "marksman remap 12: one of the 2 spec points comes back (the pick's)");
        check(knows(new12, 12, 3203) && !knows(new12, 12, 3204), "marksman remap 12: knows Full Draw, not Set On");
        eft::Pairs then12 = new12;
        then12[3204] = {1, 0};
        check(m.Validate(s, m.LoadoutString(s, then12), 12).ok(), "marksman remap 12: the point back buys Set On in group 1's open slot");

        // level 45, the bench plan: picks Full Draw, Arrowfall, Tangle Trap, Held Breath; Set On's row bought; 22 of 22
        eft::Pairs const old45 = with({{3203, {1, 0}}, {3206, {1, 0}}, {3218, {1, 0}}, {3224, {1, 0}},
                                       {3211, {1, 0}}, {3231, {2, 0}}, {3225, {1, 0}}, {3222, {1, 0}},
                                       {3239, {1, 0}}, {3208, {2, 0}}, {3214, {1, 0}}, {3234, {1, 0}},
                                       {3220, {1, 0}}, {3209, {2, 0}}, {3210, {1, 0}}, {3235, {1, 0}},
                                       {3213, {1, 0}}, {3216, {2, 0}}});
        uint32_t stored = 0; // the old data's paid points: every stored spec node (Full Draw a paid pick then)
        for (auto const& [id, p] : old45)
            if (id >= 3200)
                stored += p.rank;
        check(stored == 22, "marksman remap 45: the old loadout spends all 22 spec points");
        eft::Pairs const new45 = m.Remap(s, old45, changed);
        check(changed && !new45.count(3203) && new45.count(3204) && new45.count(3206) && new45.count(3218) && new45.count(3224),
              "marksman remap 45: Full Draw's pick goes, Set On is picked (its row is held), the other picks stay");
        check(m.Validate(s, m.LoadoutString(s, new45), 45).ok(), "marksman remap 45: the three group 1 picks fit their slots");
        check(m.PaidIn(m.trees[303], new45) == 22, "marksman remap 45: nothing refunded, 22 of 22 spent");
        check(knows(new45, 45, 3203) && knows(new45, 45, 3204) && knows(new45, 45, 3206) && knows(new45, 45, 3218) && knows(new45, 45, 3224),
              "marksman remap 45: every ability known");

        // in between (level 20): Full Draw and Arrowfall picked, Set On's row not bought: Full Draw's point comes back
        eft::Pairs const new20 = m.Remap(s, with({{3203, {1, 0}}, {3206, {1, 0}}, {3213, {1, 0}}}), changed);
        check(changed && !new20.count(3203) && !new20.count(3204) && new20.count(3206) && m.Validate(s, m.LoadoutString(s, new20), 20).ok(),
              "marksman remap 20: the pick's point comes back, a slot of group 1 opens");
        std::cout << "marksman remap: #655's loadouts at 12, 20 and 45\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        for (int i = 1; i < argc; ++i)
            runVectors(argv[i]);
        protocolTests();
        loadoutTests();
        stripTests();
        permanentTests();
        grantedTests();
        marksmanRemapTests();
        fuzz();
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL: exception: " << e.what() << "\n";
        return 2;
    }
    std::cout << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
