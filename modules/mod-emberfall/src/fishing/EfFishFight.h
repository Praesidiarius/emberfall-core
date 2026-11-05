// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfFishFight.h: the fight on the line (docs/design/fishing.md §6.3) as one small deterministic simulator, and the
// server's plausibility bounds over its result (§6.5). Header-only, standard library only, integer arithmetic only:
// the same seed and the same inputs give the same fight on every compiler.
//
// This one file is shared: the client runs the fight at 20 ticks a second from the server's OPEN (client/src/data/,
// the encounter frame reads its bars), and mod-emberfall carries a verbatim copy (src/fishing/EfFishFight.h) for the
// bounds it checks an END against (the fastest landing any input could give, the phases' minimum lengths).
// tools/py/tests/test_fishing.py checks that the copies are equal. Change both together.
//
// The model, per 50-ms tick:
//   tension T (0-1000 of the bar, kept x1000) settles toward the fish's pull (its style's pull x its size factor x
//     40-100 % with its strength, plus a surge or a dash) plus 380 while reeling, less 650 while giving line;
//   line L (yards, kept x1000) shortens while reeling (1.6 yd/s x 25-100 % as the fish tires), a little while holding
//     (0.6 yd/s x the fish's tiredness), and lengthens as the fish takes it (0.7 yd/s x its strength x its speed; twice
//     on a dash, 1.6 x while giving line);
//   strength S (x1,000,000) drains 3 %/s x the style's drain / the stamina, twice while T is in the safe band, a
//     quarter more while reeling.
// Landed at L under 2 yd; snapped at T at the break; ran at L at the maximum line, at a jump with T over 700 (the hook
// tears out) or past the fight's maximum time; snagged when a snag is not freed in time.
// Styles: a darter dashes (heading flips) and sometimes jumps after a 0.6 s warning; a bulldog surges for 1.5 s after a
// 0.8 s wind-up; a snagger runs for cover (2-4 s, steer against it), reaching it after 3.5 s it snags: give line for
// 0.5 s within the style's hold time to free it.

#ifndef EMBERFALL_FISH_FIGHT_H
#define EMBERFALL_FISH_FIGHT_H

#include "EfFishRules.h"

#include <array>
#include <cstdint>

namespace effish
{
    constexpr std::uint32_t kTickMs = 50;
    constexpr std::uint32_t kTicksPerSecond = 1000 / kTickMs;

    enum class StyleKind : std::uint8_t { None = 0, Darter = 1, Bulldog = 2, Snagger = 3 };

    // A fight style (ef_fish_style / EfFishStyle.dbc; species.yaml `styles`).
    struct Style
    {
        std::uint32_t id = 0;
        StyleKind kind = StyleKind::None;
        std::uint32_t pullPermil = 500;        // the fish's pull, of the bar
        std::uint32_t drainPermil = 1000;      // how fast it tires
        std::uint32_t everyMinMs = 3000;       // between two moves (a dash, a surge, a run for cover)
        std::uint32_t everyMaxMs = 6000;
        std::uint32_t magnitudePermil = 0;     // darter: a move's jump chance; bulldog: the surge's added pull
        std::uint32_t holdMs = 3000;           // snagger: the time to free a snag
        std::uint32_t over900Ms = 8000;        // the bounds: time over 900 an honest fight may show
    };

    enum class Outcome : char { None = 0, Landed = 'L', Snapped = 'S', Ran = 'R', Snagged = 'N', Abandoned = 'X' };

    inline bool IsOutcome(char c)
    {
        return c == 'L' || c == 'S' || c == 'R' || c == 'N' || c == 'X';
    }

    // The fish's stamina by rarity x1000 (the target lengths: uncommon 15-30 s, rare 25-45 s, the legendary 45-90 s).
    inline std::uint32_t StaminaPermil(Rarity r)
    {
        switch (r)
        {
            case Rarity::Uncommon: return 1000;
            case Rarity::Rare: return 1500;
            case Rarity::Legendary: return 1800;
            default: return 1000;
        }
    }

