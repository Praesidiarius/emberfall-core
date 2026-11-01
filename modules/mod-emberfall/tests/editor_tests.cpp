// Standalone tests of mod-emberfall's world editor logic (src/editor/EfEditorLogic.h, Emberfall
// docs/design/world-editor.md §6): the EFE requests, the rules of §6.3, the rate windows, the flat JSON of the ops,
// markers and pages, replay, and a decode fuzz. No core, no database. Built and run by tests/run_tests.sh.

#include "EfEditorLogic.h"

#include <cstdio>
#include <random>
#include <set>
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
    m.kind = Kind::Creature, m.id = 91019, m.entry = 90203, m.pos = {-1137.111f, -955.558f, 48.994f, 1.01191f};
    m.wander = 6, m.respawn = 120, m.flags = "mp", m.lowguid = 4711;
    std::string const e = encodeMarker(m);
    CHECK(e == "c,91019,90203,-1137.11,-955.56,48.99,1.012,6.0,120,mp,4711");
    auto back = decodeMarker(e);
    CHECK(back && back->id == 91019 && back->flags == "mp" && back->lowguid == 4711);
    CHECK(!decodeMarker("c,91019,90203,-1137.11,-955.56,48.99,1.012,6.0,120,mp"));
    CHECK(!decodeMarker("q,91019,90203,-1137.11,-955.56,48.99,1.012,6.0,120,mp,1"));

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

// The normal realm (world-editor.md §19): the role from the GM level, the drafts in the editor phase, editor mode.
static void normalRealm()
{
    CHECK(levelOfSecurity(0) == Level::None && levelOfSecurity(1) == Level::Contributor);
    CHECK(levelOfSecurity(2) == Level::Maintainer && levelOfSecurity(3) == Level::Maintainer && levelOfSecurity(4) == Level::Maintainer);
    CHECK(!appliesLive(Level::Contributor) && !appliesLive(Level::Reviewer) && appliesLive(Level::Maintainer));
    CHECK(kDraftPhase == (1u << 31) && kCopyIdMax < kTempIdBase);

    // a ghost marker carries the original's place, and the old 11 fields still parse
    Marker m;
    m.kind = Kind::Creature, m.id = 91019, m.entry = 90203, m.pos = {10, 20, 30, 1}, m.flags = "mg", m.lowguid = 7;
    m.ghost = Pos{1, 2, 3, 0.5f};
    std::string const e = encodeMarker(m);
    CHECK(e == "c,91019,90203,10.00,20.00,30.00,1.000,0.0,0,mg,7,1.00,2.00,3.00,0.500");
    auto back = decodeMarker(e);
    CHECK(back && back->ghost && std::fabs(back->ghost->y - 2) < 1e-4f && back->flags == "mg");
    CHECK(!decodeMarker(e + ",1"));
    CHECK(!decodeMarker("c,91019,90203,10.00,20.00,30.00,1.000,0.0,0,mg,7,1.00,2.00,x,0.500"));

    // the fold: the release state with a set's active ops
    Fields release = posObject({0, 0, 0, 0});
    release["wander"] = Value::of(6), release["respawn"] = Value::of(120), release["entry"] = Value::of(90203);
    Op move;
    move.op = OpKind::Move, move.from = posObject({0, 0, 0, 0}), move.to = posObject({5, 0, 0, 1});
    Op set;
    set.op = OpKind::Set, set.from = {{"wander", Value::of(6)}}, set.to = {{"wander", Value::of(10)}};
    Draft d = foldDraft(release, {move, set});
    CHECK(d.present && d.changed && !d.conflict);
    CHECK(posNear(d.state, posObject({5, 0, 0, 1})) && d.state["wander"] == Value::of(10) && d.state["respawn"] == Value::of(120));
    Op back2 = move;
    back2.from = move.to, back2.to = move.from;
    d = foldDraft(release, {move, back2});
    CHECK(d.present && !d.changed);                         // moved and moved back: no copy
    Op undone = move;
    undone.undone = true;
    CHECK(!foldDraft(release, {undone}).changed);
    Op del;
    del.op = OpKind::Delete;
    d = foldDraft(release, {move, del});
    CHECK(!d.present && d.changed);                         // removed in the draft
    d = foldDraft(posObject({5, 0, 0, 1}), {move});
    CHECK(d.present && !d.changed && !d.conflict);          // the release already has it (shipped)
    d = foldDraft(posObject({9, 9, 0, 0}), {move});
    CHECK(d.conflict && !d.changed);                        // a newer state: conflict, nothing drafted
    Op add;
    add.op = OpKind::Add, add.to = posObject({1, 1, 1, 0});
    add.to["entry"] = Value::of(90203);
    d = foldDraft(std::nullopt, {add, move});
    CHECK(d.present && d.changed && d.conflict);            // an add, then a move whose from it never had
    Op addMove = move;
    addMove.from = posObject({1, 1, 1, 0});
    d = foldDraft(std::nullopt, {add, addMove});
    CHECK(d.present && !d.conflict && posNear(d.state, move.to) && d.state["entry"] == Value::of(90203));
    CHECK(!foldDraft(std::nullopt, {add, del}).present && !foldDraft(std::nullopt, {add, del}).changed);
    CHECK(!foldDraft(std::nullopt, {}).present);

    // editor mode
    CHECK(checkModeOn(false, true, false, false) == Code::Ok);
    CHECK(checkModeOn(true, true, false, false) == Code::Mode && checkModeOn(false, false, false, false) == Code::Mode);
    CHECK(checkModeOn(false, true, true, false) == Code::Mode && checkModeOn(false, true, false, true) == Code::Mode);
    CHECK(returnsOnModeOff(Level::Contributor) && !returnsOnModeOff(Level::Maintainer));
    CHECK(std::string(codeName(Code::Mode)) == "MODE");
    CHECK(autoTitle("Elderwood", "2026-10-31 12:00") == "Elderwood edits 2026-10-31");
    CHECK(autoTitle(std::string(200, 'z'), "2026-10-31").size() <= kMaxTitle);
    CHECK(checkText(autoTitle("the Sunken Terrace", "2026-10-31"), kMinTitle, kMaxTitle) == Code::Ok);
}

