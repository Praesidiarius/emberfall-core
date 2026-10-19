// Standalone tests of mod-emberfall's world editor logic (src/editor/EfEditorLogic.h, Emberfall
// docs/design/world-editor.md §6): the EFE requests, the rules of §6.3, the rate windows, the flat JSON of the ops,
// markers and pages, replay, and a decode fuzz. No core, no database. Built and run by tests/run_tests.sh.

#include "EfEditorLogic.h"

#include <cstdio>
#include <random>
#include <string>

using namespace ef::editor;

static int s_failed = 0;
#define CHECK(cond)                                                                       \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++s_failed;                                                                   \
        }                                                                                 \
    } while (0)

static std::vector<std::string> fields(std::string_view body)
{
    auto r = ef::eft::parse(body);
    return r.ok() ? r.request.fields : std::vector<std::string>{};
}

static void numbers()
{
    CHECK(decimal("12.5") == 12.5f);
    CHECK(decimal("-277.67").has_value() && std::fabs(*decimal("-277.67") + 277.67f) < 1e-3f);
    CHECK(decimal("0") == 0.0f);
    CHECK(!decimal(""));
    CHECK(!decimal("-"));
    CHECK(!decimal("1.2.3"));
    CHECK(!decimal("1e5"));
    CHECK(!decimal("nan"));
    CHECK(!decimal("inf"));
    CHECK(!decimal(" 1"));
    CHECK(!decimal("1234567890123"));
    CHECK(!decimal("200001", 200000));
    CHECK(fixed(-0.001, 2) == "0.00");
    CHECK(fixed(1.005, 1) == "1.0");
    CHECK(std::fabs(normaliseOrientation(-1.0f) - 5.2831853f) < 1e-4f);
    CHECK(std::fabs(normaliseOrientation(7.0f) - 0.7168147f) < 1e-4f);
}

static void requests()
{
    auto add = parseAdd(fields("1;5;ADD;c;90203;-290.1;96.0;41.7;4.1"));
    CHECK(add && add->kind == Kind::Creature && add->entry == 90203 && std::fabs(add->pos.o - 4.1f) < 1e-5f);
    CHECK(!parseAdd(fields("1;5;ADD;x;90203;-290.1;96.0;41.7;4.1")));
    CHECK(!parseAdd(fields("1;5;ADD;c;0;-290.1;96.0;41.7;4.1")));
    CHECK(!parseAdd(fields("1;5;ADD;c;90203;-290.1;96.0;41.7")));
    CHECK(!parseAdd(fields("1;5;ADD;c;90203;-290.1;96.0;41.7;4.1;9")));
    CHECK(!parseAdd(fields("1;5;ADD;c;90203;-29000000;96.0;41.7;4.1")));
    auto move = parseMove(fields("1;6;MOVE;n;90129;-158.17;91.83;42.23;-0.5"));
    CHECK(move && move->kind == Kind::Npc && move->id == 90129 && move->pos.o > 5.7f);
    CHECK(!parseMove(fields("1;6;MOVE;c;10000000;1;1;1;0")));
    auto set = parseSet(fields("1;7;SET;c;91019;wander;10"));
    CHECK(set && set->field == SetField::Wander && set->value == 10.0f);
    CHECK(parseSet(fields("1;7;SET;c;91019;respawn;300"))->field == SetField::Respawn);
    CHECK(!parseSet(fields("1;7;SET;c;91019;entry;300")));
    CHECK(!parseSet(fields("1;7;SET;c;91019;wander;-1")));
    CHECK(parseDel(fields("1;8;DEL;c;9000001"))->id == 9000001);
    CHECK(!parseDel(fields("1;8;DEL;c")));
    auto scan = parseScan(fields("1;9;SCAN;0;-280;100;120"));
    CHECK(scan && scan->map == 0 && scan->radius == 120.0f);
    CHECK(!parseScan(fields("1;9;SCAN;0;-280;100;251")));
    CHECK(!parseScan(fields("1;9;SCAN;0;-280;100;0")));
}

