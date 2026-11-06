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

// Emberfall: one main profession (Emberfall docs/design/professions.md §25, Nathanael 2026-11-06). A character learns
// one main craft (a category-11 skill line: Alchemy, Blacksmithing, Leatherworking, Tailoring, Enchanting); Mining,
// Herbalism and Skinning are secondary. A character from before keeps its best craft once and is told by mail. Pure:
// which crafts go, and the mail's text in the client's language. No core types, so tests/oneprofession_tests.cpp runs
// it without a server.

#ifndef EF_ONE_PROFESSION_H
#define EF_ONE_PROFESSION_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Emberfall::Professions
{
    struct Craft
    {
        uint32_t skill = 0;   // the skill line
        uint32_t value = 0;   // the skill (without bonuses)
        uint32_t max = 0;     // the rank's cap: 75, 175, 325, 500
    };

    // The order a character keeps its crafts in: the higher skill first; on a tie the higher rank (cap); then the
    // lower skill line ID (the core keeps no date a profession was learned, so "learned first" cannot be told; the ID
    // makes it deterministic).
    inline bool Better(Craft const& a, Craft const& b)
    {
        if (a.value != b.value)
            return a.value > b.value;
        if (a.max != b.max)
            return a.max > b.max;
        return a.skill < b.skill;
    }

    // The crafts a character with `crafts` keeps under a limit of `limit`, best first, and the ones it loses, best
    // first. Nothing goes while the crafts fit the limit.
    struct Cut
    {
        std::vector<Craft> keep, drop;
    };

    inline Cut CutTo(std::vector<Craft> crafts, uint32_t limit)
    {
        std::sort(crafts.begin(), crafts.end(), Better);
        Cut out;
        for (Craft const& c : crafts)
            (out.keep.size() < limit ? out.keep : out.drop).push_back(c);
        return out;
    }

    // The client's locale (the core's LocaleConstant numbers): enUS 0, frFR 2, deDE 3; everything else reads English.
    enum class Lang : uint8_t { En, Fr, De };
    inline Lang LangOf(uint32_t locale) { return locale == 3 ? Lang::De : locale == 2 ? Lang::Fr : Lang::En; }

    // A main craft's name in the language (the stock IDs Emberfall keeps); empty for another line (the caller takes
    // the DBC's English name).
    inline std::string CraftName(uint32_t skill, Lang lang)
    {
        struct Name { uint32_t skill; char const* en; char const* de; char const* fr; };
        static Name const names[] = {
            { 171, "Alchemy", "Alchemie", "Alchimie" },
            { 164, "Blacksmithing", "Schmiedekunst", "Forge" },
            { 165, "Leatherworking", "Lederverarbeitung", "Travail du cuir" },
            { 197, "Tailoring", "Schneiderei", "Couture" },
            { 333, "Enchanting", "Verzauberkunst", "Enchantement" },
        };
        for (Name const& n : names)
            if (n.skill == skill)
                return lang == Lang::De ? n.de : lang == Lang::Fr ? n.fr : n.en;
        return {};
    }

    // "A", "A and B", "A, B and C" in the language.
    inline std::string JoinNames(std::vector<std::string> const& names, Lang lang)
    {
        char const* const conj = lang == Lang::De ? " und " : lang == Lang::Fr ? " et " : " and ";
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i)
                out += i + 1 == names.size() ? conj : ", ";
            out += names[i];
        }
        return out;
    }

    struct Letter
    {
        std::string subject, body;
    };

    // The mail a character gets when its crafts were cut: `kept` the craft it keeps, `dropped` the ones unlearned
    // (names in the language).
    inline Letter MailFor(Lang lang, std::string const& kept, std::string const& dropped)
    {
        switch (lang)
        {
            case Lang::De:
                return { "Nur noch ein Hauptberuf",
                         "Sei gegrüßt,\n\n"
                         "ab sofort erlernt jeder in Orrin nur noch einen Hauptberuf: Alchemie, Schmiedekunst, "
                         "Lederverarbeitung, Schneiderei oder Verzauberkunst. Bergbau, Kräuterkunde und Kürschnerei "
                         "sind jetzt Nebenberufe wie Kochkunst, Erste Hilfe und Angeln: Jeder kann sie neben seinem "
                         "Hauptberuf erlernen.\n\n"
                         "Du hattest mehr als ein Handwerk erlernt. Du behältst " + kept + ", dein höchstes, und hast " +
                         dropped + " verlernt. Deine Sammelberufe bleiben, wie sie waren.\n\n"
                         "Möchtest du lieber ein anderes Handwerk ausüben, verlerne " + kept + " im Fenster „Berufe“ (J) "
                         "und besuche den Lehrer des neuen Handwerks." };
            case Lang::Fr:
                return { "Un seul métier principal",
                         "Salutations,\n\n"
                         "désormais, chacun en Orrin n'apprend plus qu'un seul métier principal : Alchimie, Forge, "
                         "Travail du cuir, Couture ou Enchantement. Minage, Herboristerie et Dépeçage sont devenus des "
                         "métiers secondaires, comme Cuisine, Secourisme et Pêche : tout le monde peut les apprendre en "
                         "plus de son métier principal.\n\n"
                         "Vous connaissiez plus d'un artisanat. Vous gardez " + kept + ", le plus avancé, et avez désappris " +
                         dropped + ". Vos métiers de récolte restent tels quels.\n\n"
                         "Si vous préférez exercer un autre artisanat, désapprenez " + kept + " dans la fenêtre des "
                         "métiers (J) et rendez visite au maître de ce nouvel artisanat." };
            default:
                return { "One main profession",
                         "Greetings,\n\n"
                         "from now on everyone in Orrin learns one main profession: Alchemy, Blacksmithing, "
                         "Leatherworking, Tailoring or Enchanting. Mining, Herbalism and Skinning are now secondary "
                         "professions, like Cooking, First Aid and Fishing: anyone may learn them beside their main "
                         "profession.\n\n"
                         "You knew more than one craft. You keep " + kept + ", your highest, and have unlearned " + dropped +
                         ". Your gathering professions stay as they were.\n\n"
                         "If you would rather practise another craft, unlearn " + kept + " in your professions window (J) "
                         "and visit that craft's trainer." };
        }
    }
}

#endif