// Random and mutated bodies through every parser: nothing may crash (ASan/UBSan watch) or accept garbage silently.
static void fuzz()
{
    std::mt19937 rng(20261019);
    std::vector<std::string> seeds = {
        "1;5;ADD;c;90203;-290.1;96.0;41.7;4.1", "1;6;MOVE;n;90129;-158.17;91.83;42.23;3.9",
        "1;7;SET;c;91019;wander;10", "1;8;DEL;c;9000001", "1;9;SCAN;0;-280;100;120",
        "{\"x\":1.5,\"y\":-2,\"z\":null,\"s\":\"t\"}", "c,91019,90203,-1137.11,-955.56,48.99,1.012,6.0,120,mp,4711",
        "1;10;SET;c;91019;count;3", "c,91019,90203,-1137.11,-955.56,48.99,1.012,6.0,120,mp,4711,3:4712:4713",
        "1;11;PADD;p;trees/elderwood-oak;-400.5;-1337.9;41.2;52.5;1.25", "1;12;PMOVE;b;@-400.54,-1337.92;-401;-1338;41;10;1",
        "1;13;PDEL;p;9000001", "1;0;PLACE;12;b;7;dm;0;World/wmo/Elderwood/kit-inn;-1.5;2.25;41;270;1;-1.5;3",
        "{\"map\":0,\"model\":\"trees/oak\",\"size\":1.1,\"u\":1713.75,\"v\":698.37,\"yaw\":52.8,\"z\":0}"};
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
        (void)fromJson(s), (void)decodeMarker(s), (void)decimal(s), (void)parseZones(s), (void)decodePath(s);
        (void)parsePaint(f), (void)parseRoad(f), (void)parseSculpt(f);
        (void)parsePlaceAdd(f), (void)parsePlaceMove(f), (void)parsePlaceDel(f), (void)parseRef(s), (void)modelKey(s);
        if (auto j = fromJson(s))
            if (auto st = placeStateOf(*j))
                CHECK(modelKey(st->model));
        if (auto pa = parsePlaceAdd(f))
            CHECK(modelKey(pa->model) && pa->yaw >= 0 && pa->yaw < 360 && pa->size > 0 && std::isfinite(pa->pos.x));
        if (auto pd = decodePlace(f))
            CHECK(!placeBody(*pd).empty());
        if (auto m = parseMove(f))
            CHECK(std::isfinite(m->pos.x) && std::isfinite(m->pos.o) && m->pos.o >= 0 && m->pos.o < 6.2832f);
    }
}

