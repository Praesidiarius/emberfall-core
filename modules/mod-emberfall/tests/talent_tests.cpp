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
//   1. the rules vectors (Emberfall's tools/py/tests/data/talent_rules/*.json, a copy in tests/data/) through
//      the module's copy of TalentRules.h: every case's first error and detail, as the client's Catch2 test does;
//   2. EfTalentModel over the vectors' trees: wire strings <-> stored pairs round trips, Remap;
//   3. the EFT protocol (EftProtocol.h): parse, chunking both ways, names, stored pairs, rate limit; and a
//      decode fuzz: random and mutated bodies never crash and always give a defined result.
//
//   talent_tests VECTORS.json...    exit status 0 when everything passed

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

    RuleTree ruleTree(Json const& t)
    {
        RuleTree tree;
        for (Json const& n : t["nodes"].arr())
        {
            RuleNode r;
            r.id = static_cast<uint32_t>(n["id"].num());
            r.row = static_cast<uint32_t>(n["row"].num());
            r.kind = static_cast<NodeKind>(n["kind"].num());
            r.maxRank = static_cast<uint8_t>(n["maxRank"].num());
            r.numOptions = static_cast<uint8_t>(n["numOptions"].num());
            r.requiredLevel = static_cast<uint32_t>(n["requiredLevel"].num());
            r.freeSpecMask = static_cast<uint32_t>(n["freeSpecMask"].num());
            size_t k = 0;
            for (Json const& p : n["parents"].arr())
                if (k < kMaxParents)
                    r.parents[k++] = static_cast<int32_t>(p.num());
            tree.nodes.push_back(r);
        }
        for (Json const& g : t["gates"].arr())
            tree.gates.push_back(RuleGate{static_cast<uint32_t>(g["row"].num()), static_cast<uint32_t>(g["points"].num())});
        return tree;
    }

    // An EfTalentModel of one class (1) with one spec (sort 1 or 2 per the case) over the vectors' trees; node
    // spells are made up (node ID x 10 + rank / option).
    std::unique_ptr<EfTalentModel> model(Json const& classTree, Json const& specTree, uint32_t specSort)
    {
        auto m = std::make_unique<EfTalentModel>();
        EfSpec spec;
        spec.id = 91;
        spec.classId = 1;
        spec.sort = specSort;
        spec.baseline = {{91901, 10}, {91902, 20}};
        m->specs[spec.id] = spec;
        // The vectors' free node is free for the spec of sort 1; a case in the spec of sort 2 gets that one as
        // another spec (with an empty tree of its own).
        uint32_t const freeSpec = specSort == 1 ? 91 : 92;
        if (specSort != 1)
        {
            EfSpec other;
            other.id = 92;
            other.classId = 1;
            other.sort = 1;
            m->specs[other.id] = other;
            EfTree empty;
            empty.id = 999;
            empty.classId = 1;
            empty.specId = 92;
            m->trees[empty.id] = empty;
        }
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
                e.row = static_cast<uint32_t>(n["row"].num());
                e.kind = static_cast<NodeKind>(n["kind"].num());
                e.maxRank = static_cast<uint32_t>(n["maxRank"].num());
                e.requiredLevel = static_cast<uint32_t>(n["requiredLevel"].num());
                for (Json const& p : n["parents"].arr())
                    e.parents.push_back(ids[static_cast<size_t>(p.num())]);
                if (e.kind == NodeKind::Choice)
                    for (long long o = 1; o <= n["numOptions"].num(); ++o)
                        e.options.push_back({e.id * 10 + static_cast<uint32_t>(o)});
                else
                    for (uint32_t r = 1; r <= e.maxRank; ++r)
                        e.ranks.push_back({e.id * 10 + r});
                if (n["freeSpecMask"].num() & 1)
                    e.freeSpecs.push_back(freeSpec);
                t.nodes.push_back(e);
            }
            for (Json const& g : (*json)["gates"].arr())
                t.gates.push_back(RuleGate{static_cast<uint32_t>(g["row"].num()), static_cast<uint32_t>(g["points"].num())});
            m->trees[t.id] = t;
        }
        return m;
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
                                                 static_cast<uint32_t>(c["classBudget"].num()), static_cast<uint32_t>(c["specBudget"].num()),
                                                 static_cast<uint32_t>(c["specBit"].num()));
            check(errorCode(r.code) == c["code"].str() && r.detail == static_cast<uint32_t>(c["detail"].num()),
                  path + ": " + c["name"].str() + ": expected " + c["code"].str() + " " + std::to_string(c["detail"].num()) + ", got " +
                      errorCode(r.code) + " " + std::to_string(r.detail));

            // The model gives the same answer, and a valid loadout survives wire -> pairs -> wire.
            uint32_t const sort = c["specBit"].num() == 2 ? 2 : 1;
            auto m = model(treeJson.at(c["classTree"].num()), treeJson.at(c["specTree"].num()), sort);
            m->points[0][1] = {static_cast<uint32_t>(c["classBudget"].num()), static_cast<uint32_t>(c["specBudget"].num())};
            std::vector<std::string> problems;
            m->Finish(problems);
            check(problems.empty(), c["name"].str() + ": model problems: " + (problems.empty() ? "" : problems[0]));
            EfSpec const& spec = *m->Spec(91);
            RuleResult const mr = m->Validate(spec, c["loadout"].str(), static_cast<uint32_t>(c["level"].num()));
            check(mr.code == r.code && mr.detail == r.detail, c["name"].str() + ": the model's Validate differs");
            if (r.ok())
            {
                auto pairs = m->PairsOf(spec, c["loadout"].str());
                check(pairs.has_value(), c["name"].str() + ": PairsOf");
                if (pairs)
                {
                    // A free node given explicitly reads back as given; compare through the pairs.
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

        // Remap after a data change: a node removed, a max rank lowered, an option removed.
        auto m = model(treeJson.begin()->second, std::next(treeJson.begin())->second, 1);
        m->points[0][1] = {99, 99};
        std::vector<std::string> problems;
        m->Finish(problems);
        eft::Pairs const pairs = {{9002, {2, 0}}, {9101, {2, 0}}, {9104, {1, 2}}, {12345, {1, 0}}};
        bool changed = false;
        eft::Pairs const kept = m->Remap(*m->Spec(91), pairs, changed);
        check(changed && !kept.count(12345) && kept.size() == 3, "Remap drops an unknown node");
        m->trees[811].nodes[0].maxRank = 1;
        m->trees[811].nodes[0].ranks.resize(1);
        m->trees[811].nodes[3].options.resize(1);
        m->Finish(problems); // the choice node has one option now: a problem, but Remap still works
        eft::Pairs const kept2 = m->Remap(*m->Spec(91), pairs, changed);
        check(changed && kept2.at(9101).rank == 1 && !kept2.count(9104), "Remap clamps ranks and drops a lost option");
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
        for (char const* bad : {"1", "1:", ":1", "1:0", "1:d", "1:2,1:3", "0:1", "1:22", "x:1", "1:1,"})
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
            n.row = 1 + k / 2;
            n.kind = k == 4 ? NodeKind::Choice : NodeKind::Passive;
            n.maxRank = k == 4 ? 1 : 3;
            n.numOptions = k == 4 ? 2 : 0;
            if (k >= 2)
                n.parents[0] = static_cast<int32_t>(k - 2);
            tree.nodes.push_back(n);
        }
        tree.gates.push_back(RuleGate{3, 4});
        std::string const chars = "0123456789abcd.x";
        for (int i = 0; i < 100000; ++i)
        {
            std::string s;
            size_t const n = rng() % 16;
            for (size_t k = 0; k < n; ++k)
                s += chars[rng() % chars.size()];
            RuleResult const r = validateLoadout(tree, tree, s, rng() % 90, rng() % 20, rng() % 20, 1u << (rng() % 3));
            check(r.code <= RuleError::BudgetSpec, "fuzz: rules result in range");
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
        for (int i = 1; i < argc; ++i)
            runVectors(argv[i]);
        protocolTests();
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
