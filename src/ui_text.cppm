module;
#include <imgui.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
#include "text/zh_cn.hpp"

export module ui_text;

export namespace gw2::text {
// One row of a Chinese catalog: the English as the launcher draws it, and its Chinese ("" keeps
// the English). Generated from src/text/zh-CN.tsv by scripts/generate_ui_text.py.
using Row = std::pair<const char *, const char *>;
}

namespace gw2::text {
// The launcher's text in Chinese. ImGui hands every text it measures or draws to translate()
// (the ImGui patch in CMakeLists.txt); while Chinese is on, an English string with a row is
// swapped for its Chinese. A string with printf conversions or concatenated values is a pattern:
// "Signed in as %s" carries its value into the Chinese. Typed input text is never translated.

std::uint64_t hash(std::string_view text) noexcept {
    std::uint64_t value = 0xCBF29CE484222325ull;
    for (const char c : text) {
        value ^= static_cast<unsigned char>(c);
        value *= 0x100000001B3ull;
    }
    return value;
}

// The length of the value at text[i], 0 for none: a printf conversion ("%s", "%.1f"; "%%" is a
// literal percent, *literal set) or a std::format field ("{}", "{:.1f}"; "{{" and "}}" are
// literal braces). "%2$s" and "{1}" name their value: *position is 2 (else 0).
std::size_t specLength(std::string_view text, std::size_t i, bool *literal, int *position) noexcept {
    *literal = false;
    *position = 0;
    if (i >= text.size()) return 0;
    if (text[i] == '{' || text[i] == '}') {
        if (i + 1 < text.size() && text[i + 1] == text[i]) {
            *literal = true;
            return 2;
        }
        if (text[i] == '}') return 0;
        std::size_t j = i + 1;
        int number = 0;
        bool numbered = false;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9' && number < 100) {
            number = number * 10 + (text[j] - '0');
            numbered = true;
            ++j;
        }
        if (j < text.size() && text[j] == ':')
            while (j < text.size() && text[j] != '}' && text[j] != '{') ++j;
        if (j >= text.size() || text[j] != '}') return 0; // "{name}": text, not a field
        *position = numbered ? number + 1 : 0;
        return j + 1 - i;
    }
    if (text[i] != '%') return 0;
    std::size_t j = i + 1;
    if (j < text.size() && text[j] == '%') {
        *literal = true;
        return 2;
    }
    std::size_t digits = j;
    int number = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9' && number < 100) {
        number = number * 10 + (text[digits] - '0');
        ++digits;
    }
    if (digits > j && digits < text.size() && text[digits] == '$') {
        *position = number;
        j = digits + 1;
    }
    while (j < text.size() && std::string_view("-+ #0").find(text[j]) != std::string_view::npos) ++j;
    while (j < text.size() && ((text[j] >= '0' && text[j] <= '9') || text[j] == '*' || text[j] == '.')) ++j;
    while (j < text.size() && std::string_view("hljztL").find(text[j]) != std::string_view::npos) ++j;
    if (j < text.size() && std::string_view("diouxXeEfFgGaAcsp").find(text[j]) != std::string_view::npos)
        return j + 1 - i;
    return 0;
}

// What a value's conversion prints: any text ("%s", "{}"), or only a number ("%d", "%.1f"), so
// a pattern's number never takes words.
enum class Kind : std::uint8_t { text, integer, hex, real, character };

Kind kindOf(std::string_view spec) noexcept {
    if (spec.size() < 2) return Kind::text;
    char last = spec.back();
    if (spec.front() == '{') {
        if (spec.find(':') == std::string_view::npos) return Kind::text;
        last = spec[spec.size() - 2];
        if (std::string_view("dbo").find(last) != std::string_view::npos) return Kind::integer;
        if (last == 'x' || last == 'X') return Kind::hex;
        if (std::string_view("eEfFgG%").find(last) != std::string_view::npos) return Kind::real;
        return last == 'c' ? Kind::character : Kind::text;
    }
    if (std::string_view("diuo").find(last) != std::string_view::npos) return Kind::integer;
    if (last == 'x' || last == 'X' || last == 'p') return Kind::hex;
    if (std::string_view("eEfFgGaA").find(last) != std::string_view::npos) return Kind::real;
    return last == 'c' ? Kind::character : Kind::text;
}