    // The longest a fight may run before the fish is gone (and the server's timeout less its 10 s grace).
    inline std::uint32_t MaxFightMs(Rarity r)
    {
        switch (r)
        {
            case Rarity::Uncommon: return 60000;
            case Rarity::Rare: return 80000;
            case Rarity::Legendary: return 110000;
            default: return 0;
        }
    }

    // What the server sends in OPEN (with the style rows both sides have) and both sides build the fight from.
    struct FightParams
    {
        std::uint64_t seed = 0;
        std::array<Style, 3> styles{};               // one per phase
        std::array<std::uint32_t, 3> untilPermil{};  // phase k lasts while strength is above this (the last: 0)
        std::uint32_t phaseCount = 1;
        std::uint32_t sizeFactorPermil = 1000;       // SizeFactorPermil
        std::uint32_t speedPermil = 1000;            // Species::speedPermil
        std::uint32_t staminaPermil = 1000;          // StaminaPermil(rarity) x the size factor / 1000
        std::uint32_t safeLo = 350;                  // SafeBand
        std::uint32_t safeHi = 650;
        std::uint32_t breakAt = 1000;
        std::uint32_t maxLineMyd = 40000;            // yards x1000
        std::uint32_t startLineMyd = 11000;          // the bobber's distance from the angler
        std::uint32_t maxMs = 60000;                 // MaxFightMs(rarity)
        bool passiveRecovers = true;                 // holding brings line in as the fish tires (not for a rare or
                                                     // the legendary: they want the angler's hands)
    };

    struct Input
    {
        bool reel = false;
        bool give = false;
        std::int8_t steer = 0;   // -1 left, 0, 1 right
    };

    // What the fish is doing now (the status line, the bobber, the line's hum).
    enum class Move : std::uint8_t { None, Dash, JumpWarn, SurgeWarn, Surge, Run, Snagged };

    struct EndReport
    {
        Outcome outcome = Outcome::None;
        std::uint32_t durationMs = 0;
        std::uint32_t peakTension = 0;          // of the bar (0-1000)
        std::uint32_t over900Ms = 0;
        std::uint32_t inputSeconds = 0;         // whole seconds in which a control was held
        std::array<std::uint32_t, 3> phaseEndMs{};   // the legendary: when each phase but the last ended (0: not reached)
    };

    // The tension the fish pulls with at a strength (x1000 of the bar, x1000).
    inline std::int64_t PullTension(Style const& st, FightParams const& p, std::int64_t strengthPpm)
    {
        std::int64_t pull = static_cast<std::int64_t>(st.pullPermil) * p.sizeFactorPermil / 1000;
        if (pull > 1000)
            pull = 1000;
        return pull * (400000 + 600 * strengthPpm / 1000) / 1000;   // x1000: pts x (0.4 + 0.6 S)
    }

    // The strength drained in one tick (ppm).
    inline std::int64_t DrainPerTick(Style const& st, FightParams const& p, bool inBand, bool reeling)
    {
        std::int64_t d = 30000 / static_cast<std::int64_t>(kTicksPerSecond);   // 3 %/s
        d = d * st.drainPermil / 1000;
        if (inBand)
            d *= 2;
        if (reeling)
            d = d * 5 / 4;
        std::int64_t const stamina = p.staminaPermil == 0 ? 1000 : p.staminaPermil;
        d = d * 1000 / stamina;
        return d < 1 ? 1 : d;
    }

    // The line reeled in during one tick at a strength (yards x1000).
    inline std::int64_t ReelPerTick(std::int64_t strengthPpm)
    {
        return 1600 * (250000 + 750 * (1000000 - strengthPpm) / 1000) / 1000000 / static_cast<std::int64_t>(kTicksPerSecond);
    }

    class Fight
    {
    public:
        explicit Fight(FightParams const& params) : m_p(params), m_rng(params.seed)
        {
            if (m_p.phaseCount < 1)
                m_p.phaseCount = 1;
            if (m_p.phaseCount > 3)
                m_p.phaseCount = 3;
            m_p.untilPermil[m_p.phaseCount - 1] = 0;
            m_line = m_p.startLineMyd;
            m_tension = PullTension(style(), m_p, m_strength);
            m_peak = m_tension;
            scheduleMove();
        }

