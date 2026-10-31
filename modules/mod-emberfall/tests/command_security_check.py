#!/usr/bin/env python3
"""GM level 1 runs no GM command (Emberfall docs/design/world-editor.md §19). Run by tests/run_tests.sh.

Level 1 (SEC_MODERATOR) is the world editor's contributor role on the normal realm: it must play exactly as a player.
AzerothCore takes a command's level from the world database's `command` table when it has a row (ChatCommand.cpp
LoadCommandMap: "Overriding", even over a C++ RBAC permission), else from the C++ command table. So this check builds
both, the way a fresh world database ends up:

- the C++ command tables of src/server/scripts and modules/*/src (`{ "name", Handler, SEC_x | rbac::..., Console::x }`
  and `{ "name", subTable }`), resolved to full names;
- the `command` rows: data/sql/base/db_world/command.sql, then data/sql/updates/db_world/*.sql (and
  pending_db_world), then each module's data/sql/db-world/*.sql, in name order, as dbimport applies them;

and fails when any command ends at level 1 other than `efedit ...`. An upstream merge that adds a level-1 row, or a
C++ SEC_MODERATOR command, fails here: lift it in a new mod-emberfall db-world update (the 2026-10-31 one is the
model). It also checks that the auth database gives level 1 the player's RBAC role only (rbac_default_permissions:
secId 1 -> 195), the other half of "plays as a player".

Exit 0 when clean, 1 with the offending commands listed. No database, standard library only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORE = HERE.parents[2]
LEVELS = {"SEC_PLAYER": 0, "SEC_MODERATOR": 1, "SEC_GAMEMASTER": 2, "SEC_ADMINISTRATOR": 3, "SEC_CONSOLE": 4}
EXEMPT = ("efedit",)
PLAYER_ROLE = 195


# ------------------------------------------------------------------------------------------------ SQL, small

def _tokens(sql: str):
    """Tokens of a MySQL script: ('s', text) strings, ('n', text) numbers, ('w', word), ('p', char); comments skipped."""
    i, n = 0, len(sql)
    while i < n:
        c = sql[i]
        if c.isspace():
            i += 1
        elif sql.startswith("--", i) or c == "#":
            j = sql.find("\n", i)
            i = n if j < 0 else j + 1
        elif sql.startswith("/*", i):
            j = sql.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "'\"":
            out, i = [], i + 1
            while i < n:
                if sql[i] == "\\" and i + 1 < n:
                    out.append(sql[i + 1])
                    i += 2
                elif sql[i] == c and i + 1 < n and sql[i + 1] == c:
                    out.append(c)
                    i += 2
                elif sql[i] == c:
                    i += 1
                    break
                else:
                    out.append(sql[i])
                    i += 1
            yield ("s", "".join(out))
        elif c == "`":
            j = sql.find("`", i + 1)
            yield ("w", sql[i + 1:j])
            i = j + 1
        elif c.isdigit() or (c == "-" and i + 1 < n and sql[i + 1].isdigit()):
            m = re.match(r"-?\d+(\.\d+)?", sql[i:])
            yield ("n", m.group(0))
            i += len(m.group(0))
        elif c.isalpha() or c == "_" or c == "@":
            m = re.match(r"[@\w.]+", sql[i:])
            yield ("w", m.group(0))
            i += len(m.group(0))
        else:
            yield ("p", c)
            i += 1


def statements(sql: str) -> list[list[tuple[str, str]]]:
    out, cur = [], []
    for t in _tokens(sql):
        if t == ("p", ";"):
            if cur:
                out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    return out


def _words(st, k: int) -> list[str]:
    return [v.upper() for kind, v in st[:k] if kind == "w"]


def _groups(st, start: int) -> list[list[tuple[str, str]]]:
    """The parenthesised groups from `start` on (VALUES (...), (...))."""
    groups, depth, cur = [], 0, []
    for t in st[start:]:
        if t == ("p", "("):
            depth += 1
            if depth == 1:
                cur = []
                continue
        elif t == ("p", ")"):
            depth -= 1
            if depth == 0:
                groups.append(cur)
                continue
        if depth >= 1:
            cur.append(t)
    return groups


def _values(group) -> list:
    vals, cur = [], None
    depth = 0
    for kind, v in group:
        if (kind, v) == ("p", "(") :
            depth += 1
        elif (kind, v) == ("p", ")"):
            depth -= 1
        elif (kind, v) == ("p", ",") and depth == 0:
            vals.append(cur)
            cur = None
            continue
        if kind == "s":
            cur = v
        elif kind == "n":
            cur = int(float(v))
        elif kind == "w" and cur is None:
            cur = None if v.upper() == "NULL" else v
    vals.append(cur)
    return vals


def _where_names(st) -> tuple[set[str] | None, list]:
    """`WHERE name = 'x'` / `name IN ('x', ...)`: the names; other conditions returned raw."""
    try:
        w = next(i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "WHERE")
    except StopIteration:
        return None, []
    cond = st[w + 1:]
    if len(cond) >= 3 and cond[0] == ("w", "name") and cond[1] == ("p", "=") and cond[2][0] == "s" and len(cond) == 3:
        return {cond[2][1]}, []
    if len(cond) >= 2 and cond[0] == ("w", "name") and cond[1][0] == "w" and cond[1][1].upper() == "IN":
        names = {v for k, v in cond[2:] if k == "s"}
        rest = [t for t in cond[2:] if t[0] not in ("s",) and t not in (("p", "("), ("p", ")"), ("p", ","))]
        return names, rest
    return None, cond


class Unmodelled(Exception):
    pass


def apply_command_sql(table: dict[str, int], sql: str, where: str) -> None:
    for st in statements(sql):
        w = _words(st, 4)
        if not st or "command" not in [v for k, v in st[:6] if k == "w"]:
            continue
        head = w[:2]
        if head in (["LOCK", "TABLES"], ["UNLOCK", "TABLES"]) or w[:1] in (["SET"], ["ALTER"]) or \
                (w[:2] == ["DROP", "TABLE"]) or (w[:2] == ["CREATE", "TABLE"]):
            if w[:2] == ["DROP", "TABLE"]:
                table.clear()
            continue
        if w[:1] in (["INSERT"], ["REPLACE"]):
            vi = next((i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "VALUES"), None)
            if vi is None:
                raise Unmodelled(f"{where}: INSERT into command without VALUES")
            cols = None
            pre = _groups(st[:vi], 0)
            if pre:
                cols = [v for k, v in pre[0] if k == "w"]
            on_dup = next((i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "DUPLICATE"), None)
            rows = _groups(st[vi + 1:on_dup] if on_dup else st[vi + 1:], 0)
            dup_security = None
            if on_dup:
                tail = st[on_dup:]
                m = [i for i, t in enumerate(tail) if t == ("w", "security")]
                if m and m[0] + 2 < len(tail):
                    val = tail[m[0] + 2]
                    if val[0] == "n":
                        dup_security = int(val[1])
                    elif val[0] == "w" and val[1].upper() == "VALUES":
                        dup_security = "values"
            for g in rows:
                vals = _values(g)
                rec = dict(zip(cols, vals)) if cols else {"name": vals[0], "security": vals[1]}
                name, sec = rec.get("name"), rec.get("security", 0)
                if name is None:
                    continue
                if name in table and on_dup:
                    if dup_security is not None:
                        table[name] = sec if dup_security == "values" else dup_security
                elif w[:1] == ["REPLACE"] or name not in table or "IGNORE" not in w:
                    if name in table and "IGNORE" in w:
                        continue
                    table[name] = int(sec or 0)
            continue
        if w[:2] == ["DELETE", "FROM"]:
            names, rest = _where_names(st)
            if names is None:
                if not any(k == "w" and v.upper() == "WHERE" for k, v in st):
                    table.clear()
                    continue
                raise Unmodelled(f"{where}: DELETE FROM command with a condition this check cannot model")
            for name in names:
                table.pop(name, None)
            continue
        if w[:1] == ["UPDATE"]:
            sets = [i for i, t in enumerate(st) if t == ("w", "security")]
            si = next((i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "SET"), None)
            wi = next((i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "WHERE"), len(st))
            if not sets or sets[0] > wi:
                continue   # help text only
            new = st[sets[0] + 2]
            if new[0] != "n" or si is None:
                raise Unmodelled(f"{where}: UPDATE command SET security to a non-number")
            new_level = int(new[1])
            names, rest = _where_names(st)
            if names is not None:
                for name in names:
                    if name in table:
                        table[name] = new_level
                continue
            # the generic lift: WHERE security = N [AND name NOT LIKE 'prefix%' ...]
            cond = st[wi + 1:]
            if len(cond) >= 3 and cond[0] == ("w", "security") and cond[1] == ("p", "=") and cond[2][0] == "n":
                old = int(cond[2][1])
                keep: list[str] = []
                j = 3
                while j < len(cond):
                    if cond[j][0] == "w" and cond[j][1].upper() == "AND" and cond[j + 1] == ("w", "name") and \
                            cond[j + 2][1].upper() == "NOT" and cond[j + 3][1].upper() == "LIKE" and cond[j + 4][0] == "s":
                        keep.append(cond[j + 4][1].rstrip("%"))
                        j += 5
                    else:
                        raise Unmodelled(f"{where}: UPDATE command ... WHERE security = {old} with a condition this check cannot model")
                for name, level in table.items():
                    if level == old and not any(name.startswith(p) for p in keep):
                        table[name] = new_level
                continue
            raise Unmodelled(f"{where}: UPDATE command with a condition this check cannot model")
        raise Unmodelled(f"{where}: a statement on command this check cannot model: {' '.join(v for _, v in st[:6])}")


def apply_rbac_default_sql(rows: set[tuple[int, int, int]], sql: str) -> None:
    for st in statements(sql):
        if "rbac_default_permissions" not in [v for k, v in st[:6] if k == "w"]:
            continue
        w = _words(st, 4)
        if w[:2] == ["DROP", "TABLE"]:
            rows.clear()
        elif w[:1] in (["INSERT"], ["REPLACE"]):
            vi = next(i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "VALUES")
            for g in _groups(st[vi + 1:], 0):
                vals = _values(g)
                rows.add((int(vals[0]), int(vals[1]), int(vals[2]) if len(vals) > 2 and vals[2] is not None else -1))
        elif w[:2] == ["DELETE", "FROM"]:
            cond = st[next(i for i, (k, v) in enumerate(st) if k == "w" and v.upper() == "WHERE") + 1:]
            crit = {}
            j = 0
            while j + 2 < len(cond):
                if cond[j][0] == "w" and cond[j + 1] == ("p", "=") and cond[j + 2][0] == "n":
                    crit[cond[j][1]] = int(cond[j + 2][1])
                j += 1
            for r in list(rows):
                if all({"secId": r[0], "permissionId": r[1], "realmId": r[2]}.get(k) == v for k, v in crit.items()):
                    rows.discard(r)


# ------------------------------------------------------------------------------------------------ C++

ENTRY = re.compile(r'\{\s*"([^"]*)"\s*,\s*([\w:]+)\s*(?:,\s*([\w:]+)\s*)?(?:,\s*[\w:]+\s*)?\}')
TABLE = re.compile(r'ChatCommandTable\s+(\w+)\s*=\s*\{', re.S)


def cpp_commands(root: Path) -> dict[str, int | str]:
    """{full name: level (0-4) or 'rbac'} of every C++ command table under root."""
    out: dict[str, int | str] = {}
    for path in sorted(list((root / "src" / "server" / "scripts").rglob("*.cpp")) +
                       list((root / "modules").glob("*/src/**/*.cpp"))):
        text = path.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"//[^\n]*", "", text)
        tables: dict[str, list[tuple[str, str, str | None]]] = {}
        for m in TABLE.finditer(text):
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                i += 1
            body = text[m.end():i - 1]
            tables[m.group(1)] = [(e.group(1), e.group(2), e.group(3)) for e in ENTRY.finditer(body)]
        referenced = {target for entries in tables.values() for _, target, sec in entries if sec is None and target in tables}

        def walk(name: str, prefix: str, seen: frozenset):
            for sub, target, sec in tables.get(name, []):
                full = (prefix + " " + sub).strip() if sub else prefix
                if sec is None and target in tables and target not in seen:
                    walk(target, full, seen | {target})
                elif sec is not None:
                    out[full] = LEVELS.get(sec, "rbac" if sec.startswith("rbac::") else sec)

        for root_table in tables:
            if root_table not in referenced:
                walk(root_table, "", frozenset({root_table}))
    return out


def sql_files(core: Path, db: str, table: str) -> list[tuple[str, str]]:
    """(name, text) of the scripts that touch `table`, in dbimport's order: base, updates, pending, then each module's
    db-<x> directory. A file that never names the table is skipped unread by the tokenizer."""
    base = sorted((core / "data" / "sql" / "base" / f"db_{db}").glob(f"{table}.sql"))
    updates = sorted((core / "data" / "sql" / "updates" / f"db_{db}").glob("*.sql"))
    pending = sorted((core / "data" / "sql" / "updates" / f"pending_db_{db}").glob("*.sql"))
    modules = sorted((core / "modules").glob(f"*/data/sql/db-{db}/*.sql"))
    pattern = re.compile(r"[`\s]" + re.escape(table) + r"[`\s(]")
    out = []
    for f in base + updates + pending + modules:
        text = f.read_text(encoding="utf-8", errors="replace")
        if pattern.search(text):
            out.append((f.name, text))
    return out


def check(core: Path = CORE) -> list[str]:
    problems: list[str] = []
    table: dict[str, int] = {}
    for name, text in sql_files(core, "world", "command"):
        try:
            apply_command_sql(table, text, name)
        except Unmodelled as e:
            problems.append(str(e))
    cpp = cpp_commands(core)
    if len(cpp) < 100 or len(table) < 100:
        problems.append(f"found only {len(cpp)} C++ commands and {len(table)} command rows: the parsers no longer match the core")
    effective = dict((n, lvl) for n, lvl in cpp.items())
    effective.update(table)   # a row overrides the C++ level (LoadCommandMap)
    for name, level in sorted(effective.items()):
        if level == 1 and not any(name == p or name.startswith(p + " ") for p in EXEMPT):
            src = "the command table" if name in table else "C++ (SEC_MODERATOR)"
            problems.append(f"'{name}' is at GM level 1 ({src}): level 1 is the world editor's contributor and runs no GM command")
    rbac: set[tuple[int, int, int]] = set()
    for _, text in sql_files(core, "auth", "rbac_default_permissions"):
        apply_rbac_default_sql(rbac, text)
    level1 = sorted(r[1] for r in rbac if r[0] == 1)
    if level1 != [PLAYER_ROLE]:
        problems.append(f"rbac_default_permissions gives GM level 1 {level1}, not only the player's role {PLAYER_ROLE}")
    return problems


def main() -> int:
    problems = check()
    if problems:
        print("command security: GM level 1 must play as a player (world-editor.md §19):", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1
    print("command security: ok (no GM command at level 1 but .efedit; level 1 has the player's RBAC role)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
