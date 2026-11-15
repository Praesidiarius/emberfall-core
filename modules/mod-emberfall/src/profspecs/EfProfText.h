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

// Emberfall: the profession trainer's lines about talents and specialisations (Emberfall docs/design/professions.md
// §28) in the client's language (enUS, deDE, frFR; the Lang of the one-profession mail). Pure, so
// tests/profspec_tests.cpp checks them without a server. Names (a tree's, a specialisation's) come in the language
// already (ef_prof_text).

#pragma once

#include "../professions/EfOneProfession.h"

#include <cstdint>
#include <string>

namespace ef::prof::text
{
    using Emberfall::Professions::Lang;

    // "1,234" / "1.234" / "1 234": thousands as each language writes them.
    inline std::string Thousands(uint64_t v, Lang lang)
    {
        std::string digits = std::to_string(v), out;
        char const* sep = lang == Lang::De ? "." : lang == Lang::Fr ? "\xC2\xA0" : ",";   // frFR: a no-break space
        for (size_t i = 0; i < digits.size(); ++i)
        {
            if (i && (digits.size() - i) % 3 == 0)
                out += sep;
            out += digits[i];
        }
        return out;
    }

    // A price in copper as words: "10 gold", "1 gold 50 silver", "25 copper".
    inline std::string Money(uint64_t copper, Lang lang)
    {
        uint64_t const g = copper / 10000, s = copper / 100 % 100, c = copper % 100;
        char const* gw = lang == Lang::De ? " Gold" : lang == Lang::Fr ? "\xC2\xA0pi\xC3\xA8" "ces d'or" : " gold";
        char const* sw = lang == Lang::De ? " Silber" : lang == Lang::Fr ? "\xC2\xA0pi\xC3\xA8" "ces d'argent" : " silver";
        char const* cw = lang == Lang::De ? " Kupfer" : lang == Lang::Fr ? "\xC2\xA0pi\xC3\xA8" "ces de cuivre" : " copper";
        std::string out;
        auto add = [&](uint64_t v, char const* w) {
            if (!v)
                return;
            if (!out.empty())
                out += ' ';
            out += Thousands(v, lang) + w;
        };
        add(g, gw);
        add(s, sw);
        add(c, cw);
        return out.empty() ? Thousands(0, lang) + cw : out;
    }

    inline std::string Points(uint32_t n, Lang lang)
    {
        if (lang == Lang::De)
            return std::to_string(n) + (n == 1 ? " Punkt" : " Punkte");
        return std::to_string(n) + (n == 1 ? " point" : " points");   // frFR as English
    }

    // The trainer's line that unlearns a talent tree, and its confirmation.
    inline std::string UnlearnTreeLine(Lang lang, std::string const& tree, uint32_t points)
    {
        switch (lang)
        {
            case Lang::De: return "Talente verlernen: " + tree + " (" + Points(points, lang) + ")";
            case Lang::Fr: return "D\xC3\xA9" "sapprendre des talents\xC2\xA0: " + tree + " (" + Points(points, lang) + ")";
            default: return "Unlearn talents: " + tree + " (" + Points(points, lang) + ")";
        }
    }
    inline std::string UnlearnTreeBox(Lang lang, std::string const& tree, uint32_t points)
    {
        switch (lang)
        {
            case Lang::De:
                return "Das nimmt alle " + Points(points, lang) + " aus " + tree + " zur\xC3\xBC" "ck. Die anderen B\xC3\xA4ume behalten "
                       "ihre; ein Baum wird nur ganz verlernt, nie ein einzelner Knoten.";
            case Lang::Fr:
                return "Cela rend les " + Points(points, lang) + " de " + tree + ". Les autres arbres gardent les leurs\xC2\xA0; un "
                       "arbre se d\xC3\xA9" "sapprend en entier, jamais un n\xC5\x93ud.";
            default:
                return "This takes back all " + Points(points, lang) + " spent in " + tree + ". The other trees keep theirs; a tree is "
                       "unlearned whole, never a node.";
        }
    }

    // The trainer's line that picks a specialisation, and its confirmation (`what` its own text: what it makes).
    inline std::string PickLine(Lang lang, std::string const& name)
    {
        switch (lang)
        {
            case Lang::De: return "Spezialisierung w\xC3\xA4hlen: " + name;
            case Lang::Fr: return "Choisir votre sp\xC3\xA9" "cialisation\xC2\xA0: " + name;
            default: return "Choose your specialisation: " + name;
        }
    }
    inline std::string PickBox(Lang lang, std::string const& what)
    {
        switch (lang)
        {
            case Lang::De:
                return what + "\n\nDu kannst sie sp\xC3\xA4ter bei einem Lehrer wechseln, gegen einen Preis, der jedes Mal steigt.";
            case Lang::Fr:
                return what + "\n\nVous pourrez en changer plus tard aupr\xC3\xA8s d'un ma\xC3\xAEtre, pour un prix qui augmente \xC3\xA0 "
                       "chaque fois.";
            default:
                return what + "\n\nYou can change it later at a trainer, for a price that rises each time.";
        }
    }

