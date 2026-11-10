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

// Emberfall (PATCHES.md, "Module"): the sun's hours (Emberfall docs/design/atmosphere.md "The sun's hours"). The game
// clock is the server's real local time already; here the server works out each day's sunrise and sunset and tells
// every client, so the world's sun rises and sets with the real one and every player sees the same sky.
//
//   Emberfall.Sun.Mode            real (default) | softened | fixed
//                                   real      the real sunrise and sunset of the place below on the server's date;
//                                   softened  half way between the fixed hours and the real ones;
//                                   fixed     the fixed hours below, as before.
//   Emberfall.Sun.Latitude        47.37   degrees north (Zurich)
//   Emberfall.Sun.Longitude       8.54    degrees east
//   Emberfall.Sun.FixedSunrise    6.0     hours
//   Emberfall.Sun.FixedSunset     19.75   hours
//   Emberfall.Sun.DaylightEvent   200     the game event that runs by day (Emberfall's world.yaml: NPCs with `hours`,
//                                         quest markers `during`); it follows the sun. 0: the core's clock keeps it.
//
// Read at start and on `.reload config` (AC_EMBERFALL_SUN_MODE ... in the environment, as every setting of the module).
// The daylight saving comes from the server's own clock: the hours are local hours by the UTC offset of the date's noon.
//
// The message (src/sun/EfSunTime.h, a verbatim copy of the client's): an addon whisper in the player's own name,
// `EFSUN\t1;<mode>;<sunrise>;<sunset>` in minutes after local midnight, at login, when the server's day changes and
// when the mode, the place or the fixed hours change. A client without it ignores the prefix; a server without this
// script sends nothing and the client keeps 06:00 / 19:45.
//
// The sun clock (EfSunTime.h SunClock: the local time mapped so that the day's sunrise is 06:00 and its sunset 19:45) is
// what the server reads the time of day on: Emberfall::Sun::PartNow() (src/sun/EfSun.h) for fishing's windows, and the
// daylight event's window, moved every day (its data says 06:00 for 840 minutes: it runs while the sun clock is within
// 06:00-20:00). The maths without the core: src/sun/EfSunLogic.h (tests/sun_tests.cpp).
//
// GM commands: `.efsun` (the mode, the place, today's hours in every mode, the sun clock and the part of the day now,
// the daylight event's window) and `.efsun mode real|softened|fixed` (admin; until the next config reload or restart).

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "sun/EfSun.h"
#include "sun/EfSunLogic.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>

using namespace Acore::ChatCommands;

namespace
{
    namespace ES = Emberfall::Sun;

    constexpr uint32 kTickMs = 10000;   // how often the day and the daylight event's window are looked at

    // ---- state: written on the world thread (config, the tick, the command), the hours read from any thread -------------
    ES::Settings s_settings;
    ES::Date s_today;
    int64 s_offsetSeconds = 0;          // the local clock's UTC offset at today's noon
    std::atomic<uint32> s_hours{ (360u << 16) | 1185u };   // today's sunrise and sunset in minutes, packed
    bool s_started = false;             // the world is loaded (the game events with it)
    uint32 s_tickMs = 0;

    uint32 s_eventId = 200;
    struct EventNominal
    {
        bool known = false;
        double start = 6.0;      // hours on the nominal clock (game_event.start_time's time of day)
        uint32 minutes = 840;    // game_event.length
    };
    EventNominal s_event;
    ES::Window s_eventWindow;

    uint32 Pack(efsun::SunHours h) { return (uint32(efsun::MinutesOf(h.sunrise)) << 16) | uint32(efsun::MinutesOf(h.sunset)); }
    efsun::SunHours Unpack(uint32 packed) { return { (packed >> 16) / 60.0, (packed & 0xFFFF) / 60.0 }; }

    // ---- the server's clock --------------------------------------------------------------------------------------------
    int64 NowSeconds() { return int64(GetEpochTime().count()); }

    ES::Date DateOf(std::tm const& tm) { return { tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday }; }

    // The seconds the local clock is ahead of UTC at Unix time `t`.
    int64 OffsetAt(int64 t)
    {
        std::tm const tm = Acore::Time::TimeBreakdown(time_t(t));
        return ES::DaysFromCivil(DateOf(tm)) * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec - t;
    }

