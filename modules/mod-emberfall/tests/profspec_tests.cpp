// Standalone tests of mod-emberfall's profession specialisation model (src/profspecs/EfProfModel.h): no core, no
// database. Built and run by tests/run_profspec_tests.sh.

#include "EfProfModel.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ef::prof;

static int s_failed = 0;
#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++s_failed;                                                             \
        }                                                                           \
    } while (0)

static Node MakeNode(uint32_t id, uint32_t tree, uint32_t ord, uint32_t row, uint32_t ranks, uint32_t gate, std::vector<uint32_t> parents,
                     Effect effect, uint32_t perRank, bool active, uint32_t& spell)
{
    Node n;
    n.id = id;
    n.tree = tree;
    n.ord = ord;
    n.row = row;
    n.maxRank = ranks;
    n.gate = gate;
    n.parents = std::move(parents);
    n.effect = effect;
    n.perRank = perRank;
    n.active = active;
    for (uint32_t r = 0; r < ranks; ++r)
        n.spells.push_back(spell++);
    return n;
}

// Mining's shape (professions/specs.yaml): Rich Seams' first rows, a Prospecting chain, an empty third tree.
static Model MakeModel()
{
    Model m;
    m.hash = "0011223344556677";
    m.pointsPerSkill = 10;
    m.feePerPoint = 25;
    uint32_t spell = 97001;
    Tree a;
    a.id = 1, a.skill = 186, a.sort = 1, a.name = "Rich Seams";
    a.nodes.push_back(MakeNode(101, 1, 0, 1, 3, 0, {}, Effect::Yield, 10, true, spell));
    a.nodes.push_back(MakeNode(102, 1, 1, 2, 3, 3, {101}, Effect::Yield, 10, false, spell));
    a.nodes.push_back(MakeNode(103, 1, 2, 2, 3, 3, {101}, Effect::Yield, 10, false, spell));
    a.nodes.push_back(MakeNode(106, 1, 3, 3, 11, 9, {103}, Effect::Yield, 10, false, spell));
    a.nodes[0].items = {90611, 90612};
    Tree b;
    b.id = 3, b.skill = 186, b.sort = 2, b.name = "Prospecting";
    b.nodes.push_back(MakeNode(301, 3, 0, 1, 3, 0, {}, Effect::MinimapRange, 20, true, spell));
    b.nodes.push_back(MakeNode(302, 3, 1, 2, 6, 3, {301}, Effect::RarestFirst, 15, true, spell));
    Tree c;
    c.id = 2, c.skill = 186, c.sort = 3, c.name = "Deep Reading";
    c.nodes.push_back(MakeNode(201, 2, 0, 1, 3, 0, {}, Effect::None, 5, false, spell));
    Tree d;
    d.id = 7, d.skill = 171, d.sort = 1, d.name = "The Steady Hand";
    d.nodes.push_back(MakeNode(701, 7, 0, 1, 3, 0, {}, Effect::CraftExtra, 5, true, spell));
    d.nodes[0].families = {1};
    m.trees = {{1, a}, {3, b}, {2, c}, {7, d}};
    m.recipes[90661] = {171, 1, 90631};
    m.Index();
    return m;
}