// Whether `value` is what a conversion of `kind` prints (width padding allowed).
bool fits(Kind kind, std::string_view value) noexcept {
    if (kind == Kind::text) return true;
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
    if (value.empty()) return false;
    if (kind == Kind::character) return value.size() <= 4;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (kind == Kind::hex) {
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) value.remove_prefix(2);
        if (value.empty()) return false;
        for (const char c : value)
            if (!digit(c) && !((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    }
    if (value.front() == '-' || value.front() == '+') value.remove_prefix(1);
    std::size_t digits = 0, dots = 0, i = 0;
    for (; i < value.size(); ++i) {
        if (digit(value[i])) {
            ++digits;
            continue;
        }
        if (value[i] == '.' && kind == Kind::real && dots == 0) {
            ++dots;
            continue;
        }
        break;
    }
    return digits > 0 && i == value.size();
}

// An English string with values: its literal pieces around them (one more piece than values)
// and the Chinese, as pieces and the values they take.
struct Pattern {
    std::vector<std::string> pieces;
    std::vector<Kind> kinds;
    std::size_t literal{}; // its own letters and signs: the most specific match wins
    struct Out {
        std::string literal;
        int value{-1};    // -1: literal
        bool shown{true}; // false for "%.0s": the value is taken and not shown (an English plural "s")
    };
    std::vector<Out> chinese;
    // Hardly any words of its own ("%s: %s"): it matches only text whose values have rows or are
    // no words, so it never leaves an English part.
    bool strict{};
};

struct Catalog {
    std::unordered_map<std::uint64_t, const char *> exact; // "" for a row kept English
    std::vector<Pattern> patterns;
    // Patterns by their first three bytes, else by their last three; ones short at both ends
    // ("%s: %s") are always tried.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> byStart, byEnd;
    std::vector<std::uint32_t> open;
};

std::uint32_t startKey(std::string_view text) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8 |
        static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16;
}

bool parsePattern(std::string_view english, std::string_view chinese, Pattern &out) {
    out.pieces.assign(1, {});
    for (std::size_t i = 0; i < english.size();) {
        bool literal{};
        int position{};
        if (const auto n = specLength(english, i, &literal, &position)) {
            if (literal) {
                out.pieces.back() += english[i];
            } else {
                out.pieces.emplace_back();
                out.kinds.push_back(kindOf(english.substr(i, n)));
            }
            i += n;
            continue;
        }
        out.pieces.back() += english[i++];
    }
    for (const auto &piece : out.pieces) out.literal += piece.size();
    const int values = static_cast<int>(out.pieces.size()) - 1;
    if (values == 0) return false;
    out.chinese.assign(1, {});
    int next = 0;
    for (std::size_t i = 0; i < chinese.size();) {
        bool literal{};
        int position{};
        if (const auto n = specLength(chinese, i, &literal, &position)) {
            if (literal) {
                out.chinese.back().literal += chinese[i];
            } else {
                const int value = position > 0 ? position - 1 : next++;
                if (value < 0 || value >= values) return false;
                out.chinese.push_back({{}, value, !chinese.substr(i, n).ends_with(".0s")});
                out.chinese.push_back({});
            }
            i += n;
            continue;
        }
        out.chinese.back().literal += chinese[i++];
    }
    return true;
}

std::size_t letters(std::string_view text) noexcept {
    std::size_t n = 0;
    for (const char c : text) n += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return n;
}

std::vector<std::span<const Row>> &sources() {
    static std::vector<std::span<const Row>> rows{std::span<const Row>(zhCn)};
    return rows;
}
std::optional<Catalog> &builtCatalog() {
    static std::optional<Catalog> catalog;
    return catalog;
}

const Catalog &catalog() {
    auto &built = builtCatalog();
    if (built) return *built;
    built.emplace();
    for (const auto rows : sources()) {
        for (const auto &[englishText, chineseText] : rows) {
            const std::string_view english(englishText);
            // Every key matches whole first: a percent in prose ("5% done") is no format.
            built->exact.emplace(hash(english), chineseText);
            Pattern pattern;
            if (!*chineseText || english.find_first_of("%{") == std::string_view::npos ||
                !parsePattern(english, chineseText, pattern))
                continue;
            std::size_t own = 0;
            for (const auto &piece : pattern.pieces) own += letters(piece);
            pattern.strict = own < 3;
            const auto index = static_cast<std::uint32_t>(built->patterns.size());
            const auto &front = pattern.pieces.front(), &back = pattern.pieces.back();
            if (front.size() >= 3)
                built->byStart[startKey(front)].push_back(index);
            else if (back.size() >= 3)
                built->byEnd[startKey(std::string_view(back).substr(back.size() - 3))].push_back(index);
            else
                built->open.push_back(index);
            built->patterns.push_back(std::move(pattern));
        }
    }
    return *built;
}

// The values `text` gives the pattern's pieces, in order; false when it is not that pattern. A
// value ends where the next piece first appears; the last piece ends the text.
bool match(const Pattern &pattern, std::string_view text, std::vector<std::string_view> &values) {
    values.clear();
    const auto &pieces = pattern.pieces;
    if (!text.starts_with(pieces.front())) return false;
    std::size_t at = pieces.front().size();
    for (std::size_t k = 1; k < pieces.size(); ++k) {
        const auto &piece = pieces[k];
        if (k + 1 == pieces.size()) {
            if (text.size() < at + piece.size() || !text.ends_with(piece)) return false;
            values.push_back(text.substr(at, text.size() - piece.size() - at));
            return fits(pattern.kinds[k - 1], values.back());
        }
        if (piece.empty()) return false; // two values side by side cannot be told apart
        const auto found = text.find(piece, at);
        if (found == std::string_view::npos) return false;
        values.push_back(text.substr(at, found - at));
        if (!fits(pattern.kinds[k - 1], values.back())) return false;
        at = found + piece.size();
    }
    return false;
}

constexpr int maxDepth = 4; // values and joined pieces are translated a few levels deep at most

std::string translatePart(std::string_view text, int depth, bool *kept);
std::string translateCore(std::string_view core, int depth, bool *kept);

// Text joined from pieces ("Path: C:\Games\Gw2-64.exe", the lines of a tooltip): split where
// pieces meet (a line break, two blanks, a bracket, or a separator beside a blank), each piece
// translated on its own. All or nothing: one piece with English and no row leaves the text as it
// was, so a sentence is never put together word by word.
std::string translatePieces(std::string_view core, int depth) {
    constexpr std::string_view separators = " \n+/|=-,:;()[]<>";
    std::string out;
    std::size_t pieces = 0, i = 0;
    while (i < core.size()) {
        std::size_t start = i, runStart = core.size(), runEnd = core.size();
        for (std::size_t j = i; j < core.size();) {
            if (separators.find(core[j]) == std::string_view::npos) {
                ++j;
                continue;
            }
            std::size_t k = j, blanks = 0;
            bool separator{}, boundary{};
            while (k < core.size() && separators.find(core[k]) != std::string_view::npos) {
                const char c = core[k];
                boundary |= c == '\n' || c == '(' || c == ')' || c == '[' || c == ']';
                blanks += c == ' ';
                separator |= c != ' ';
                ++k;
            }
            if (boundary || blanks >= 2 || (separator && blanks >= 1 && j > i && k < core.size())) {
                runStart = j;
                runEnd = k;
                break;
            }
            j = k;
        }
        if (start == 0 && runStart == core.size()) return {}; // one piece: the text itself
        auto piece = core.substr(start, runStart - start);
        auto run = core.substr(runStart, runEnd - runStart);
        if (!piece.empty()) {
            std::string translated;
            bool pieceKept{};
            // "(Chinese client)" has its row with the brackets.
            const char opening = start > 0 ? core[start - 1] : '\0';
            if ((opening == '(' && run.starts_with(')')) || (opening == '[' && run.starts_with(']'))) {
                translated = translatePart(core.substr(start - 1, runStart + 2 - start), depth + 1, &pieceKept);
                if (!translated.empty() || pieceKept) {
                    out.pop_back();
                    piece = core.substr(start - 1, runStart + 2 - start);
                    run.remove_prefix(1);
                }
            }
            // "Path:" has its row with the colon.
            if (translated.empty() && !pieceKept && !run.empty() &&
                std::string_view(":,;").find(run.front()) != std::string_view::npos) {
                translated = translatePart(core.substr(start, runStart + 1 - start), depth + 1, &pieceKept);
                if (!translated.empty() || pieceKept) {
                    piece = core.substr(start, runStart + 1 - start);
                    run.remove_prefix(1);
                }
            }
            if (translated.empty() && !pieceKept) translated = translatePart(piece, depth + 1, &pieceKept);
            if (translated.empty()) {
                if (!pieceKept && letters(piece) >= 2) return {};
                out += piece;
            } else {
                out += translated;
            }
            ++pieces;
        }
        out += run;
        i = runEnd;
    }
    return pieces >= 2 ? out : std::string();
}

// The Chinese of `core` (no blanks around it); empty when the catalog has none (*kept: a row
// keeps it English on purpose).
std::string translateCore(std::string_view core, int depth, bool *kept) {
    *kept = false;
    const auto &c = catalog();
    if (const auto found = c.exact.find(hash(core)); found != c.exact.end()) {
        *kept = !found->second[0];
        return found->second;
    }
    std::vector<std::string_view> values;
    const auto tryPattern = [&](std::uint32_t index) -> std::string {
        const auto &pattern = c.patterns[index];
        if (!match(pattern, core, values)) return {};
        std::string out;
        for (const auto &part : pattern.chinese) {
            if (part.value < 0) {
                out += part.literal;
                continue;
            }
            // A value is drawn text too ("Steam", "3 accounts"): shown in Chinese when it has a
            // row of its own.
            const auto value = values[static_cast<std::size_t>(part.value)];
            bool valueKept{};
            const auto translated = depth < maxDepth && letters(value) ? translatePart(value, depth + 1, &valueKept)
                                                                         : std::string();
            if (pattern.strict && translated.empty() && !valueKept && letters(value)) return {};
            if (!part.shown) continue;
            out += translated.empty() ? std::string(value) : translated;
        }
        return out;
    };
    // The translation that leaves the fewest English letters wins (a pattern passes a value with
    // no row through: a name, a path), then the pattern with the most text of its own.
    std::string best;
    std::size_t bestLetters{}, bestLiteral{};
    const auto offer = [&](std::string out, std::size_t literal) {
        if (out.empty()) return;
        const auto n = letters(out);
        if (!best.empty() && (n > bestLetters || (n == bestLetters && literal <= bestLiteral))) return;
        best = std::move(out);
        bestLetters = n;
        bestLiteral = literal;
    };
    const auto tryAll = [&](const std::vector<std::uint32_t> &indices) {
        for (const auto index : indices) offer(tryPattern(index), c.patterns[index].literal);
    };
    if (core.size() >= 3) {
        if (const auto bucket = c.byStart.find(startKey(core)); bucket != c.byStart.end()) tryAll(bucket->second);
        if (const auto bucket = c.byEnd.find(startKey(core.substr(core.size() - 3))); bucket != c.byEnd.end())
            tryAll(bucket->second);
    }
    tryAll(c.open);
    if (depth >= maxDepth || (!best.empty() && bestLetters == 0)) return best;
    offer(translatePieces(core, depth), 0);
    return best;
}

// `text` in Chinese with its blanks and line breaks around it kept; empty when the catalog has no
// Chinese for it.
std::string translatePart(std::string_view text, int depth, bool *kept) {
    *kept = false;
    std::size_t lead = 0, end = text.size();
    while (lead < end && (text[lead] == ' ' || text[lead] == '\n')) ++lead;
    while (end > lead && (text[end - 1] == ' ' || text[end - 1] == '\n')) --end;
    if (end <= lead) return {};
    const auto core = translateCore(text.substr(lead, end - lead), depth, kept);
    if (core.empty()) return {};
    std::string out;
    out.reserve(text.size() + core.size());
    out.append(text.substr(0, lead)).append(core).append(text.substr(end));
    return out;
}

bool chineseOn{};
// Drawn text (whole, by hash) -> its translation, empty when it has none. Node-based, so a
// translation stays put while ImGui draws it; cleared between frames only.
std::unordered_map<std::uint64_t, std::string> cache;

void translate(const char **begin, const char **end) {
    if (!chineseOn) return;
    const std::string_view text(*begin, static_cast<std::size_t>(*end - *begin));
    if (text.size() > 4096 || !letters(text)) return;
    const auto key = hash(text);
    auto cached = cache.find(key);
    if (cached == cache.end()) {
        bool kept{};
        cached = cache.emplace(key, translatePart(text, 0, &kept)).first;
    }
    if (cached->second.empty()) return;
    *begin = cached->second.data();
    *end = cached->second.data() + cached->second.size();
}

// The system's Chinese font, merged into the launcher font the first time the text turns
// Chinese (Roboto has no Chinese glyphs): Microsoft YaHei on Windows, Noto Sans CJK SC or
// WenQuanYi on Linux.
void mergeChineseFont() {
    static bool merged{};
    if (merged) return;
    merged = true;
    auto &io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) return;
    std::vector<std::pair<std::filesystem::path, int>> fonts;
#ifdef _WIN32
    PWSTR directory{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &directory))) {
        const std::filesystem::path dir(directory);
        fonts = {{dir / "msyh.ttc", 0}, {dir / "simhei.ttf", 0}, {dir / "simsun.ttc", 0}};
    }
    CoTaskMemFree(directory);