    // The same at the date's local noon: the day's offset (a daylight saving switch happens in the small hours).
    int64 OffsetAtNoon(ES::Date date)
    {
        int64 const noonAsUtc = ES::DaysFromCivil(date) * 86400 + 12 * 3600;
        return OffsetAt(noonAsUtc - OffsetAt(noonAsUtc));
    }

    // "11-10 07:22": the local day and clock at a Unix time.
    std::string LocalStamp(int64 t)
    {
        std::tm const tm = Acore::Time::TimeBreakdown(time_t(t));
        return Acore::StringFormat("{:02}-{:02} {:02}:{:02}", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }

    double LocalHourNow()
    {
        std::tm const tm = Acore::Time::TimeBreakdown(time_t(NowSeconds()));
        return tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0;
    }

    efsun::SunHours HoursOn(ES::Settings const& settings, ES::Date date)
    {
        return ES::HoursFor(settings, date, OffsetAtNoon(date) / 3600.0);
    }

    // ---- telling the clients -------------------------------------------------------------------------------------------
    void Send(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
                                     std::string(efsun::kPrefix) + "\t" + efsun::Encode(s_settings.mode, Unpack(s_hours.load())));
        player->SendDirectMessage(&data);
    }

    uint32 SendAll()
    {
        uint32 told = 0;
        for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
            if (Player* player = session ? session->GetPlayer() : nullptr; player && player->IsInWorld())
            {
                Send(player);
                ++told;
            }
        return told;
    }

    // ---- the daylight event ----------------------------------------------------------------------------------------------
    // The window the event should have now: of yesterday's, today's and tomorrow's the first that has not ended.
    ES::Window EventWindowNow(int64 now)
    {
        ES::Window days[3];
        for (int i = 0; i < 3; ++i)
        {
            ES::Date const date = ES::AddDays(s_today, i - 1);
            int64 const offset = OffsetAtNoon(date);
            days[i] = ES::DayWindow(ES::DaysFromCivil(date) * 86400 - offset, s_event.start, s_event.minutes,
                                    ES::HoursFor(s_settings, date, offset / 3600.0));
        }
        return ES::CurrentWindow(now, days, 3);
    }

    void LoadEvent()
    {
        s_event = {};
        s_eventWindow = {};
        if (!s_eventId)
            return;
        QueryResult r = WorldDatabase.Query("SELECT CAST(TIME_TO_SEC(TIME(`start_time`)) AS UNSIGNED), `occurence`, `length` FROM `game_event` "
                                            "WHERE `eventEntry` = {}", s_eventId);
        if (!r)
            return;   // a world without the event (the stock world): nothing to move
        Field* f = r->Fetch();
        uint64 const startSeconds = f[0].Get<uint64>(), occurence = f[1].Get<uint64>(), length = f[2].Get<uint64>();
        if (occurence != 1440 || length == 0 || length >= 1440)
        {
            LOG_ERROR("server.loading", ">> Emberfall sun: game event {} (Emberfall.Sun.DaylightEvent) does not come round once a day "
                      "(every {} min for {} min): the core's clock keeps it", s_eventId, occurence, length);
            return;
        }
        s_event.known = true;
        s_event.start = startSeconds / 3600.0;
        s_event.minutes = uint32(length);
    }

    // Moves the event's daily window onto the sun (once the world is loaded; on the world thread). The core then starts
    // and stops it by its own check, as any event.
    void ApplyEventWindow()
    {
        if (!s_started || !s_event.known)
            return;
        GameEventMgr::GameEventDataMap const& events = sGameEventMgr->GetEventMap();
        if (s_eventId >= events.size() || !events[s_eventId].isValid() || events[s_eventId].State != GAMEEVENT_NORMAL)
            return;
        int64 const now = NowSeconds();
        ES::Window const want = EventWindowNow(now);
        // The manager hands its events out read-only and has no setter for an event's times; the events themselves are
        // its own (not const) data, and this runs where the manager's own update runs (the world thread).
        GameEventData& event = const_cast<GameEventData&>(events[s_eventId]);
        if (int64(event.Start) == want.start && event.Length == want.minutes)
            return;
        event.Start = time_t(want.start);
        event.Length = want.minutes;
        s_eventWindow = want;
        sWorld->ForceGameEventUpdate();
        LOG_INFO("module", "Emberfall sun: game event {} \"{}\": its window is {} to {} local time ({} for {} min on the sun clock), {}",
                 s_eventId, event.Description, LocalStamp(want.start), LocalStamp(want.end()), ES::ClockText(s_event.start), s_event.minutes,
                 sGameEventMgr->IsActiveEvent(uint16(s_eventId)) ? "running" : "not running");
    }