    // The line that gives a specialisation up, and its confirmation: the price and what is lost.
    inline std::string DropLine(Lang lang, std::string const& name)
    {
        switch (lang)
        {
            case Lang::De: return "Spezialisierung aufgeben: " + name;
            case Lang::Fr: return "Abandonner la sp\xC3\xA9" "cialisation\xC2\xA0: " + name;
            default: return "Give up your specialisation: " + name;
        }
    }
    inline std::string DropBox(Lang lang, std::string const& name, std::string const& craft, uint64_t copper, uint32_t backTo)
    {
        std::string const price = Money(copper, lang), back = std::to_string(backTo);
        switch (lang)
        {
            case Lang::De:
                return "Das kostet " + price + ". Deine Fertigkeit in " + craft + " f\xC3\xA4llt auf " + back + " zur\xC3\xBC" "ck, und die "
                       "Talentpunkte dar\xC3\xBC" "ber gehen verloren. Die Rezepte von " + name + " bleiben in deinem Buch, aber du kannst "
                       "sie nicht mehr herstellen. Danach w\xC3\xA4hlst du neu und steigst den Rang noch einmal auf.";
            case Lang::Fr:
                return "Cela co\xC3\xBBte " + price + ". Votre comp\xC3\xA9tence en " + craft + " redescend \xC3\xA0 " + back + ", et les "
                       "points de talent au-del\xC3\xA0 sont perdus. Les recettes de " + name + " restent dans votre livre, mais vous ne "
                       "pouvez plus les fabriquer. Ensuite, vous choisissez \xC3\xA0 nouveau et remontez le rang.";
            default:
                return "This costs " + price + ". Your " + craft + " skill goes back to " + back + ", and the talent points above it are "
                       "lost. The " + name + " recipes stay in your book, but you cannot make them. Then you choose again and level "
                       "the rank again.";
        }
    }

    // What the chat says after a pick or a change.
    inline std::string Picked(Lang lang, std::string const& name)
    {
        switch (lang)
        {
            case Lang::De: return "Deine Spezialisierung: " + name + ". Dein Lehrer bringt dir ihre Rezepte bei.";
            case Lang::Fr: return "Votre sp\xC3\xA9" "cialisation\xC2\xA0: " + name + ". Votre ma\xC3\xAEtre vous enseigne ses recettes.";
            default: return "Your specialisation: " + name + ". Your trainer teaches you its recipes.";
        }
    }
    inline std::string Dropped(Lang lang, std::string const& name, std::string const& craft, uint64_t copper, uint32_t backTo)
    {
        std::string const price = Money(copper, lang), back = std::to_string(backTo);
        switch (lang)
        {
            case Lang::De:
                return "Du hast " + name + " f\xC3\xBCr " + price + " aufgegeben. " + craft + " steht wieder auf " + back +
                       ". W\xC3\xA4hle bei einem Lehrer eine neue Spezialisierung.";
            case Lang::Fr:
                return "Vous avez abandonn\xC3\xA9 " + name + " pour " + price + ". " + craft + " est revenu \xC3\xA0 " + back +
                       ". Choisissez une nouvelle sp\xC3\xA9" "cialisation aupr\xC3\xA8s d'un ma\xC3\xAEtre.";
            default:
                return "You gave up " + name + " for " + price + ". " + craft + " is back at " + back +
                       ". Choose a new specialisation at a trainer.";
        }
    }
    // A last-rank recipe refused for want of its specialisation.
    inline std::string NeedsSpec(Lang lang, std::string const& name)
    {
        switch (lang)
        {
            case Lang::De: return "Daf\xC3\xBCr brauchst du die Spezialisierung " + name + ". W\xC3\xA4hle sie bei einem Lehrer.";
            case Lang::Fr: return "Il faut la sp\xC3\xA9" "cialisation " + name + ". Choisissez-la aupr\xC3\xA8s d'un ma\xC3\xAEtre.";
            default: return "This needs the " + name + " specialisation. Choose it at a trainer.";
        }
    }

    inline std::string NotEnoughMoney(Lang lang, uint64_t copper)
    {
        std::string const price = Money(copper, lang);
        switch (lang)
        {
            case Lang::De: return "Daf\xC3\xBCr brauchst du " + price + ".";
            case Lang::Fr: return "Il vous faut " + price + ".";
            default: return "You need " + price + " for that.";
        }
    }
}