        // One 50-ms tick with the controls as they are now. Returns the outcome (None while it goes on).
        Outcome Step(Input const& in)
        {
            if (m_outcome != Outcome::None)
                return m_outcome;
            ++m_tick;
            Style const& st = style();
            bool const reel = in.reel && !in.give;
            bool const give = in.give;
            if (in.reel || in.give || in.steer != 0)
                m_inputThisSecond = true;
            if (m_tick % kTicksPerSecond == 0)
            {
                if (m_inputThisSecond)
                    ++m_inputSeconds;
                m_inputThisSecond = false;
            }

            // the fish's move
            std::int64_t pullAdd = 0;
            std::int64_t take = 700 * m_strength / 1000000 * m_p.speedPermil / 1000 / kTicksPerSecond;   // yd x1000
            advanceMove(in, give, pullAdd, take);
            if (m_outcome != Outcome::None)
                return finish();

            // tension
            std::int64_t target = PullTension(st, m_p, m_strength) + pullAdd * 1000;
            if (m_move == Move::Snagged)
                target += 200000;
            if (reel)
                target += 380000;
            if (give)
                target -= 650000;
            if (target < 0)
                target = 0;
            std::int64_t const k = target > m_tension ? 120 : 200;   // per mille a tick
            m_tension += (target - m_tension) * k / 1000;
            if (m_jolt > 0)
            {
                m_tension += m_jolt;
                m_jolt = 0;
            }
            if (m_tension < 0)
                m_tension = 0;
            if (m_tension > m_peak)
                m_peak = m_tension;
            if (m_tension > 900000)
                m_over900 += kTickMs;

            // line
            if (m_move != Move::Snagged)
            {
                std::int64_t delta = give ? take * 8 / 5 : take;
                if (reel)
                    delta -= ReelPerTick(m_strength);
                else if (!give && m_p.passiveRecovers)
                    delta -= 600 * (1000000 - m_strength) / 1000000 / kTicksPerSecond;
                m_line += delta;
                if (m_line < 0)
                    m_line = 0;
            }

            // strength
            bool const inBand = m_tension >= static_cast<std::int64_t>(m_p.safeLo) * 1000 &&
                                m_tension <= static_cast<std::int64_t>(m_p.safeHi) * 1000;
            m_strength -= DrainPerTick(st, m_p, inBand, reel);
            if (m_strength < 0)
                m_strength = 0;
            while (m_phase + 1 < m_p.phaseCount &&
                   m_strength <= static_cast<std::int64_t>(m_p.untilPermil[m_phase]) * 1000)
            {
                m_report.phaseEndMs[m_phase] = m_tick * kTickMs;
                ++m_phase;
                m_move = Move::None;
                m_moveTicks = 0;
                scheduleMove();
            }

            // the end
            if (m_tension >= static_cast<std::int64_t>(m_p.breakAt) * 1000)
                m_outcome = Outcome::Snapped;
            else if (m_line >= static_cast<std::int64_t>(m_p.maxLineMyd))
                m_outcome = Outcome::Ran;
            else if (m_line < 2000)
                m_outcome = Outcome::Landed;
            else if (m_tick * kTickMs >= m_p.maxMs)
                m_outcome = Outcome::Ran;
            return m_outcome == Outcome::None ? m_outcome : finish();
        }

        // Taking damage jerks the hands (fishing.md §6.6): tension +150.
        void Jolt(std::uint32_t points = 150) { m_jolt += static_cast<std::int64_t>(points) * 1000; }

        // The player gave up (the frame's close, moving): the fight ends abandoned.
        void Abandon()
        {
            if (m_outcome == Outcome::None)
            {
                m_outcome = Outcome::Abandoned;
                finish();
            }
        }

        Outcome outcome() const { return m_outcome; }
        std::uint32_t elapsedMs() const { return m_tick * kTickMs; }
        std::uint32_t tension() const { return static_cast<std::uint32_t>(m_tension / 1000); }   // 0-1000+
        std::uint32_t lineMyd() const { return static_cast<std::uint32_t>(m_line); }
        std::uint32_t strengthPermil() const { return static_cast<std::uint32_t>(m_strength / 1000); }
        std::uint32_t phase() const { return m_phase; }
        Move move() const { return m_move; }
        std::int8_t heading() const { return m_heading; }   // the side the fish pulls to (the bobber's drag)
        FightParams const& params() const { return m_p; }
        StyleKind kind() const { return style().kind; }

