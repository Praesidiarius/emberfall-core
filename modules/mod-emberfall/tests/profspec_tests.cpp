// Standalone tests of mod-emberfall's profession specialisation model (src/profspecs/EfProfModel.h): no core, no
// database. Built and run by tests/run_profspec_tests.sh.

#include "EfProfModel.h"
#include "EfProfText.h"

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

// The other professions' shapes (feedback #204): Alchemy's Double Batch next to Tailoring's Whole Bolt, Tailoring's
// refunds, Enchanting's chalk upgrade and proc glyph potency, First Aid's bandage potency and cleanse, a repair discount.
static Model MakeOtherModel()
{
    Model m;
    m.hash = "8899aabbccddeeff";
    uint32_t spell = 98001;
    auto tree = [&](uint32_t id, uint32_t skill, std::vector<Node> nodes) {
        Tree t;
        t.id = id, t.skill = skill, t.sort = 1, t.name = "tree " + std::to_string(id);
        for (auto const& [other, o] : m.trees)
            t.sort += o.skill == skill ? 1 : 0;   // 1..n within the profession
        t.nodes = std::move(nodes);
        m.trees[id] = t;
    };
    Node batch = MakeNode(705, 7, 0, 1, 7, 0, {}, Effect::CraftBatch, 1, true, spell);
    batch.extra = 4;
    tree(7, 171, {batch});
    Node bolts = MakeNode(1602, 16, 0, 1, 3, 0, {}, Effect::CraftRefund, 10, true, spell);
    bolts.families = {6};
    bolts.items = {99470, 99471};
    Node pieces = MakeNode(1604, 16, 1, 2, 8, 3, {1602}, Effect::CraftRefund, 4, true, spell);
    pieces.families = {7, 12};
    Node whole = MakeNode(1607, 16, 2, 3, 7, 9, {1604}, Effect::CraftBatch, 1, true, spell);
    whole.families = {6};
    whole.extra = 2;
    tree(16, 197, {bolts, pieces, whole});
    Node chalk = MakeNode(1904, 19, 0, 1, 8, 0, {}, Effect::Upgrade, 2, true, spell);
    chalk.items = {93230};
    chalk.extra = 1;
    tree(19, 333, {chalk});
    Node flame = MakeNode(2101, 21, 0, 1, 3, 0, {}, Effect::Potency, 5, true, spell);
    flame.targetSpells = {100200, 100201};
    tree(21, 333, {flame});
    Node firm = MakeNode(3101, 31, 0, 1, 3, 0, {}, Effect::Potency, 5, true, spell);
    firm.families = {16};
    Node clean = MakeNode(3102, 31, 1, 2, 6, 3, {3101}, Effect::Cleanse, 5, true, spell);
    clean.families = {16};
    tree(31, 129, {firm, clean});
    Node pressure = MakeNode(3201, 32, 0, 1, 3, 0, {}, Effect::Potency, 10, true, spell);
    pressure.families = {16};
    pressure.extra = 35;
    tree(32, 129, {pressure});
    tree(12, 164, {MakeNode(1201, 12, 0, 1, 35, 0, {}, Effect::RepairDiscount, 3, true, spell)});
    m.Index();
    return m;
}

