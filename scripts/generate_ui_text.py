r"""The launcher's text in Chinese: the catalogs src/ui_text.cppm translates drawn text with.

src/text/zh-CN.tsv holds one `english<TAB>chinese` row per string the public sources draw (tab,
newline and backslash escaped as \t \n \\; lines starting "# " are comments; an empty Chinese keeps
the English). This writes src/text/zh_cn.hpp, ASCII only with the Chinese as octal escapes, so no
source file holds Chinese. private/text/zh-CN.tsv does the same for private/ (the public export
removes that folder), into private/text/zh_cn.hpp, which the private extension hands to
gw2::text::addCatalog. Text only the dev launcher draws (code under KX_DEV_TOOLS or
KX_LAUNCHER_UPDATE_GIT) has its rows in private/text/zh-CN-dev.tsv instead: the header compiles
those rows only into KX_DEV_TOOLS builds, so player builds never carry dev strings such as the
private repository's address. Each run checks that every private row sits in the right file.

An English string with printf conversions ("%u accounts") or built with + ("Signed in as " +
name: "Signed in as %s") is a pattern: the Chinese takes the same number of values, in order, or
names them ("%2$s"). A key is the text as ImGui draws it: no "##id" part, no blanks around it.

usage: py -3 -I scripts/generate_ui_text.py [--check] [--missing OUT]
  --check    fail when a generated header is out of date or a row is malformed
  --missing  also write the strings found in source that have no row yet (OUT, TSV: english,
             empty, catalog, first place, source line) for translating; catalog "private-dev"
             means the row belongs in private/text/zh-CN-dev.tsv
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PRIVATE = os.path.join(ROOT, "private")
CATALOGS = {
    # name: (TSV, header, C++ namespace, source folders)
    "public": (os.path.join(ROOT, "src", "text", "zh-CN.tsv"), os.path.join(ROOT, "src", "text", "zh_cn.hpp"),
               "gw2::text", [os.path.join(ROOT, d) for d in ("src", "native", "auth")]),
    "private": (os.path.join(PRIVATE, "text", "zh-CN.tsv"), os.path.join(PRIVATE, "text", "zh_cn.hpp"),
                "kx::text", [PRIVATE]),
}
# Rows only KX_DEV_TOOLS builds compile (see the module docstring).
DEV_TSV = {"private": os.path.join(PRIVATE, "text", "zh-CN-dev.tsv")}
DEV_MACRO = "KX_DEV_TOOLS"
# Conditions whose code only dev launcher builds compile (Git updates require KX_DEV_TOOLS).
DEV_CONDITIONS = {"defined(KX_DEV_TOOLS)", "defined(KX_LAUNCHER_UPDATE_GIT)", "KX_DEV_TOOLS"}
SKIP_DIRS = {"tests", "scripts", "redist", "theme", "docs", "text", "icons", "__pycache__"}
# The dev launcher's R2 file browser (KX_DEV_TOOLS): no player build has it.
SKIP_FILES = {"R2Client.cppm", "Files.cppm"}

SPEC = re.compile(r"%(?:%|(?:\d+\$)?[-+ #0]*[\d*]*(?:\.[\d*]+)?(?:hh|h|ll|l|j|z|t|L)?[diouxXeEfFgGaAcsp])"
                  r"|\{\{|\}\}|\{\d*(?::[^{}]*)?\}")


def unescape(text):
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            out.append({"t": "\t", "n": "\n", "\\": "\\", "#": "#"}.get(text[i + 1], "\\" + text[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def escape(text):
    out = text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
    return "\\" + out if out.startswith("#") else out


def values(text):
    return [m for m in SPEC.findall(text) if m not in ("%%", "{{", "}}")]


def load_tsv(path):
    rows, problems, seen = [], [], set()
    if not os.path.isfile(path):
        return rows, problems
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n").rstrip("\r")
            if not line or line == "#" or line.startswith("# "):
                continue
            english, sep, chinese = line.partition("\t")
            if not sep:
                problems.append(f"{path}:{n}: no tab")
                continue
            english, chinese = unescape(english), unescape(chinese)
            if english != english.strip() or "##" in english:
                problems.append(f"{path}:{n}: key has blanks around it or an ##id: {english!r}")
            if english in seen:
                problems.append(f"{path}:{n}: duplicate key {english!r}")
            seen.add(english)
            if chinese:
                want, got = values(english), values(chinese)
                positional = [v for v in got if "$" in v or re.match(r"\{\d", v)]
                prose = not got and all(v.startswith("% ") for v in want)  # "5% done"
                if want and not prose and (len(got) != len(want) or (positional and len(positional) != len(got))):
                    problems.append(f"{path}:{n}: {len(want)} values in the English, {len(got)} in the Chinese: {english!r}")
            rows.append((english, chinese))
    return rows, problems


def c_string(text):
    out, octal = [], False
    for b in text.encode("utf-8"):
        c = chr(b)
        if octal and c.isdigit():  # a digit right after an octal escape: escaped too, or MSVC warns (C4125)
            out.append("\\%03o" % b)
            continue
        octal = False
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif 0x20 <= b < 0x7F and c != "?":
            out.append(c)
        else:
            out.append("\\%03o" % b)
            octal = True
    return '"' + "".join(out) + '"'


def render(rows, tsv, namespace, dev_rows=(), dev_tsv=None):
    rel = os.path.relpath(tsv, ROOT).replace("\\", "/")
    source = rel if not dev_tsv else rel + " and " + os.path.relpath(dev_tsv, ROOT).replace("\\", "/")
    lines = [
        f"// Generated by scripts/generate_ui_text.py from {source}; do not edit.",
        f"// {len(rows)} strings with their Chinese, \"\" for one kept English (octal escapes keep this file ASCII).",
    ]
    if dev_tsv:
        lines.append(f"// {len(dev_rows)} more only in {DEV_MACRO} builds: player builds carry no dev launcher text.")
    lines += [
        "#pragma once",
        "#include <utility>",
        f"namespace {namespace} {{",
        "inline constexpr std::pair<const char *, const char *> zhCn[] = {",
    ]
    for english, chinese in sorted(rows):
        lines.append(f"    {{{c_string(english)}, {c_string(chinese)}}},")
    if not rows:
        lines.append('    {"", ""},')
    if dev_rows:
        lines.append(f"#if defined({DEV_MACRO})")
        for english, chinese in sorted(dev_rows):
            lines.append(f"    {{{c_string(english)}, {c_string(chinese)}}},")
        lines.append("#endif")
    lines += ["};", "}", ""]
    return "\n".join(lines)


# ---- finding UI strings in source -------------------------------------------------------
# Text that never reaches the screen: includes, asserts, environment and registry names, ids.
SKIP_LINE = re.compile(r"\b(static_assert|IM_ASSERT|assert|getenv|_wgetenv|GetEnvironmentVariable\w*|PushID|GetID|"
                       r"LoadLibrary\w*|GetProcAddress|GetModuleHandle\w*|RegOpenKey\w*|RegGetValue\w*|"
                       r"RegQueryValue\w*|SDL_SetHint|SDL_GetHint|setenv|putenv)\b|^\s*#\s*(include|pragma|define|if|elif)\b")
JSON_BEFORE = re.compile(r'(\[\s*$|\.(value|contains|at|find|erase|count|emplace|get|starts_with|ends_with)\(\s*$)')


def c_unescape(text):
    try:
        return bytes(text, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
    except (UnicodeDecodeError, UnicodeEncodeError):
        return text


def ui_key(text):
    """The key of a drawn string: the text before its "##id", no blanks around it; None when it
    holds no English to translate (fewer than two letters)."""
    text = text.split("##", 1)[0].strip()
    letters = sum(c.isascii() and c.isalpha() for c in SPEC.sub("", text))
    if letters < 2 or len(text) > 4000:
        return None
    return text


def blank_comments(source):
    """The source with comments, character literals and raw strings blanked (lines kept), and the
    spans of its string literals: (start, end, text)."""
    out, spans, i, n = list(source), [], 0, len(source)

    def blank(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = source[i]
        if source.startswith("//", i):
            j = source.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
            continue
        if source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
            continue
        if c == "'":
            k = i - 1
            while k >= 0 and (source[k].isalnum() or source[k] == "_"):
                k -= 1
            if source[k + 1:i][:1].isdigit():  # 1'000'000
                i += 1
                continue
            j = i + 1
            while j < n and source[j] != "'" and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            blank(i, j + 1)
            i = j + 1
            continue
        if c == "R" and source.startswith('R"', i) and (i == 0 or not (source[i - 1].isalnum() or source[i - 1] == "_")):
            d = source.find("(", i + 2)
            close = ")" + source[i + 2:d] + '"'
            j = source.find(close, d)
            j = n if j < 0 else j + len(close)
            blank(i, j)
            i = j
            continue
        if c == '"':
            j = i + 1
            while j < n and source[j] != '"' and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            start = i - 2 if source[i - 2:i] == "u8" else i
            spans.append((start, j + 1, source[i + 1:j]))
            i = j + 1
            continue
        i += 1
    return "".join(out), spans


def literal_groups(source):
    """(text, start, end) of each string the compiler builds: adjacent literals joined."""
    clean, spans = blank_comments(source)
    groups = []
    for start, end, text in spans:
        if groups and clean[groups[-1][2]:start].strip() == "":
            t, s, _ = groups[-1]
            groups[-1] = (t + text, s, end)
        else:
            groups.append((text, start, end))
    return clean, groups


def ident(clean, k):
    """Whether clean[k] belongs to a name or member access (a::b, a.b, a->b)."""
    c = clean[k]
    return c.isalnum() or c in "_:." or clean[k:k + 2] == "->" or clean[k - 1:k + 1] == "->"


def skip_bracket(clean, k, step):
    """From the bracket at k, the index just past its partner, walking by step (+1 or -1)."""
    opening, closing = ("([", ")]") if step > 0 else (")]", "([")
    depth = 0
    while 0 <= k < len(clean):
        if clean[k] in opening:
            depth += 1
        elif clean[k] in closing:
            depth -= 1
            if depth == 0:
                return k + step
        k += step
    return k


def operand_left(clean, i, by_end):
    """The operand that ends before clean[i]: (its start, literal text or None)."""
    j = i
    while j > 0 and clean[j - 1].isspace():
        j -= 1
    if j in by_end:
        return by_end[j][1], by_end[j][0]
    k = j - 1
    while k >= 0:
        if clean[k] in ")]":
            k = skip_bracket(clean, k, -1)
        elif ident(clean, k):
            k -= 1
        else:
            break
    return k + 1, None


def operand_right(clean, i, by_start):
    """The operand that starts at or after clean[i]: (its end, literal text or None, literal start)."""
    j = i
    while j < len(clean) and clean[j].isspace():
        j += 1
    if j in by_start:
        return by_start[j][2], by_start[j][0], j
    m = re.match(r'std::string\s*[({]\s*', clean[j:])  # std::string("text")
    if m and j + m.end() in by_start:
        g = by_start[j + m.end()]
        k = g[2]
        while k < len(clean) and clean[k].isspace():
            k += 1
        if k < len(clean) and clean[k] in ")}":
            return k + 1, g[0], g[1]
    k = j
    while k < len(clean):
        if clean[k] in "([":
            k = skip_bracket(clean, k, 1)
        elif ident(clean, k):
            k += 1
        else:
            break
    return k, None, None


def plus_at(clean, i, forward):
    """The index of a lone binary + right after (forward) or before position i, else -1."""
    j = i
    if forward:
        while j < len(clean) and clean[j] in " \t\r\n":
            j += 1
        return j if j < len(clean) and clean[j] == "+" and clean[j + 1:j + 2] not in ("+", "=") else -1
    while j > 0 and clean[j - 1] in " \t\r\n":
        j -= 1
    return j - 1 if j > 0 and clean[j - 1] == "+" and clean[j - 2:j - 1] not in ("+", "=") else -1


def concatenations(clean, groups):
    """Text joined with + from literals and values: (pattern, starts of the literals used, start)."""
    by_start = {g[1]: g for g in groups}
    by_end = {g[2]: g for g in groups}
    seen, out = set(), []
    for g in groups:
        if g[1] in seen or (plus_at(clean, g[2], True) < 0 and plus_at(clean, g[1], False) < 0):
            continue
        parts, used = [g[0]], {g[1]}
        pos = g[1]
        while (p := plus_at(clean, pos, False)) >= 0:
            start, lit = operand_left(clean, p, by_end)
            if start >= p:
                break
            parts.insert(0, lit)
            if lit is not None:
                used.add(start)
            pos = start
        pos = g[2]
        while (p := plus_at(clean, pos, True)) >= 0:
            end, lit, lit_start = operand_right(clean, p + 1, by_start)
            if end <= p + 1:
                break
            parts.append(lit)
            if lit is not None:
                used.add(lit_start)
            pos = end
        seen |= used
        if any(p is None for p in parts) and any(p for p in parts if p):
            text = "".join("%s" if p is None else c_unescape(p).replace("%", "%%") for p in parts)
            out.append((re.sub(r"(%s)+", "%s", text), used, g[1]))
    return out


def scan_file(found, rel, source):
    lines = source.split("\n")
    clean, groups = literal_groups(source)

    def line_of(pos):
        return clean.count("\n", 0, pos) + 1

    joined = concatenations(clean, groups)
    in_chain = set()
    for text, used, start in joined:
        in_chain |= used
        ln = line_of(start)
        if not SKIP_LINE.search(lines[ln - 1]):
            add(found, text, f"{rel}:{ln}", lines[ln - 1])
    for text, start, end in groups:
        ln = line_of(start)
        line = lines[ln - 1]
        before = clean[clean.rfind("\n", 0, start) + 1:start]
        if start in in_chain or SKIP_LINE.search(line) or JSON_BEFORE.search(before):
            continue
        for item in c_unescape(text).split("\0"):  # a Combo list "A\0B\0" shows each item
            add(found, item, f"{rel}:{ln}", line)


def add(found, text, place, code):
    key = ui_key(text)
    if key and key not in found:
        found[key] = (place, code.strip()[:200])


DIRECTIVE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")


def dev_condition(expr):
    """Whether code under `#if expr` is compiled only in dev launcher builds: a dev condition, alone
    or joined with && (an || with a player condition is compiled in player builds too)."""
    return any(term in DEV_CONDITIONS for term in re.sub(r"\s+", "", expr.split("//", 1)[0]).split("&&"))


def player_view(source):
    """The source a player build compiles: lines under a dev-only condition blanked (lines kept)."""
    out, stack = [], []  # per open #if: [the #ifndef negates a dev condition, this branch is dev-only]
    for line in source.split("\n"):
        m = DIRECTIVE.match(line)
        if m:
            kind, rest = m.group(1), m.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                condition = rest if kind == "if" else f"defined({rest})"
                stack.append([kind == "ifndef" and dev_condition(condition), kind != "ifndef" and dev_condition(condition)])
            elif kind == "elif" and stack:
                stack[-1] = [False, dev_condition(rest)]
            elif kind == "else" and stack:
                stack[-1][1] = stack[-1][0]
            elif kind == "endif" and stack:
                stack.pop()
            out.append(line)
        else:
            out.append("" if any(dev for _, dev in stack) else line)
    return "\n".join(out)


def scan(folders, view=None):
    found = {}
    for root in folders:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
            for name in sorted(filenames):
                if name.endswith((".cppm", ".cpp", ".hpp", ".h")) and name not in SKIP_FILES:
                    path = os.path.join(dirpath, name)
                    with open(path, encoding="utf-8", errors="replace") as f:
                        source = f.read()
                    scan_file(found, os.path.relpath(path, ROOT).replace("\\", "/"), view(source) if view else source)
    return found


def write_if_changed(path, text, what, check):
    old = open(path, encoding="utf-8").read() if os.path.isfile(path) else None
    rel = os.path.relpath(path, ROOT)
    if check:
        if old != text:
            print(f"{rel} is out of date: run scripts/generate_ui_text.py")
            return False
    elif old != text:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"{what} -> {rel}")
    else:
        print(f"{what}, unchanged")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--missing")
    args = ap.parse_args()
    current, problems, missing = True, [], []

    def rel(path):
        return os.path.relpath(path, ROOT).replace("\\", "/")

    for name, (tsv, header, namespace, folders) in CATALOGS.items():
        if name == "private" and not os.path.isdir(PRIVATE):
            continue
        rows, row_problems = load_tsv(tsv)
        problems += row_problems
        dev_tsv, dev_rows = DEV_TSV.get(name), []
        found = player = None
        if dev_tsv:
            dev_rows, dev_problems = load_tsv(dev_tsv)
            problems += dev_problems
            keys = {english for english, _ in rows}
            problems += [f"{rel(dev_tsv)}: {english!r} also has a row in {rel(tsv)}" for english, _ in dev_rows if english in keys]
            # Each row in the file of the builds that draw it.
            found, player = scan(folders), scan(folders, player_view)
            problems += [f"{rel(tsv)}: only dev launcher code draws {english!r}: move its row to {rel(dev_tsv)}"
                         for english, _ in rows if english in found and english not in player]
            problems += [f"{rel(dev_tsv)}: player builds draw {english!r}: move its row to {rel(tsv)}"
                         for english, _ in dev_rows if english in player]
        what = f"{len(rows)} {name} strings" + (f" and {len(dev_rows)} dev launcher ones" if dev_tsv else "")
        current &= write_if_changed(header, render(rows, tsv, namespace, dev_rows, dev_tsv), what, args.check)
        if args.missing:
            known = {english for english, _ in rows} | {english for english, _ in dev_rows}
            if found is None:
                found = player = scan(folders)
            new = [(k, name if k in player else name + "-dev", place, code)
                   for k, (place, code) in sorted(found.items()) if k not in known]
            missing += new
            print(f"{len(found)} {name} strings in source, {len(new)} without a row")
    for p in problems:
        print(p)
    if args.missing:
        with open(args.missing, "w", encoding="utf-8", newline="\n") as f:
            f.write("# english\tchinese\tcatalog\tfirst place\tsource line (scripts/generate_ui_text.py --missing)\n")
            for key, name, place, code in missing:
                f.write(f"{escape(key)}\t\t{name}\t{place}\t{escape(code)}\n")
        print(f"-> {args.missing}")
    return 1 if problems or not current else 0


if __name__ == "__main__":
    sys.exit(main())
