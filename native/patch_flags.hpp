#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <string_view>

// GW2 adds the "arguments" list of %APPDATA%\Guild Wars 2\Settings.json to the command line of
// every start (the global and the China client both read that one file), so a flag a player
// keeps there also reaches the updater this launcher starts. Two of them stop GW2 from patching:
// with -shareArchive an older executable ends on "Client needs to be patched first.", and with
// either one an archive that still needs files ends on "Need to patch before using
// -shareArchive or -noPatch." Both leave GW2's own error window and exit code 1.
//
// Run against the game on 2026-11-12: GW2 takes any letter case, '-' or '/' before the name,
// and several flags in one string. A shortened name, -multi (named in GW2's message, but it
// patches), and an "arguments" that is one string instead of a list change nothing.
namespace gw2::patch_flags {
inline constexpr unsigned shareArchive = 1, noPatch = 2;

// The flag one command-line word names, or 0.
inline unsigned named(std::string_view word) {
    if (word.empty() || (word.front() != '-' && word.front() != '/')) return 0;
    word.remove_prefix(1);
    const auto is = [word](std::string_view name) {
        if (word.size() != name.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const char c = word[i];
            if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c) != name[i]) return false;
        }
        return true;
    };
    return is("sharearchive") ? shareArchive : is("nopatch") ? noPatch : 0;
}
// The flags in one string of the list. GW2 appends the string to its command line as it is:
// words end at blanks outside double quotes.
inline unsigned inText(std::string_view text) {
    unsigned flags{};
    std::string word;
    bool quoted{};
    for (const char c : text) {
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && (c == ' ' || c == '\t')) {
            flags |= named(word);
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    return flags | named(word);
}
// The flags in the whole file. Text GW2 would not read as a list of arguments holds none.
inline unsigned inSettings(std::string_view settings) {
    const auto root = nlohmann::json::parse(settings, nullptr, false);
    if (!root.is_object()) return 0;
    const auto arguments = root.find("arguments");
    if (arguments == root.end() || !arguments->is_array()) return 0;
    unsigned flags{};
    for (const auto &value : *arguments)
        if (value.is_string()) flags |= inText(value.get_ref<const std::string &>());
    return flags;
}
} // namespace gw2::patch_flags