// ROLE fits one message (alpha1 0.4.19: a maintainer's 100000x limits pushed 12 zones to 256 bytes; the client dropped
// it and the editor never appeared for a GM).
static void role()
{
    std::vector<Zone> const alpha1 = parseZones(
        "cinderpine:0:5170 elderwood:0:5001 frostcombe:0:5120 greyfell:0:5130 greymarch:0:5110 hollow-hills:0:5200 "
        "kingsfall:0:5160 redmere:0:5230 stonereach:0:5150 sunken-terrace:0:5240 thornwold:0:5140 wester-reach:0:5210");
    CHECK(alpha1.size() == 12);
    for (Level level : {Level::Contributor, Level::Reviewer, Level::Maintainer})
        for (bool local : {false, true})
        {
            Limits const l = Limits{}.scaled(level);
            size_t dropped = 99;
            std::string const body = roleBody(uint32_t(level), alpha1, l.opsPerDay, l.openSets, l.opsPerSet, local, &dropped);
            CHECK(body.size() <= ef::eft::kMaxBody);
            CHECK(dropped == 0);
            auto const f = fields(body);
            CHECK(f.size() == 4);
            if (f.size() == 4)
            {
                CHECK(f[0] == std::to_string(uint32_t(level)));
                CHECK(ef::eft::split(f[1], ',').size() == 12);
                CHECK(f[3] == (local ? "local" : "realm"));
            }
        }
    // contributors see their real limits; a maintainer's are capped for display
    CHECK(fields(roleBody(1, alpha1, 600, 3, 150, false)).at(2) == "600,3,150");
    CHECK(fields(roleBody(3, alpha1, 60000000, 300000, 15000000, false)).at(2) == "9999,9999,9999");
    // more zones than one message holds: the ones at the end are left off, the rest still arrives
    std::vector<Zone> many = alpha1;
    for (int i = 0; i < 12; ++i)
        many.push_back({"extra-zone-" + std::to_string(i), 1, uint32_t(6000 + i)});
    size_t dropped = 0;
    std::string const body = roleBody(3, many, 1, 1, 1, false, &dropped);
    CHECK(body.size() <= ef::eft::kMaxBody);
    CHECK(dropped > 0 && dropped < many.size());
    auto const f = fields(body);
    CHECK(f.size() == 4 && ef::eft::split(f[1], ',').size() == many.size() - dropped);
    // RZONE carries them all, paged: alpha1's 13 zones (one more long key made ROLE drop one), and 60
    std::vector<Zone> thirteen = alpha1;
    thirteen.push_back({"highwall", 0, 5250});
    std::vector<Zone> sixty = many;
    for (int i = 0; i < 36; ++i)
        sixty.push_back({"another-long-zone-key-" + std::to_string(i), 1, uint32_t(7000 + i)});
    for (std::vector<Zone> const* list : {&thirteen, &sixty})
    {
        auto const out = roleZonePages(*list);
        CHECK(!out.empty());
        std::vector<std::string> got;
        for (size_t i = 0; i < out.size(); ++i)
        {
            CHECK(out[i].size() <= ef::eft::kMaxBody);
            auto const pf = fields(out[i]);
            CHECK(pf.size() == 2 && pf[0] == std::to_string(i + 1) + "/" + std::to_string(out.size()));
            if (pf.size() == 2)
                for (auto const& e : ef::eft::split(pf[1], '|'))
                    got.emplace_back(e);
        }
        CHECK(got.size() == list->size());
        CHECK(!got.empty() && got.back() == list->back().key + ":" + std::to_string(list->back().map) + ":" +
                                                std::to_string(list->back().area));
    }
    CHECK(roleZonePages(sixty).size() > 1);
}