static void rules()
{
    Limits const l;
    CHECK(checkWander(30, l) == Code::Ok && checkWander(30.5f, l) == Code::Value && checkWander(0, l) == Code::Ok);
    CHECK(checkRespawn(30, false, Level::Contributor, l) == Code::Ok);
    CHECK(checkRespawn(29, false, Level::Contributor, l) == Code::Value);
    CHECK(checkRespawn(3601, false, Level::Reviewer, l) == Code::Value);
    CHECK(checkRespawn(120.5f, false, Level::Contributor, l) == Code::Value);
    CHECK(checkRespawn(600, true, Level::Reviewer, l) == Code::Named);
    CHECK(checkRespawn(86400, true, Level::Maintainer, l) == Code::Ok);
    Pos const home{0, 0, 0, 0};
    CHECK(checkNpcMove(home, {39, 0, 0, 0}, Level::Contributor, l) == Code::Ok);
    CHECK(checkNpcMove(home, {41, 0, 0, 0}, Level::Contributor, l) == Code::Distance);
    CHECK(checkNpcMove(home, {400, 0, 0, 0}, Level::Reviewer, l) == Code::Ok);
    CHECK(checkGround(41.7f, 41.6f) == Code::Ok && checkGround(45.0f, 41.6f) == Code::Ground);
    CHECK(checkGround(41.7f, -100000.0f) == Code::Ground);

    // density: 12 hostiles within 30 yd including the new one is fine, 13 is not
    std::vector<Neighbour> near;
    for (int i = 0; i < 11; ++i)
        near.push_back({{float(5 + i * 2), 5, 0, 0}, true});
    CHECK(checkDensity({0, 0, 0, 0}, near, l).code == Code::Ok);
    near.push_back({{25, 5, 0, 0}, true});
    CHECK(checkDensity({0, 0, 0, 0}, near, l).code == Code::Density);
    near.back().self = true; // the moved spawn itself does not count
    CHECK(checkDensity({0, 0, 0, 0}, near, l).code == Code::Ok);
    CHECK(checkDensity({0, 0, 0, 0}, {{{2, 0, 0, 0}, true}}, l).code == Code::Spacing);
    CHECK(checkDensity({0, 0, 0, 0}, {{{7, 0, 0, 0}, false, true}}, l).code == Code::Clearance);
    Density d = checkDensity({0, 0, 0, 0}, {{{15, 0, 0, 0}, false, false, true}}, l);
    CHECK(d.code == Code::Ok && d.questGiverNear);

    Limits const r = l.scaled(Level::Reviewer), m = l.scaled(Level::Maintainer);
    CHECK(r.opsPerMinute == 600 && r.openSets == 30 && m.opsPerDay >= 60000000);
}

static void rate()
{
    Rate rate;
    Limits l;
    l.opsPerMinute = 3, l.deletesPerDay = 1;
    rate.configure(l);
    uint64_t t = 1000000;
    for (int i = 0; i < 3; ++i)
    {
        CHECK(rate.allows(OpKind::Move, t));
        rate.record(OpKind::Move, t);
    }
    CHECK(!rate.allows(OpKind::Move, t + 1000));
    CHECK(rate.allows(OpKind::Move, t + 60000));
    rate.record(OpKind::Delete, t + 60000);
    CHECK(!rate.allows(OpKind::Delete, t + 120000));
    CHECK(rate.allows(OpKind::Add, t + 120000));
    CHECK(rate.allows(OpKind::Delete, t + 60000 + 86400000));
}

static void text()
{
    CHECK(checkText("Boars off the lane", kMinTitle, kMaxTitle) == Code::Ok);
    CHECK(checkText("ab", kMinTitle, kMaxTitle) == Code::Text);
    CHECK(checkText(std::string(81, 'a'), kMinTitle, kMaxTitle) == Code::Text);
    CHECK(checkText("see https://example.org", kMinTitle, kMaxTitle) == Code::Text);
    CHECK(checkText("ping @everyone", kMinTitle, kMaxTitle) == Code::Text);
    CHECK(checkText("tab\there", kMinTitle, kMaxTitle) == Code::Text);
    CHECK(checkText("caf\xc3\xa9", kMinTitle, kMaxTitle) == Code::Text);
}