        // The summary END carries (valid once the fight has an outcome).
        EndReport const& report() const { return m_report; }

    private:
        Style const& style() const { return m_p.styles[m_phase]; }

        void scheduleMove()
        {
            Style const& st = style();
            std::int32_t ms = m_rng.Range(static_cast<std::int32_t>(st.everyMinMs), static_cast<std::int32_t>(st.everyMaxMs));
            std::uint32_t const speed = m_p.speedPermil == 0 ? 1000 : m_p.speedPermil;
            ms = static_cast<std::int32_t>(static_cast<std::int64_t>(ms) * 1000 / speed);
            m_nextMoveTick = m_tick + static_cast<std::uint32_t>(ms) / kTickMs + 1;
        }

        void advanceMove(Input const& in, bool give, std::int64_t& pullAdd, std::int64_t& take)
        {
            Style const& st = style();
            if (m_move == Move::None)
            {
                if (st.kind == StyleKind::None || m_tick < m_nextMoveTick)
                    return;
                switch (st.kind)
                {
                    case StyleKind::Darter:
                        if (m_rng.Below(1000) < st.magnitudePermil)
                            startMove(Move::JumpWarn, 12);
                        else
                        {
                            m_heading = static_cast<std::int8_t>(-m_heading);
                            startMove(Move::Dash, 20);
                        }
                        break;
                    case StyleKind::Bulldog:
                        startMove(Move::SurgeWarn, 16);
                        break;
                    case StyleKind::Snagger:
                        m_heading = m_rng.Below(2) ? std::int8_t{1} : std::int8_t{-1};
                        m_cover = 0;
                        startMove(Move::Run, static_cast<std::uint32_t>(m_rng.Range(30, 80)));
                        break;
                    default:
                        break;
                }
            }
            switch (m_move)
            {
                case Move::JumpWarn:
                    pullAdd += 450;   // the bobber surges up before the jump
                    break;
                case Move::Dash:
                    pullAdd += 150;
                    take *= 2;
                    break;
                case Move::Surge:
                    pullAdd += st.magnitudePermil;
                    break;
                case Move::Run:
                    take = take * 3 / 2;
                    if (in.steer == -m_heading)
                        m_cover = m_cover > 2 ? m_cover - 2 : 0;
                    else
                        ++m_cover;
                    if (m_cover >= 70)
                    {
                        startMove(Move::Snagged, st.holdMs / kTickMs);
                        m_giveRun = 0;
                        return;
                    }
                    break;
                case Move::Snagged:
                    m_giveRun = give ? m_giveRun + 1 : 0;
                    if (m_giveRun >= 10)
                    {
                        endMove();
                        return;
                    }
                    break;
                default:
                    break;
            }
            if (m_move != Move::None && --m_moveTicks == 0)
            {
                switch (m_move)
                {
                    case Move::JumpWarn:
                        // the jump: a tight line tears the hook out
                        if (m_tension > 700000)
                            m_outcome = Outcome::Ran;
                        endMove();
                        break;
                    case Move::SurgeWarn:
                        startMove(Move::Surge, 30);
                        break;
                    case Move::Snagged:
                        m_outcome = Outcome::Snagged;
                        endMove();
                        break;
                    default:
                        endMove();
                        break;
                }
            }
        }

        void startMove(Move m, std::uint32_t ticks)
        {
            m_move = m;
            m_moveTicks = ticks == 0 ? 1 : ticks;
        }

        void endMove()
        {
            m_move = Move::None;
            m_moveTicks = 0;
            m_cover = 0;
            scheduleMove();
        }

        Outcome finish()
        {
            m_report.outcome = m_outcome;
            m_report.durationMs = m_tick * kTickMs;
            m_report.peakTension = static_cast<std::uint32_t>(m_peak / 1000);
            m_report.over900Ms = m_over900;
            m_report.inputSeconds = m_inputSeconds + (m_inputThisSecond ? 1u : 0u);
            return m_outcome;
        }