#else
    // Noto Sans CJK's collection holds JP, KR, SC, TC, HK in that order: face 2 is Simplified.
    for (const char *file : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc"})
        fonts.emplace_back(file, 2);
    for (const char *file : {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
             "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
             "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"})
        fonts.emplace_back(file, 0);
#endif
    for (const auto &[file, face] : fonts) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) continue;
        ImFontConfig config;
        config.MergeMode = true;
        config.DstFont = io.Fonts->Fonts[0];
        config.FontNo = static_cast<ImU32>(face);
        if (io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char *>(file.u8string().c_str()), 0.0f, &config)) return;
    }
}
} // namespace gw2::text

export namespace gw2::text {
// Adds rows to the catalog (the private extension's own text); call before the first frame.
void addCatalog(std::span<const Row> rows) {
    sources().push_back(rows);
    builtCatalog().reset();
    cache.clear();
}
// Hooks ImGui's text; once, after the ImGui context exists.
void install() { ImGui::SetTranslateTextFn(&translate); }
// Each frame before NewFrame: whether the launcher speaks Chinese.
void setChinese(bool on) {
    if (on) mergeChineseFont();
    if (on != chineseOn) {
        chineseOn = on;
        cache.clear();
    }
    if (cache.size() > 16384) cache.clear();
}
bool chinese() { return chineseOn; }
} // namespace gw2::text