static void json()
{
    Fields o = posObject({-283.404f, 101.9f, 41.62f, 0.7854f});
    o["entry"] = Value::of(90203);
    o["wander"] = Value::null();
    o["note"] = Value::str("a \"b\"");
    std::string const s = toJson(o);
    CHECK(s == "{\"entry\":90203,\"note\":\"a \\\"b\\\"\",\"o\":0.785,\"wander\":null,\"x\":-283.4,\"y\":101.9,\"z\":41.62}");
    auto back = fromJson(s);
    CHECK(back && *back == o);
    CHECK(fromJson("{}") && fromJson("{}")->empty());
    CHECK(fromJson(" { \"a\" : 1 , \"b\":null } ") && fromJson(" { \"a\" : 1 , \"b\":null } ")->size() == 2);
    CHECK(fromJson("{\"id\":9000001}")->at("id").number == 9000001);
    CHECK(!fromJson("{\"a\":1e5}"));
    CHECK(!fromJson("{\"a\":1,}"));
    CHECK(!fromJson("{\"a\":1} x"));
    CHECK(!fromJson("[1]"));
    CHECK(!fromJson("{\"a\":\"unterminated}"));
    auto p = posOf(o);
    CHECK(p && std::fabs(p->x + 283.4f) < 1e-3f && std::fabs(p->o - 0.785f) < 1e-4f);
}

static void markers()
{
    Marker m;
    m.kind = Kind::Creature, m.id = 91019, m.entry = 90203, m.pos = {-277.667f, 110.333f, 41.51f, 1.01191f};
    m.wander = 6, m.respawn = 120, m.flags = "mp", m.lowguid = 4711;
    std::string const e = encodeMarker(m);
    CHECK(e == "c,91019,90203,-277.67,110.33,41.51,1.012,6.0,120,mp,4711");
    auto back = decodeMarker(e);
    CHECK(back && back->id == 91019 && back->flags == "mp" && back->lowguid == 4711);
    CHECK(!decodeMarker("c,91019,90203,-277.67,110.33,41.51,1.012,6.0,120,mp"));
    CHECK(!decodeMarker("q,91019,90203,-277.67,110.33,41.51,1.012,6.0,120,mp,1"));

    std::vector<std::string> entries(40, e);
    auto bodies = pages("MARK", {"7"}, entries);
    CHECK(bodies.size() > 1);
    size_t total = 0;
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        CHECK(bodies[i].size() <= ef::eft::kMaxBody);
        auto r = ef::eft::parse(bodies[i]);
        CHECK(r.ok() && r.request.cmd == "MARK" && r.request.fields.size() == 3);
        CHECK(r.request.fields[1] == std::to_string(i + 1) + "/" + std::to_string(bodies.size()));
        for (auto part : ef::eft::split(r.request.fields[2], '|'))
            total += decodeMarker(part).has_value();
    }
    CHECK(total == 40);
    CHECK(pages("MARK", {"7"}, {}).size() == 1);

    auto zones = parseZones("elderwood:0:5001, kindred:1:5401  bad:x:1");
    CHECK(zones.size() == 2 && zones[0].key == "elderwood" && zones[1].area == 5401);
}