int main()
{
    Model const m = MakeModel();
    CHECK(m.Problems().empty());
    CHECK(m.TreesOf(186).size() == 3);
    CHECK(m.TreesOf(186)[0]->id == 1 && m.TreesOf(186)[1]->id == 3 && m.TreesOf(186)[2]->id == 2);
    CHECK(m.Budget(75) == 7 && m.Budget(9) == 0 && m.Budget(500) == 50);

    // the wire: base 36, a tree per part
    CHECK(m.Encode(186, {}) == "0000.00.0");
    Ranks r{{101, 3}, {103, 2}, {302, 11}, {106, 11}};
    CHECK(m.Encode(186, r) == "302b.0b.0");
    auto decoded = m.Decode(186, "302b.0b.0");
    CHECK(decoded && *decoded == Ranks({{101, 3}, {103, 2}, {106, 11}, {302, 11}}));
    CHECK(!m.Decode(186, "3002.00"));          // a tree missing
    CHECK(!m.Decode(186, "3002.00.0.0"));      // one too many
    CHECK(!m.Decode(186, "30A2.00.0"));        // upper case is not a digit
    CHECK(!m.Decode(186, "300.00.0"));         // a tree too short
    CHECK(!m.Decode(999, "0"));                // no trees

    // the rules
    CHECK(m.Validate(186, {{101, 3}}, 75).ok());
    CHECK(m.Validate(186, {{101, 3}, {103, 3}, {301, 1}}, 75).ok());              // 7 points at 75
    CHECK(m.Validate(186, {{101, 3}, {103, 3}, {301, 2}}, 75).code == "BUDGET");  // 8 points
    CHECK(m.Validate(186, {{101, 2}, {103, 1}}, 75).code == "PATH");              // the parent is not full
    CHECK(m.Validate(186, {{101, 4}}, 500).code == "RANK");
    CHECK(m.Validate(186, {{101, 3}, {102, 3}, {103, 3}, {106, 1}}, 500).ok());   // row 3 opens at 9
    CHECK(m.Validate(186, {{101, 3}, {103, 3}, {106, 1}}, 500).code == "GATE");   // only 6 above
    CHECK(m.Validate(186, {{301, 3}, {302, 1}}, 75).ok());
    CHECK(m.Validate(186, {{701, 1}}, 75).code == "DECODE");                       // a node of another profession
    CHECK(m.Validate(182, {}, 75).code == "SKILL");
    Ranks stored{{101, 3}, {301, 1}};
    CHECK(m.Validate(186, {{101, 3}, {301, 2}}, 75, &stored).ok());
    CHECK(m.Validate(186, {{101, 3}}, 75, &stored).code == "LOWER");               // ranks never go down
    CHECK(m.Validate(186, {{101, 3}}, 75, &stored).detail == 301);

    // effects: only nodes that work today count, per_rank x rank
    Ranks e{{101, 2}, {102, 3}, {301, 3}, {302, 2}, {701, 3}};
    CHECK(m.Sum(e, Effect::Yield, 186, [](Node const& n) { return n.items.count(90611) != 0; }) == 20);
    CHECK(m.Sum(e, Effect::Yield, 186, [](Node const& n) { return n.items.count(90601) != 0; }) == 0);
    CHECK(m.Sum(e, Effect::Yield) == 20);          // node 102 waits for later content
    CHECK(m.Sum(e, Effect::MinimapRange, 186) == 60);
    CHECK(m.Sum(e, Effect::MinimapRange, 182) == 0);
    CHECK(m.Sum(e, Effect::RarestFirst) == 30);
    CHECK(m.Sum(e, Effect::CraftExtra, 171, [](Node const& n) { return n.families.count(1) != 0; }) == 15);
    CHECK(m.Expected({{101, 2}, {301, 3}}) == std::set<uint32_t>({97002, m.NodeById(301)->spells[2]}));

    // resets and data changes
    Ranks w{{101, 3}, {103, 2}, {301, 1}};
    CHECK(m.WipeTree(1, w) == 5 && w == Ranks({{301, 1}}));
    Ranks old{{101, 9}, {999, 2}, {301, 0}};
    CHECK(m.Remap(old) && old == Ranks({{101, 3}}));
    CHECK(m.Spent(186, {{101, 3}, {701, 2}}) == 3 && m.SpentInTree(7, {{101, 3}, {701, 2}}) == 2);
    CHECK(Digit(35) == 'z' && DigitValue('z') == 35 && DigitValue('9') == 9 && DigitValue('-') == -1);

    if (s_failed)
    {
        std::fprintf(stderr, "profspec tests: %d failed\n", s_failed);
        return 1;
    }
    std::printf("profspec tests: ok\n");
    return 0;
}