    // ---- the day -----------------------------------------------------------------------------------------------------------
    // Today's hours from the clock and the settings; true when they changed.
    bool Recompute()
    {
        std::tm const tm = Acore::Time::TimeBreakdown(time_t(NowSeconds()));
        s_today = DateOf(tm);
        s_offsetSeconds = OffsetAtNoon(s_today);
        uint32 const packed = Pack(ES::HoursFor(s_settings, s_today, s_offsetSeconds / 3600.0));
        return s_hours.exchange(packed) != packed;
    }

    std::string Describe()
    {
        efsun::SunHours const h = Unpack(s_hours.load());
        return Acore::StringFormat("{} at {:.2f} N {:.2f} E, {:04}-{:02}-{:02} (UTC{:+.2g}): sunrise {}, sunset {}", efsun::ModeName(s_settings.mode),
                                   s_settings.place.latitude, s_settings.place.longitude, s_today.year, s_today.month, s_today.day,
                                   s_offsetSeconds / 3600.0, ES::ClockText(h.sunrise), ES::ClockText(h.sunset));
    }

    // The day turned, or the settings changed: new hours to everyone, the event's window moved.
    uint32 Refresh(char const* why)
    {
        uint32 told = 0;
        if (Recompute())
        {
            told = s_started ? SendAll() : 0;
            if (s_started)
                LOG_INFO("module", "Emberfall sun: {}: {} ({} player(s) told)", why, Describe(), told);
        }
        ApplyEventWindow();
        return told;
    }

    void ReadConfig()
    {
        std::string mode = sConfigMgr->GetOption<std::string>("Emberfall.Sun.Mode", "real", false);
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (auto const known = efsun::ModeOf(mode))
            s_settings.mode = *known;
        else
        {
            LOG_ERROR("server.loading", ">> Emberfall sun: Emberfall.Sun.Mode = \"{}\" is not real, softened or fixed: real", mode);
            s_settings.mode = efsun::Mode::Real;
        }
        s_settings.place.latitude = sConfigMgr->GetOption<float>("Emberfall.Sun.Latitude", 47.37f, false);
        s_settings.place.longitude = sConfigMgr->GetOption<float>("Emberfall.Sun.Longitude", 8.54f, false);
        s_settings.place = ES::Sanitised(s_settings.place);
        s_settings.fixed.sunrise = sConfigMgr->GetOption<float>("Emberfall.Sun.FixedSunrise", float(efsun::kNominalSunrise), false);
        s_settings.fixed.sunset = sConfigMgr->GetOption<float>("Emberfall.Sun.FixedSunset", float(efsun::kNominalSunset), false);
        s_settings.fixed = efsun::Sanitised(s_settings.fixed);
        s_eventId = sConfigMgr->GetOption<uint32>("Emberfall.Sun.DaylightEvent", 200, false);
    }

    class emberfall_sun_world : public WorldScript
    {
    public:
        emberfall_sun_world()
            : WorldScript("emberfall_sun_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_UPDATE }) { }

        void OnAfterConfigLoad(bool reload) override
        {
            uint32 const eventBefore = s_eventId;
            ReadConfig();
            if (!reload)
            {
                Recompute();
                return;
            }
            if (s_started && s_eventId != eventBefore)
                LoadEvent();
            // A reload tells everyone again when anything changed (the mode's name too: the hours may be the same).
            if (!Refresh("config reloaded") && s_started)
                SendAll();
        }

        // The game events are loaded and started by now.
        void OnBeforeWorldInitialized() override
        {
            s_started = true;
            Recompute();
            LoadEvent();
            ApplyEventWindow();
            LOG_INFO("server.loading", ">> Emberfall sun: {} (EFSUN){}", Describe(),
                     s_event.known ? Acore::StringFormat("; game event {} follows the sun", s_eventId)
                                   : !s_eventId ? std::string("; no daylight event")
                                                : Acore::StringFormat("; game event {} is not in this world", s_eventId));
        }

        void OnUpdate(uint32 diff) override
        {
            s_tickMs += diff;
            if (s_tickMs < kTickMs)
                return;
            s_tickMs = 0;
            Refresh("the day turned");
        }
    };