        FightParams m_p;
        Rng m_rng;
        std::uint32_t m_tick = 0;
        std::int64_t m_tension = 0;
        std::int64_t m_line = 0;
        std::int64_t m_strength = 1000000;
        std::int64_t m_peak = 0;
        std::int64_t m_jolt = 0;
        std::uint32_t m_over900 = 0;
        std::uint32_t m_inputSeconds = 0;
        bool m_inputThisSecond = false;
        std::uint32_t m_phase = 0;
        Move m_move = Move::None;
        std::uint32_t m_moveTicks = 0;
        std::uint32_t m_nextMoveTick = 0;
        std::uint32_t m_cover = 0;
        std::uint32_t m_giveRun = 0;
        std::int8_t m_heading = 1;
        Outcome m_outcome = Outcome::None;
        EndReport m_report;
    };

    // --- the server's bounds (fishing.md §6.5) -------------------------------------------------------------------------

    // The fastest any input could land the fish: reeling every tick while the strength drains at its fastest (in the
    // band and reeling) and the fish takes no line. Every honest fight is at least this long (the reel speed only falls
    // with strength and the fish's take only adds line). Also gives each phase's fastest end (phaseEndMs).
    struct Fastest
    {
        std::uint32_t landMs = 0;
        std::array<std::uint32_t, 3> phaseMinMs{};   // the shortest each phase can last
    };

    inline Fastest FastestLanding(FightParams const& p)
    {
        Fastest out;
        std::int64_t line = p.startLineMyd;
        std::int64_t strength = 1000000;
        std::uint32_t phase = 0;
        std::uint32_t phaseCount = p.phaseCount < 1 ? 1 : (p.phaseCount > 3 ? 3 : p.phaseCount);
        std::uint32_t tick = 0;
        std::uint32_t phaseStart = 0;
        std::uint32_t const cap = 20 * 60 * kTicksPerSecond;
        while (tick < cap)
        {
            ++tick;
            line -= ReelPerTick(strength);
            strength -= DrainPerTick(p.styles[phase], p, true, true);
            if (strength < 0)
                strength = 0;
            while (phase + 1 < phaseCount && strength <= static_cast<std::int64_t>(p.untilPermil[phase]) * 1000)
            {
                out.phaseMinMs[phase] = (tick - phaseStart) * kTickMs;
                phaseStart = tick;
                ++phase;
            }
            if (line < 2000)
                break;
        }
        out.landMs = tick * kTickMs;
        return out;
    }

    // The minimum time to drain a phase from its start to its end, phase by phase, at the fastest drain (independent of
    // the line: a phase that ends after the landing still has its minimum).
    inline std::array<std::uint32_t, 3> PhaseMinimums(FightParams const& p)
    {
        std::array<std::uint32_t, 3> out{};
        std::uint32_t const phaseCount = p.phaseCount < 1 ? 1 : (p.phaseCount > 3 ? 3 : p.phaseCount);
        std::int64_t strength = 1000000;
        for (std::uint32_t k = 0; k + 1 < phaseCount; ++k)
        {
            std::int64_t const until = static_cast<std::int64_t>(p.untilPermil[k]) * 1000;
            std::uint32_t ticks = 0;
            std::int64_t const drain = DrainPerTick(p.styles[k], p, true, true);
            while (strength > until && ticks < 20 * 60 * kTicksPerSecond)
            {
                strength -= drain;
                ++ticks;
            }
            out[k] = ticks * kTickMs;
        }
        return out;
    }

    enum class Verdict : std::uint8_t
    {
        Ok = 0,
        Outcome,     // not an outcome code
        TooFast,     // landed faster than any input could
        TooSlow,     // past the fight's maximum and the grace
        Break,       // reported peak at or over the break
        UnderPull,   // reported peak under the fish's own pull at the hook
        Over900,     // too long over 900
        Inputs,      // a rare or the legendary landed with a control held in under a quarter of its seconds
        Phases,      // the legendary's phases out of order or too short
    };