// A spawn of several creatures (count): derived GUIDs, the ring, the rules, the marker field, the fold and replay.
static void count()
{
    // GUIDs: content spawns + slot x 1,000,000, the realm's own ids + slot x 10,000,000; never 0, never a clash
    CHECK(extraId(91019, 1) == 1091019 && extraId(91019, 7) == 7091019 && extraId(999999, 7) == 7999999);
    CHECK(extraId(91019, 0) == 0 && extraId(91019, kMaxCount) == 0 && extraId(0, 1) == 0);
    CHECK(extraId(kTempIdBase, 1) == 19000001 && extraId(kCopyIdBase, 2) == 28000001 && extraId(kTempIdMax, 7) == 79999999);
    CHECK(extraId(1091019, 1) == 0 && extraId(19000001, 1) == 0);   // an extra has no extras of its own
    CHECK(extraOf(1091019) && extraOf(1091019)->base == 91019 && extraOf(1091019)->slot == 1);
    CHECK(extraOf(28000001) && extraOf(28000001)->base == kCopyIdBase && extraOf(28000001)->slot == 2);
    CHECK(!extraOf(91019) && !extraOf(2000000) && !extraOf(kCopyIdBase) && !extraOf(kTempIdBase) && !extraOf(15000000));
    CHECK(!extraOf(80000001) && baseOf(91019) == 91019 && baseOf(7091019) == 91019);
    std::set<uint32_t> seen;
    for (uint32_t id : {90001u, 91019u, 200001u, 999999u, kCopyIdBase, kCopyIdMax, kTempIdBase, kTempIdMax})
        for (uint32_t slot = 1; slot < kMaxCount; ++slot)
        {
            uint32_t const e = extraId(id, slot);
            CHECK(e != 0 && seen.insert(e).second);
            CHECK(e > kContentGuidMax && !(e >= kCopyIdBase && e <= kTempIdMax));   // outside every spawn range
            auto back = extraOf(e);
            CHECK(back && back->base == id && back->slot == slot);
        }
    CHECK(*seen.rbegin() < 0xFFFFFFFFu);

    // the ring: the same places whatever the count, every creature 3 yd or more from every other
    Pos const at{100, 200, 30, 1};
    auto five = groupPositions(at, 5), eight = groupPositions(at, 8);
    CHECK(five.size() == 5 && eight.size() == 8 && groupPositions(at, 1).size() == 1 && groupPositions(at, 99).size() == kMaxCount);
    for (size_t i = 0; i < five.size(); ++i)
        CHECK(dist2d(five[i], eight[i]) < 1e-4f);
    for (size_t i = 0; i < eight.size(); ++i)
    {
        CHECK(std::fabs(eight[i].z - 30) < 1e-4f);
        for (size_t j = i + 1; j < eight.size(); ++j)
            CHECK(dist2d(eight[i], eight[j]) >= 3.0f);
        if (i)
            CHECK(std::fabs(dist2d(at, eight[i]) - kCountRing) < 1e-3f);
    }
    // the vectors compile.py's count_slots matches (tools/py/tests/test_world_editor_count.py)
    CHECK(fixed(slotPos(at, 1).x, 2) == "104.00" && fixed(slotPos(at, 1).y, 2) == "200.00");
    CHECK(fixed(slotPos(at, 2).x, 2) == "96.40" && fixed(slotPos(at, 2).y, 2) == "198.26");
    CHECK(fixed(slotPos(at, 1).o, 3) == "3.400");

    // the value and who may set it
    CHECK(checkCount(1, false, Level::Contributor) == Code::Ok && checkCount(8, false, Level::Contributor) == Code::Ok);
    CHECK(checkCount(0, false, Level::Maintainer) == Code::Value && checkCount(9, false, Level::Maintainer) == Code::Value);
    CHECK(checkCount(2.5f, false, Level::Maintainer) == Code::Value);
    CHECK(checkCount(3, true, Level::Contributor) == Code::Named && checkCount(1, true, Level::Contributor) == Code::Ok);
    CHECK(checkCount(3, true, Level::Maintainer) == Code::Ok);
    auto set = parseSet(fields("1;7;SET;c;91019;count;3"));
    CHECK(set && set->field == SetField::Count && set->value == 3);

    // density for the whole group: 8 fit in an empty field, not beside 6 others (12 within 30 yd), not 2 yd off one
    Limits const l;
    CHECK(checkGroupDensity(groupPositions(at, 8), {}, l).density.code == Code::Ok);
    std::vector<Neighbour> crowd;
    for (int i = 0; i < 5; ++i)
    {
        Neighbour n;
        n.pos = {at.x + 15 + 4 * i, at.y, at.z, 0}, n.hostile = true;
        crowd.push_back(n);
    }
    CHECK(checkGroupDensity(groupPositions(at, 7), crowd, l).density.code == Code::Ok);      // 7 + 5 = 12
    GroupDensity g = checkGroupDensity(groupPositions(at, 8), crowd, l);                       // 13
    CHECK(g.density.code == Code::Density);
    Neighbour close;
    close.pos = slotPos(at, 3), close.pos.x += 1.5f, close.hostile = true;
    g = checkGroupDensity(groupPositions(at, 4), {close}, l);
    CHECK(g.density.code == Code::Spacing && g.member == 3);
    close.self = true;   // the spawn's own others never count against it
    CHECK(checkGroupDensity(groupPositions(at, 4), {close}, l).density.code == Code::Ok);

    // the marker: the count and the others' units at the end; 1 is not written; the old forms still parse
    Marker m;
    m.kind = Kind::Creature, m.id = 91019, m.entry = 90203, m.pos = at, m.flags = "m", m.lowguid = 7;
    CHECK(encodeMarker(m).find(",1,") == std::string::npos && ef::eft::split(encodeMarker(m), ',').size() == 11);
    m.count = 3, m.others = {8, 9};
    std::string e = encodeMarker(m);
    CHECK(e == "c,91019,90203,100.00,200.00,30.00,1.000,0.0,0,m,7,3:8:9");
    auto back = decodeMarker(e);
    CHECK(back && back->count == 3 && back->others == std::vector<uint32_t>({8, 9}) && !back->ghost);
    m.ghost = Pos{1, 2, 3, 0};
    e = encodeMarker(m);
    back = decodeMarker(e);
    CHECK(back && back->count == 3 && back->ghost && back->others.size() == 2);
    CHECK(!decodeMarker("c,91019,90203,100.00,200.00,30.00,1.000,0.0,0,m,7,1"));
    CHECK(!decodeMarker("c,91019,90203,100.00,200.00,30.00,1.000,0.0,0,m,7,9"));
    CHECK(!decodeMarker("c,91019,90203,100.00,200.00,30.00,1.000,0.0,0,m,7,3:x"));
    CHECK(!decodeMarker("c,91019,90203,100.00,200.00,30.00,1.000,0.0,0,m,7,3:1:2:3:4:5:6:7:8"));
    // the longest marker (temp id, every flag, a ghost, 8 creatures with 10-digit units) stays one message
    Marker big;
    big.kind = Kind::Creature, big.id = kTempIdMax, big.entry = 9999999, big.pos = {-19999.99f, -19999.99f, -4999.99f, 6.283f};
    big.wander = 999.9f, big.respawn = 604800, big.flags = "plrnadgx", big.lowguid = 4000000000u;
    big.ghost = Pos{-19999.99f, -19999.99f, -4999.99f, 6.283f}, big.count = 8;
    big.others = std::vector<uint32_t>(7, 4000000000u);
    std::string const be = encodeMarker(big);
    CHECK(be.size() <= kMaxMarker);
    auto bb = decodeMarker(be);
    CHECK(bb && bb->count == 8 && bb->others.size() <= 7);
    CHECK((ef::eft::body(0, "DELTA", {be})).size() <= ef::eft::kMaxBody);
    for (std::string const& body : pages("MARK", {"9999"}, std::vector<std::string>(5, be)))
        CHECK(body.size() <= ef::eft::kMaxBody);
    CHECK(pages("MARK", {"9999"}, std::vector<std::string>(5, be)).size() == 5);   // nothing dropped

    // the fold and replay: count is a field like wander; a release spawn with its count in effect is skipped
    Fields release = posObject({0, 0, 0, 0});
    release["wander"] = Value::of(6), release["respawn"] = Value::of(120), release["entry"] = Value::of(90203);
    release["count"] = Value::of(1);
    Op more;
    more.op = OpKind::Set, more.from = {{"count", Value::of(1)}}, more.to = {{"count", Value::of(3)}};
    Draft d = foldDraft(release, {more});
    CHECK(d.present && d.changed && d.state["count"] == Value::of(3));
    Fields shipped = release;
    shipped["count"] = Value::of(3);
    CHECK(replayState(more, shipped) == Replay::AlreadyDone);
    CHECK(replayState(more, release) == Replay::Apply);
    shipped["count"] = Value::of(5);
    CHECK(replayState(more, shipped) == Replay::Conflict);
    CHECK(!foldDraft(release, {inverse(more), more}).conflict);
    Op undone = more;
    undone.undone = true;
    CHECK(!foldDraft(release, {undone}).changed);
    CHECK(inverse(more).to == more.from);
    // an add with a count later: the count is in the draft's state
    Op add;
    add.op = OpKind::Add, add.to = posObject({1, 1, 1, 0});
    add.to["entry"] = Value::of(90203);
    d = foldDraft(std::nullopt, {add, more});
    CHECK(d.present && d.state["count"] == Value::of(3));

    // a raised count counts against the day's adds
    Rate r;
    Limits lim;
    lim.addsPerDay = 5;
    r.configure(lim);
    CHECK(r.allowsAdds(5, 1000) && !r.allowsAdds(6, 1000) && r.allowsAdds(0, 1000));
    r.recordAdds(4, 1000);
    CHECK(r.allowsAdds(1, 1000) && !r.allowsAdds(2, 1000) && r.allows(OpKind::Add, 1000));
    r.recordAdds(1, 1000);
    CHECK(!r.allows(OpKind::Add, 1000) && r.allowsAdds(3, 1000 + 86400000));
}

