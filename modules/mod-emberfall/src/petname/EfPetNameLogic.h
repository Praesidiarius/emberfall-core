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

#ifndef EMBERFALL_PET_NAME_LOGIC_H
#define EMBERFALL_PET_NAME_LOGIC_H

// A new companion's default name (Emberfall docs/design/hunter-taming.md §17, feedback #579: "the fell goat name after
// taming is boar"), pure rules, no core types (tests/petname_tests.cpp). The core names a tamed beast after its
// CreatureFamily (Hound, Lynx, Boar, Hawk, Lizard: the companion kinds), so a fell goat filed under Boar became "Boar".
// Its default name is the creature's own name as the hunter knew it, in his language, each word's first letter
// capitalised ("fell goat" -> "Fell Goat", "scald-newt basker" -> "Scald-newt Basker"); a name longer than the pet name
// column (character_pet.name, 21 characters) keeps its last words that fit ("scald-newt pool-keeper" -> "Pool-keeper").

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::PetName
{
    constexpr std::size_t kMaxChars = 21;   // character_pet.name VARCHAR(21)

    // Characters (UTF-8 code points) in `s`.
    inline std::size_t Chars(std::string_view s)
    {
        std::size_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80)
                ++n;
        return n;
    }

    // Each space-separated word with its first letter capitalised (ASCII letters; others as they are).
    inline std::string TitleCase(std::string_view s)
    {
        std::string out(s);
        bool start = true;
        for (char& c : out)
        {
            if (c == ' ')
            {
                start = true;
                continue;
            }
            if (start && c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
            start = false;
        }
        return out;
    }

    // The default name for a creature called `creatureName` (its name in the hunter's language); "" when it has none.
    inline std::string DefaultName(std::string_view creatureName, std::size_t maxChars = kMaxChars)
    {
        std::vector<std::string_view> words;
        std::size_t i = 0;
        while (i < creatureName.size())
        {
            std::size_t const j = creatureName.find(' ', i);
            std::string_view const w = creatureName.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
            if (!w.empty())
                words.push_back(w);
            if (j == std::string_view::npos)
                break;
            i = j + 1;
        }
        // the last words that fit (at least the last word, cut to the limit)
        std::string name;
        for (std::size_t k = words.size(); k-- > 0;)
        {
            std::string const longer = name.empty() ? std::string(words[k]) : std::string(words[k]) + " " + name;
            if (Chars(longer) > maxChars && !name.empty())
                break;
            name = longer;
        }
        if (Chars(name) > maxChars)
        {
            std::size_t cut = 0, n = 0;
            for (; cut < name.size(); ++cut)
                if ((static_cast<unsigned char>(name[cut]) & 0xC0) != 0x80)
                {
                    if (n == maxChars)
                        break;
                    ++n;
                }
            name.resize(cut);
        }
        return TitleCase(name);
    }
}

#endif