    inline char const* VerdictCode(Verdict v)
    {
        switch (v)
        {
            case Verdict::Ok: return "OK";
            case Verdict::Outcome: return "OUTCOME";
            case Verdict::TooFast: return "FAST";
            case Verdict::TooSlow: return "SLOW";
            case Verdict::Break: return "BREAK";
            case Verdict::UnderPull: return "PULL";
            case Verdict::Over900: return "OVER900";
            case Verdict::Inputs: return "INPUTS";
            case Verdict::Phases: return "PHASES";
        }
        return "?";
    }

    // The checks on END (fishing.md §6.5). `serverMs` is the server's own clock from OPEN to END. Only a landed fish
    // needs to pass the lower bounds; any reported loss is accepted as it is (a cheat gains nothing by losing).
    inline Verdict CheckEnd(FightParams const& p, Rarity rarity, EndReport const& r, std::uint32_t serverMs)
    {
        if (!IsOutcome(static_cast<char>(r.outcome)))
            return Verdict::Outcome;
        if (serverMs > p.maxMs + 10000)
            return Verdict::TooSlow;
        if (r.outcome != Outcome::Landed)
            return Verdict::Ok;
        Fastest const f = FastestLanding(p);
        if (serverMs + 500 < f.landMs || r.durationMs + 500 < f.landMs)
            return Verdict::TooFast;
        if (r.peakTension >= p.breakAt)
            return Verdict::Break;
        std::uint32_t const pull0 = static_cast<std::uint32_t>(PullTension(p.styles[0], p, 1000000) / 1000);
        if (r.peakTension + 1 < pull0)
            return Verdict::UnderPull;
        std::uint32_t allow = 0;
        for (std::uint32_t k = 0; k < p.phaseCount && k < 3; ++k)
            if (p.styles[k].over900Ms > allow)
                allow = p.styles[k].over900Ms;
        if (r.over900Ms > allow)
            return Verdict::Over900;
        if ((rarity == Rarity::Rare || rarity == Rarity::Legendary) && r.inputSeconds * 4 < r.durationMs / 1000)
            return Verdict::Inputs;
        if (p.phaseCount > 1)
        {
            std::array<std::uint32_t, 3> const mins = PhaseMinimums(p);
            std::uint32_t prev = 0;
            for (std::uint32_t k = 0; k + 1 < p.phaseCount && k < 3; ++k)
            {
                std::uint32_t const end = r.phaseEndMs[k];
                if (end == 0 || end < prev || end > r.durationMs || end - prev + 500 < mins[k])
                    return Verdict::Phases;
                prev = end;
            }
        }
        return Verdict::Ok;
    }

    // Builds a fight's parameters from the rows both sides have: the species, its styles (by phase), the size, the safe
    // band, the bobber's distance and the seed. `styleOf` returns the Style of an id (a default Style when unknown).
    template <typename StyleOf>
    FightParams MakeParams(Species const& s, StyleOf const& styleOf, std::uint32_t sizeMm, Band band,
                           std::uint32_t startLineMyd, std::uint64_t seed)
    {
        FightParams p;
        p.seed = seed;
        p.sizeFactorPermil = SizeFactorPermil(sizeMm, s.sizeTypMm, s.sizeMaxMm);
        p.speedPermil = s.speedPermil == 0 ? 1000 : s.speedPermil;
        p.staminaPermil = StaminaPermil(s.rarity) * p.sizeFactorPermil / 1000;
        p.safeLo = band.lo;
        p.safeHi = band.hi;
        p.startLineMyd = startLineMyd < 3000 ? 3000 : (startLineMyd > 30000 ? 30000 : startLineMyd);
        p.maxMs = MaxFightMs(s.rarity);
        p.passiveRecovers = s.rarity == Rarity::Common || s.rarity == Rarity::Uncommon;
        std::uint32_t n = 0;
        for (Phase const& ph : s.phases)
        {
            if (ph.style == 0 || n >= 3)
                continue;
            p.styles[n] = styleOf(ph.style);
            p.untilPermil[n] = ph.untilPermil;
            ++n;
        }
        if (n == 0)
        {
            p.styles[0] = styleOf(s.style);
            n = 1;
        }
        p.phaseCount = n;
        p.untilPermil[n - 1] = 0;
        return p;
    }
}

#endif