static void ops()
{
    Op add;
    add.op = OpKind::Add, add.target = 9000001, add.to = posObject({1, 2, 3, 0});
    Op inv = inverse(add);
    CHECK(inv.op == OpKind::Delete && inv.from == add.to && inv.to.empty());
    Op move;
    move.op = OpKind::Move, move.from = posObject({0, 0, 0, 0}), move.to = posObject({5, 0, 0, 1});
    CHECK(inverse(move).to == move.from);

    std::vector<Op> list{move, move, move};
    CHECK(undoIndex(list) == 2u && !redoIndex(list));
    list[2].undone = true;
    CHECK(undoIndex(list) == 1u && redoIndex(list) == 2u);
    list[1].undone = true;
    CHECK(undoIndex(list) == 0u && redoIndex(list) == 1u);
    list.push_back(move); // a new op after undos: the undone ones are no longer redoable
    CHECK(!redoIndex(list) && undoIndex(list) == 3u);
    CHECK(netObjects(list) == 1);

    // replay (§7.3)
    Fields now = posObject({5, 0, 0, 1});
    CHECK(replayState(move, now) == Replay::AlreadyDone);
    CHECK(replayState(move, posObject({0, 0, 0, 0})) == Replay::Apply);
    CHECK(replayState(move, posObject({9, 9, 0, 0})) == Replay::Conflict);
    Op set;
    set.op = OpKind::Set, set.from = {{"wander", Value::of(6)}}, set.to = {{"wander", Value::of(10)}};
    CHECK(replayState(set, {{"wander", Value::of(10)}}) == Replay::AlreadyDone);
    CHECK(replayState(set, {{"wander", Value::of(6)}}) == Replay::Apply);
    CHECK(replayState(set, {{"wander", Value::of(3)}}) == Replay::Conflict);
    CHECK(state::replayed("accepted") && !state::replayed("released") && state::editable("changes_requested"));
    CHECK(!state::open("withdrawn") && state::known("conflict") && !state::known("bogus"));
}

// Random and mutated bodies through every parser: nothing may crash (ASan/UBSan watch) or accept garbage silently.
static void fuzz()
{
    std::mt19937 rng(20261019);
    std::vector<std::string> seeds = {
        "1;5;ADD;c;90203;-290.1;96.0;41.7;4.1", "1;6;MOVE;n;90129;-158.17;91.83;42.23;3.9",
        "1;7;SET;c;91019;wander;10", "1;8;DEL;c;9000001", "1;9;SCAN;0;-280;100;120",
        "{\"x\":1.5,\"y\":-2,\"z\":null,\"s\":\"t\"}", "c,91019,90203,-277.67,110.33,41.51,1.012,6.0,120,mp,4711"};
    char const alphabet[] = "0123456789.-;,|:/{}\"\\ acnxyzADDMOVESETwanderrespawnnull\t\x01\xff";
    for (int round = 0; round < 200000; ++round)
    {
        std::string s = seeds[rng() % seeds.size()];
        int const edits = 1 + int(rng() % 6);
        for (int e = 0; e < edits; ++e)
        {
            size_t const at = s.empty() ? 0 : rng() % (s.size() + 1);
            switch (rng() % 3)
            {
                case 0: s.insert(s.begin() + std::ptrdiff_t(at), alphabet[rng() % (sizeof alphabet - 1)]); break;
                case 1: if (!s.empty() && at < s.size()) s.erase(at, 1); break;
                default: if (!s.empty() && at < s.size()) s[at] = alphabet[rng() % (sizeof alphabet - 1)]; break;
            }
        }
        auto f = fields(s);
        (void)parseAdd(f), (void)parseMove(f), (void)parseSet(f), (void)parseDel(f), (void)parseScan(f);
        (void)fromJson(s), (void)decodeMarker(s), (void)decimal(s), (void)parseZones(s);
        if (auto m = parseMove(f))
            CHECK(std::isfinite(m->pos.x) && std::isfinite(m->pos.o) && m->pos.o >= 0 && m->pos.o < 6.2832f);
    }
}

int main()
{
    numbers();
    requests();
    rules();
    rate();
    text();
    json();
    markers();
    ops();
    fuzz();
    if (s_failed)
    {
        std::fprintf(stderr, "editor tests: %d check(s) failed\n", s_failed);
        return 1;
    }
    std::printf("editor tests: ok\n");
    return 0;
}