// Step 3 (world-editor.md §21): ground strokes, roads and height stamps.
static Op groundOp(uint32_t seq, Kind kind, uint32_t group, bool undone = false)
{
    Op op;
    op.seq = seq, op.op = OpKind::Add, op.kind = kind, op.target = seq, op.undone = undone;
    op.to["g"] = Value::of(group);
    return op;
}

static void ground()
{
    // paths: the first point absolute (one decimal), then decimetres
    auto p = decodePath("-283.4,101.9|12,-5|0,30");
    CHECK(p && p->size() == 3);
    if (p && p->size() == 3)
    {
        CHECK(std::fabs((*p)[1].x + 282.2f) < 1e-3f && std::fabs((*p)[1].y - 101.4f) < 1e-3f);
        CHECK(std::fabs((*p)[2].y - 104.4f) < 1e-3f);
        CHECK(encodePath(*p) == "-283.4,101.9|12,-5|0,30");
    }
    CHECK(encodePath({{-0.04f, 3.06f}, {1.0f, 3.0f}}) == "0.0,3.1|10,-1");
    CHECK(!decodePath(""));
    CHECK(!decodePath("1,2|601,0"));        // a step over 60 yd
    CHECK(!decodePath("1,2|1.5,0"));        // deltas are whole decimetres
    CHECK(!decodePath("1,2|a,0"));
    CHECK(!decodePath("1;2"));
    std::string many = "0,0";
    for (int i = 1; i < 40; ++i)
        many += "|1,1";
    CHECK(decodePath(many) && !decodePath(many + "|1,1"));

    auto paint = parsePaint(fields("1;9;PAINT;dirt;4.0;0.60;0.50;12;-283.4,101.9|12,-5"));
    CHECK(paint && paint->kind == Kind::Ground && paint->layer == "dirt" && paint->group == 12 && paint->pts.size() == 2);
    CHECK(paint && checkStroke(*paint, GroundLimits{}) == Code::Ok);
    CHECK(!parsePaint(fields("1;9;PAINT;Dirt;4.0;0.60;0.50;12;-283.4,101.9")));       // layer names are lower case
    CHECK(!parsePaint(fields("1;9;PAINT;dirt;4.0;0.60;0.50;12")));
    auto tooBig = parsePaint(fields("1;9;PAINT;dirt;40.0;0.60;0.50;0;0,0"));
    CHECK(tooBig && checkStroke(*tooBig, GroundLimits{}) == Code::Value);
    auto weak = parsePaint(fields("1;9;PAINT;dirt;4;0.01;0.5;0;0,0"));
    CHECK(weak && checkStroke(*weak, GroundLimits{}) == Code::Value);
    auto longLine = parsePaint(fields("1;9;PAINT;dirt;4;0.5;0.5;0;0,0|600,0|600,0|600,0|600,0|600,0"));
    CHECK(longLine && checkStroke(*longLine, GroundLimits{}) == Code::Distance);   // 300 yd > 250

    auto road = parseRoad(fields("1;9;ROAD;cobble;track;3;10,10|100,0"));
    CHECK(road && road->kind == Kind::Road && road->cls == "track" && std::fabs(road->radius - 2.5f) < 1e-6f);
    CHECK(!parseRoad(fields("1;9;ROAD;cobble;lane;3;10,10|100,0")));
    auto onePoint = parseRoad(fields("1;9;ROAD;cobble;path;3;10,10"));
    CHECK(onePoint && checkStroke(*onePoint, GroundLimits{}) == Code::Value);

    auto raise = parseSculpt(fields("1;9;SCULPT;raise;8.0;0.50;-;0;-290.1,96.0"));
    CHECK(raise && raise->kind == Kind::Height && raise->mode == Sculpt::Raise && !raise->height);
    auto flat = parseSculpt(fields("1;9;SCULPT;flatten;8.0;0.50;41.2;0;-290.1,96.0"));
    CHECK(flat && flat->height && std::fabs(*flat->height - 41.2f) < 1e-4f);
    CHECK(!parseSculpt(fields("1;9;SCULPT;flatten;8.0;0.50;-;0;-290.1,96.0")));     // flatten needs its height
    CHECK(!parseSculpt(fields("1;9;SCULPT;dig;8.0;0.50;-;0;-290.1,96.0")));
    auto tiny = parseSculpt(fields("1;9;SCULPT;lower;1.0;0.50;-;0;0,0"));
    CHECK(tiny && checkStroke(*tiny, GroundLimits{}) == Code::Value);

    // stored and read back (ef_op.to_json), and the GND message every client gets
    for (auto const& s : {*paint, *road, *flat})
    {
        Fields const to = strokeFields(s, 0);
        auto back = fromJson(toJson(to));
        CHECK(back.has_value());
        auto again = back ? strokeOf(s.kind, *back) : std::nullopt;
        CHECK(again && again->pts.size() == s.pts.size() && again->layer == s.layer && again->cls == s.cls);
        if (again)
            CHECK(encodePath(again->pts) == encodePath(s.pts) && std::fabs(again->radius - s.radius) < 1e-4f);
        std::string const body = groundBody(42, 17, "dm", 0, s, "Elderwood/" + s.layer);
        CHECK(!body.empty() && body.size() <= ef::eft::kMaxBody);
        auto f = ef::eft::split(body, ';');
        CHECK(f.size() > 7 && f[2] == "GND" && f[3] == "42" && f[4] == "17" && f[5] == "dm" && f[6] == "0");
    }
    CHECK(!strokeOf(Kind::Creature, strokeFields(*paint, 0)));
    // the client fills a request up to kMaxGroundRequest bytes (net/EfEditorSync): its GND still fits one message,
    // with the longest layer and tileset names, the biggest set and seq numbers
    std::string line = "-12345.6,-1234.5", req;
    for (int i = 1;; ++i)
    {
        std::string const next = line + "|-123,-45";
        if (ef::eft::body(9999, "PAINT", {"forest_floor_with_moss_x", "16.0", "1.00", "0.50", "9999", next}).size() > kMaxGroundRequest ||
            i >= int(kMaxGroundPoints))
            break;
        line = next;
    }
    auto full = parsePaint(fields(ef::eft::body(9999, "PAINT", {"forest_floor_with_moss_x", "16.0", "1.00", "0.50", "9999", line})));
    CHECK(full && full->pts.size() > 12);
    if (full)
        CHECK(!groundBody(9999, 9999, "dm", 1, *full, "Elderwood/forest_floor_with_moss_x").empty());

    // tiles as the client's data::tileAt: x north, y west
    CHECK(tileOf(0.0f, 0.0f) == std::make_pair(32, 32));
    CHECK(tileOf(-283.4f, 101.9f) == std::make_pair(31, 32));
    CHECK(tileOf(1e9f, -1e9f) == std::make_pair(63, 0));

    // undo and redo take a line's whole group; strokes are not objects (no lock, not in netObjects)
    std::vector<Op> ops{groundOp(1, Kind::Ground, 5), groundOp(2, Kind::Ground, 7), groundOp(3, Kind::Ground, 7),
                        groundOp(4, Kind::Ground, 7)};
    auto u = undoRange(ops);
    CHECK(u && u->first == 1 && u->second == 3);
    for (size_t i = 1; i <= 3; ++i)
        ops[i].undone = true;
    CHECK(groundCount(ops) == 1);
    auto r = redoRange(ops);
    CHECK(r && r->first == 1 && r->second == 3);
    std::vector<Op> loose{groundOp(1, Kind::Ground, 0), groundOp(2, Kind::Ground, 0)};
    auto u2 = undoRange(loose);
    CHECK(u2 && u2->first == 1 && u2->second == 1);   // group 0: one stroke at a time
    std::vector<Op> mixed{groundOp(1, Kind::Ground, 3), groundOp(2, Kind::Height, 3)};
    auto u3 = undoRange(mixed);
    CHECK(u3 && u3->first == 1);                       // another kind ends the group
    CHECK(netObjects(ops) == 0);
}