    class emberfall_sun_player : public PlayerScript
    {
    public:
        emberfall_sun_player() : PlayerScript("emberfall_sun_player", { PLAYERHOOK_ON_LOGIN }) { }

        // With the login's first packets: the client lights its first frame by the day's hours.
        void OnPlayerLogin(Player* player) override { Send(player); }
    };

    class emberfall_sun_commands : public CommandScript
    {
    public:
        emberfall_sun_commands() : CommandScript("emberfall_sun_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable sunTable =
            {
                { "mode", HandleMode,   SEC_ADMINISTRATOR, Console::Yes },
                { "",     HandleStatus, SEC_GAMEMASTER,    Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "efsun", sunTable },
            };
            return commandTable;
        }

        static std::string Pair(efsun::SunHours h) { return ES::ClockText(h.sunrise) + " / " + ES::ClockText(h.sunset); }

        // `EFSUN ...` lines: the mode and the place, today's hours in use and in every mode, the clock and the sun clock
        // now with the part of the day, the daylight event.
        static bool HandleStatus(ChatHandler* handler)
        {
            Refresh("asked");
            ES::Settings other = s_settings;
            handler->PSendSysMessage("EFSUN {}", Describe());
            other.mode = efsun::Mode::Real;
            std::string const real = Pair(HoursOn(other, s_today));
            other.mode = efsun::Mode::Softened;
            std::string const softened = Pair(HoursOn(other, s_today));
            other.mode = efsun::Mode::Fixed;
            handler->PSendSysMessage("EFSUN today in each mode: real {}, softened {}, fixed {}", real, softened, Pair(HoursOn(other, s_today)));
            double const hour = LocalHourNow();
            handler->PSendSysMessage("EFSUN now {} is {} on the sun clock: {}", ES::ClockText(hour), ES::ClockText(ES::SunClockNow()),
                                     efsun::PartName(ES::PartNow()));
            if (s_event.known)
                handler->PSendSysMessage("EFSUN game event {} ({} for {} min on the sun clock): its window is {} to {} local time, {}",
                                         s_eventId, ES::ClockText(s_event.start), s_event.minutes, LocalStamp(s_eventWindow.start),
                                         LocalStamp(s_eventWindow.end()), sGameEventMgr->IsActiveEvent(uint16(s_eventId)) ? "running" : "not running");
            else
                handler->PSendSysMessage("EFSUN no daylight event (Emberfall.Sun.DaylightEvent = {})", s_eventId);
            return true;
        }

        // `.efsun mode real|softened|fixed`: switches the mode now for everyone; the config's value comes back with the
        // next config reload or restart.
        static bool HandleMode(ChatHandler* handler, std::string name)
        {
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            auto const mode = efsun::ModeOf(name);
            if (!mode)
            {
                handler->PSendSysMessage("EFSUN no mode \"{}\": real, softened or fixed", name);
                return true;
            }
            bool const same = *mode == s_settings.mode;
            s_settings.mode = *mode;
            uint32 told = Refresh("mode switched");
            if (!same && !told)
                told = SendAll();   // the same hours under another name (softened next to fixed at an equinox)
            handler->PSendSysMessage("EFSUN {}{} ({} player(s) told)", Describe(), same ? " (as before)" : "", told);
            LOG_INFO("module", "Emberfall sun: mode {} by command{}", efsun::ModeName(*mode), same ? " (as before)" : "");
            return true;
        }
    };
}

namespace Emberfall::Sun
{
    efsun::SunHours Hours() { return Unpack(s_hours.load(std::memory_order_relaxed)); }
    double SunClockNow() { return efsun::SunClock(LocalHourNow(), Hours()); }
    efsun::Part PartNow() { return efsun::PartOfSunHour(SunClockNow()); }
}

void AddEmberfallSunScripts()
{
    new emberfall_sun_world();
    new emberfall_sun_player();
    new emberfall_sun_commands();
}