int main()
{
    Model const m = MakeModel();
    CHECK(m.Problems().empty());
    CHECK(m.TreesOf(186).size() == 3);
    CHECK(m.TreesOf(186)[0]->id == 1 && m.TreesOf(186)[1]->id == 3 && m.TreesOf(186)[2]->id == 2);
    CHECK(m.Budget(186, 75) == 7 && m.Budget(186, 9) == 0 && m.Budget(186, 500) == 50);   // no floor named: every 10 skill

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

    // the login check: a character's stored ranks hold every profession's nodes, each profession is checked on its
    // own (Validate of the whole set answered DECODE, and the login wiped Alchemy's ranks next to Mining's)
    Ranks both{{101, 3}, {301, 1}, {701, 3}};
    CHECK(m.Of(171, both) == Ranks({{701, 3}}) && m.Of(186, both) == Ranks({{101, 3}, {301, 1}}));
    CHECK(m.Validate(171, both, 75).code == "DECODE");
    CHECK(m.ValidateStored(171, both, 75).ok());
    CHECK(m.ValidateStored(186, both, 75).ok());
    CHECK(m.ValidateStored(171, both, 29).code == "BUDGET");   // its own budget still counts
    CHECK(m.ValidateStored(186, both, 39).code == "BUDGET");   // 4 points need 40

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

    // the other professions' effects (feedback #204, professions.md §10.11)
    Model const t = MakeOtherModel();
    CHECK(t.Problems().empty());
    // craft batches count only the profession's own nodes and the recipe's family (an alchemist's five brews never
    // make five robes, a tailor's bolts never batch a robe)
    Ranks b{{705, 2}, {1607, 3}};
    CHECK(t.Batch(b, 171, 1) == std::make_pair(2u, 4u));
    CHECK(t.Batch(b, 197, 6) == std::make_pair(3u, 2u));          // bolts (part)
    CHECK(t.Batch(b, 197, 7) == std::make_pair(0u, 0u));          // armour: the node names part only
    // node by node: a refund names its own reagents, an upgrade its own better item
    Ranks rr{{1602, 2}, {1604, 4}, {1904, 3}};
    auto refunds = t.Invested(rr, Effect::CraftRefund, 197, [](Node const& n) { return Model::TakesFamily(n, 7); });
    CHECK(refunds.size() == 1 && refunds[0].first->id == 1604 && refunds[0].second == 16);
    auto bolts = t.Invested(rr, Effect::CraftRefund, 197, [](Node const& n) { return Model::TakesFamily(n, 6); });
    CHECK(bolts.size() == 1 && bolts[0].first->id == 1602 && bolts[0].second == 20);
    auto silver = t.Invested(rr, Effect::Upgrade, 333, [](Node const& n) { return n.items.count(93230) != 0; });
    CHECK(silver.size() == 1 && silver[0].second == 6 && silver[0].first->extra == 1);
    CHECK(t.Invested(rr, Effect::Upgrade, 197, [](Node const&) { return true; }).empty());   // another profession's
    // potency: the named spells or the item spells of the families; a threshold only below that health
    Ranks p{{3101, 3}, {3201, 2}, {2101, 1}};
    CHECK(t.Potency(p, 99470, 16, 80.0f) == 15);                  // a bandage, the target at 80 %: Firm Hands only
    CHECK(t.Potency(p, 99470, 16, 30.0f) == 35);                  // under 35 %: Pressure too
    CHECK(t.Potency(p, 100200, 0, 50.0f) == 5);                   // a flame glyph's hit
    CHECK(t.Potency(p, 12345, 0, 10.0f) == 0);
    CHECK(Model::Raise(64, 35) == 86 && Model::Raise(10, 0) == 10);
    CHECK(t.Sum(p, Effect::Cleanse) == 0);
    CHECK(t.Sum({{3102, 4}}, Effect::Cleanse, 0, [](Node const& n) { return n.families.count(16) != 0; }) == 20);
    // repairs: the discounts summed, never below nothing
    CHECK(t.RepairFactor({}) == 1.0f);
    CHECK(t.RepairFactor({{1201, 3}}) > 0.909f && t.RepairFactor({{1201, 3}}) < 0.911f);
    CHECK(t.RepairFactor({{1201, 35}}) == 0.0f);

    // the profession rework (professions.md §28): talent points only above rank 2's cap, per profession
    {
        Model f = MakeModel();
        f.pointsFrom = {{186, 175}, {185, 150}};
        f.stepOf = {{185, 5}};
        CHECK(f.Budget(186, 75) == 0 && f.Budget(186, 175) == 0 && f.Budget(186, 184) == 0 && f.Budget(186, 185) == 1);
        CHECK(f.Budget(186, 325) == 15 && f.Budget(186, 500) == 32);     // a tree and a half of 21
        CHECK(f.Budget(185, 150) == 0 && f.Budget(185, 300) == 30);      // Cooking and First Aid: one per 5 above 150
        CHECK(f.Budget(171, 100) == 10);                                  // a profession not named: as before
        CHECK(f.Validate(186, {{101, 1}}, 175).code == "BUDGET");        // nothing to spend below rank 3
        CHECK(f.Validate(186, {{101, 1}}, 190).ok());
        // the specialisations: the pick needs the last rank, the unlearn ladder's last step repeats
        Specialisation w{97901, 164, 1, 325, 500, "weaponsmith"};
        f.specs[w.spell] = w;
        f.specs[97902] = {97902, 164, 2, 325, 500, "armoursmith"};
        f.specs[97903] = {97903, 171, 1, 325, 500, "potions"};
        f.unlearnCopper = {10000, 100000, 1000000, 10000000, 100000000};
        CHECK(f.Problems().empty());
        CHECK(f.SpecsOf(164).size() == 2 && f.SpecsOf(164)[0]->key == "weaponsmith" && f.SpecsOf(171).size() == 1);
        CHECK(f.Spec(97902) && f.Spec(97902)->sort == 2 && !f.Spec(1));
        CHECK(Model::CanPick(w, 500, 0) && !Model::CanPick(w, 325, 0) && !Model::CanPick(w, 500, 97902));
        CHECK(f.UnlearnCopper(0) == 10000 && f.UnlearnCopper(3) == 10000000 && f.UnlearnCopper(4) == 100000000);
        CHECK(f.UnlearnCopper(9) == 100000000);                           // 10,000 gold from the fifth on
        Model none;
        CHECK(none.UnlearnCopper(2) == 0);
        Specialisation bad{97904, 164, 3, 500, 500, "bad"};               // falls back to its own cap: no
        f.specs[bad.spell] = bad;
        CHECK(f.Problems().size() == 1);
    }

    // the trainer's lines in the three languages
    {
        using namespace ef::prof::text;
        CHECK(Money(100000000, Lang::En) == "10,000 gold" && Money(10000, Lang::En) == "1 gold");
        CHECK(Money(15025, Lang::En) == "1 gold 50 silver 25 copper" && Money(0, Lang::En) == "0 copper");
        CHECK(Money(100000000, Lang::De) == "10.000 Gold");
        CHECK(Money(100000000, Lang::Fr) == "10\xC2\xA0" "000\xC2\xA0pi\xC3\xA8" "ces d'or");
        CHECK(UnlearnTreeLine(Lang::En, "Rich Seams", 1) == "Unlearn talents: Rich Seams (1 point)");
        CHECK(UnlearnTreeLine(Lang::De, "Reiche Adern", 3) == "Talente verlernen: Reiche Adern (3 Punkte)");
        CHECK(PickLine(Lang::En, "Weaponsmith") == "Choose your specialisation: Weaponsmith");
        std::string const box = DropBox(Lang::En, "Weaponsmith", "Blacksmithing", 100000, 325);
        CHECK(box.find("10 gold") != std::string::npos && box.find("back to 325") != std::string::npos);
        CHECK(DropBox(Lang::De, "Waffenschmied", "Schmiedekunst", 10000, 325).find("1 Gold") != std::string::npos);
        CHECK(DropBox(Lang::Fr, "Forgeron d'armes", "Forge", 10000, 325).find("325") != std::string::npos);
        CHECK(PickBox(Lang::En, "Weapons.").rfind("Weapons.", 0) == 0);
        CHECK(Dropped(Lang::En, "Weaponsmith", "Blacksmithing", 10000, 325) ==
              "You gave up Weaponsmith for 1 gold. Blacksmithing is back at 325. Choose a new specialisation at a trainer.");
    }

    if (s_failed)
    {
        std::fprintf(stderr, "profspec tests: %d failed\n", s_failed);
        return 1;
    }
    std::printf("profspec tests: ok\n");
    return 0;
}