// Step 2 (world-editor.md §22): props and kit buildings. The vectors of variantOf, worldOf and orientationOf are
// Emberfall's doodads.py (variant_of, design_to_world, prop_orientation), so the preview stands what the bake builds.
static Op placeOp(uint32_t seq, OpKind kind, uint32_t target, Fields from, Fields to, bool undone = false)
{
    Op op;
    op.seq = seq, op.op = kind, op.kind = Kind::Prop, op.target = target, op.from = std::move(from), op.to = std::move(to);
    op.undone = undone;
    return op;
}

static void places()
{
    // requests
    auto a = parsePlaceAdd(fields("1;5;PADD;p;trees/elderwood-oak;-400.5;-1337.9;41.2;412.5;1.25"));
    CHECK(a && a->kind == Kind::Prop && a->model == "trees/elderwood-oak" && std::fabs(a->yaw - 52.5f) < 1e-3f &&
          std::fabs(a->size - 1.25f) < 1e-4f && std::fabs(a->pos.z - 41.2f) < 1e-3f);
    auto b = parsePlaceAdd(fields("1;5;PADD;b;kit-hut;-400.5;-1337.9;41.2;-90;1"));
    CHECK(b && b->kind == Kind::Building && std::fabs(b->yaw - 270.0f) < 1e-3f);
    CHECK(!parsePlaceAdd(fields("1;5;PADD;c;trees/oak;1;2;3;0;1")));        // creatures go through ADD
    CHECK(!parsePlaceAdd(fields("1;5;PADD;p;Trees/Oak;1;2;3;0;1")));        // keys are lower case
    CHECK(!parsePlaceAdd(fields("1;5;PADD;p;a/b/c;1;2;3;0;1")));
    CHECK(!parsePlaceAdd(fields("1;5;PADD;p;../x;1;2;3;0;1")));
    CHECK(!parsePlaceAdd(fields("1;5;PADD;p;trees/oak;1;2;3;0;0")));        // no size 0
    CHECK(!parsePlaceAdd(fields("1;5;PADD;p;trees/oak;1;2;3;0")));
    auto m = parsePlaceMove(fields("1;6;PMOVE;p;@-400.54,-1337.92;-401;-1338;41;10;1"));
    CHECK(m && m->ref.at && std::fabs(m->ref.x + 400.54f) < 1e-3f && std::fabs(m->ref.y + 1337.92f) < 1e-3f);
    auto m2 = parsePlaceMove(fields("1;6;PMOVE;b;9000004;-401;-1338;41;10;1"));
    CHECK(m2 && !m2->ref.at && m2->ref.id == 9000004);
    CHECK(!parsePlaceMove(fields("1;6;PMOVE;p;0;-401;-1338;41;10;1")));
    CHECK(!parsePlaceMove(fields("1;6;PMOVE;p;@1;-401;-1338;41;10;1")));
    auto d = parsePlaceDel(fields("1;7;PDEL;p;@-400.54,-1337.92"));
    CHECK(d && d->ref.at);
    CHECK(!parsePlaceDel(fields("1;7;PDEL;p")));
    CHECK(normaliseYaw(-0.01f) == 0.0f && normaliseYaw(359.97f) == 0.0f && std::fabs(normaliseYaw(720.4f) - 0.4f) < 1e-5f);

    // sizes: a contributor's prop 0.5-2, a maintainer's 0.1-5, a kit building 1 only
    PlaceLimits const pl;
    CHECK(checkPlaceSize(Kind::Prop, 1.5f, Level::Contributor, pl) == Code::Ok);
    CHECK(checkPlaceSize(Kind::Prop, 3.0f, Level::Contributor, pl) == Code::Value);
    CHECK(checkPlaceSize(Kind::Prop, 3.0f, Level::Maintainer, pl) == Code::Ok);
    CHECK(checkPlaceSize(Kind::Building, 1.0f, Level::Maintainer, pl) == Code::Ok);
    CHECK(checkPlaceSize(Kind::Building, 1.2f, Level::Maintainer, pl) == Code::Value);

    // the frame (Elderwood's origin 157, 235; a continent zone's -1053, -61), doodads.py design_to_world
    Frame const ew{157, 235, 1600.0 / 3.0}, cont{-1053, -61, 1600.0 / 3.0};
    auto w = worldOf(ew, 1713.75, 698.37, Kind::Prop);
    CHECK(std::fabs(w.first + 400.53666666) < 1e-6 && std::fabs(w.second + 1337.91666666) < 1e-6);
    auto w2 = worldOf(cont, 5971.0, 891.3, Kind::Prop);
    CHECK(std::fabs(w2.first + 297.46666666) < 1e-6 && std::fabs(w2.second + 4385.16666666) < 1e-6);
    auto uv = designOf(ew, w.first, w.second, Kind::Prop);
    CHECK(uv.first == 1713.75 && uv.second == 698.37);
    auto bw = worldOf(ew, 1231.83, 1119.28, Kind::Building);   // a building's u, v is its centre: no half tile
    auto buv = designOf(ew, bw.first, bw.second, Kind::Building);
    CHECK(buv.first == 1231.83 && buv.second == 1119.28);
    CHECK(orientationOf(Kind::Prop, 257.0) == 77.0 && orientationOf(Kind::Prop, 300.0) == 120.0);
    CHECK(orientationOf(Kind::Building, 90.0) == 270.0 && orientationOf(Kind::Building, 0.0) == 0.0);

    // variants: zlib's CRC-32 of "%.3f,%.3f"
    CHECK(crc32("hello") == 907060870u);
    CHECK(crc32("1713.750,698.370") == 1072536343u);
    CHECK(variantOf(1713.75, 698.37, 6) == 2 && variantOf(1824.6, 724.0, 6) == 4 && variantOf(1824.6, 724.0, 3) == 1);
    CHECK(variantOf(5971.0, 891.3, 6) == 5 && variantOf(0.0, 0.0, 6) == 6 && variantOf(-12.5, 3.25, 3) == 2);
    CHECK(variantOf(1713.75, 698.37, 0) == 0);
    CHECK(shownVariant(1824.6, 724.0, 6, 1u << 4) == 4);                 // the hashed one is built
    CHECK(shownVariant(1824.6, 724.0, 6, (1u << 2) | (1u << 6)) == 2);   // not built: the lowest one that is
    CHECK(variantPath("World/Doodads/trees/elderwood-oak", 2) == "World/Doodads/trees/elderwood-oak-v2");
    CHECK(variantPath("World/Doodads/clutter/rock", 0) == "World/Doodads/clutter/rock");

    // ops fold: a release prop moved, then removed; an add moved; undone ops skipped
    PlaceState rel;
    rel.present = true, rel.model = "trees/elderwood-oak", rel.u = 1713.75, rel.v = 698.37, rel.yaw = 52.8, rel.size = 1.1;
    PlaceState moved = rel;
    moved.u = 1720.0, moved.yaw = 90.0, moved.z = 41.5;
    Fields const relF = placeFields(Kind::Prop, rel), movedF = placeFields(Kind::Prop, moved);
    CHECK(toJson(relF) == "{\"map\":0,\"model\":\"trees/elderwood-oak\",\"size\":1.1,\"u\":1713.75,\"v\":698.37,\"yaw\":52.8,\"z\":0}");
    auto back = placeStateOf(*fromJson(toJson(movedF)));
    CHECK(back && samePlace(*back, moved));
    std::vector<Op> ops{placeOp(1, OpKind::Move, 7, relF, movedF)};
    PlaceFold f = foldPlace(ops);
    CHECK(f.touched && f.before.present && samePlace(f.before, rel) && samePlace(f.after, moved));
    ops.push_back(placeOp(2, OpKind::Delete, 7, movedF, {}));
    f = foldPlace(ops);
    CHECK(f.touched && !f.after.present);
    ops[1].undone = true;
    CHECK(samePlace(foldPlace(ops).after, moved));
    ops[0].undone = true;
    CHECK(!foldPlace(ops).touched);
    std::vector<Op> added{placeOp(1, OpKind::Add, 9000001, {}, movedF), placeOp(2, OpKind::Move, 9000001, movedF, relF)};
    f = foldPlace(added);
    CHECK(!f.before.present && f.after.present && samePlace(f.after, rel));
    CHECK(placeCount(added, Kind::Prop) == 1 && placeCount(added, Kind::Building) == 0);
    CHECK(netObjects(added) == 0);   // props have their own caps
    PlaceState turned = rel;
    turned.yaw = 52.8 + 360.0;
    CHECK(samePlace(rel, turned));
    turned.size = 1.2;
    CHECK(!samePlace(rel, turned));

    // PLACE fits one message and reads back; a removed object streams by the tile of its release instance
    PlaceShow s;
    s.cs = 9999, s.id = 9999999, s.map = 1, s.kind = Kind::Building, s.flags = "dm";
    s.path = "World/wmo/SunkenTerrace/kit-kf-tollhouse-and-a-very-long-model-name-x";
    s.x = -12345.67f, s.y = -12345.67f, s.z = -1234.56f, s.o = 359.9f, s.size = 1, s.hide = std::make_pair(-12345.67f, -12345.67f);
    std::string const body = placeBody(s);
    CHECK(!body.empty() && body.size() <= ef::eft::kMaxBody);
    auto r = ef::eft::parse(body);
    CHECK(r.ok() && r.request.cmd == "PLACE");
    auto got = decodePlace(r.request.fields);
    CHECK(got && got->cs == 9999 && got->kind == Kind::Building && got->path == s.path && got->hide &&
          std::fabs(got->hide->first + 12345.67f) < 0.01f && got->flags == "dm");
    PlaceShow gone = s;
    gone.path.clear();
    gone.x = gone.y = 0;
    gone.hide = std::make_pair(-283.4f, 101.9f);
    CHECK(placeTile(gone) == std::make_pair(31, 32));
    auto g2 = decodePlace(ef::eft::parse(placeBody(gone)).request.fields);
    CHECK(g2 && g2->path.empty() && g2->hide);
    CHECK(!decodePlace(ef::eft::parse("1;0;PLACE;1;p;7;-;0;World\\Doodads;0;0;0;0;1;-;-").request.fields));   // no backslashes
}

int main()
{
    places();
    ground();
    numbers();
    requests();
    rules();
    rate();
    text();
    json();
    markers();
    ops();
    normalRealm();
    role();
    count();
    fuzz();
    if (s_failed)
    {
        std::fprintf(stderr, "editor tests: %d check(s) failed\n", s_failed);
        return 1;
    }
    std::printf("editor tests: ok\n");
    return 0;
}
