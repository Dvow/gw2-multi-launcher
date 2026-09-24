module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module game;

export namespace gw2 {
using Bytes = std::vector<unsigned char>;
struct GamePathRequired : std::runtime_error {
    GamePathRequired() : std::runtime_error("Select your installed Gw2-64.exe in Settings.") {}
};
struct Anchor {
    std::uint32_t rva{}, size{};
    std::array<unsigned char, 128> bytes{};
};
struct Layout {
    std::uint32_t timestamp{}, imageSize{}, build{}, context{}, contextTable{}, login{};
    std::uint32_t launcher{}, launcherTable{}, connection{}, play{}, tokenLogin{}, tokenSubmit{},
        preferences{};
    std::array<Anchor, 26> anchors{};
};

// PE addresses are checked against file sections before they become an offset.
// Only copied RVAs and instruction bytes cross the helper/target boundary.
class Image {
    struct Section {
        std::uint32_t rva, size, raw, flags;
    };
    Bytes bytes_;
    std::vector<Section> sections_;
    std::uint64_t base_{};
    std::uint32_t timestamp_{}, size_{};
    [[noreturn]] static void invalid() {
        throw std::runtime_error("GW2's native interface has changed or its executable is incomplete. Update "
                                 "the launcher before signing in.");
    }
    template <class T> T read(std::size_t offset) const {
        if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset) invalid();
        T value;
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        return value;
    }
    std::size_t raw(std::uint32_t rva, std::size_t length = 1) const {
        for (const auto &s : sections_)
            if (rva >= s.rva && rva - s.rva < s.size && length <= s.size - (rva - s.rva))
                return s.raw + static_cast<std::size_t>(rva - s.rva);
        invalid();
    }
    static std::vector<int> pattern(std::string_view text) {
        std::vector<int> result;
        while (!text.empty()) {
            if (text.front() == ' ') {
                text.remove_prefix(1);
                continue;
            }
            if (text.front() == '?') {
                result.push_back(-1);
                text.remove_prefix(1);
                continue;
            }
            auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
            if (text.size() < 2) invalid();
            result.push_back(hex(text[0]) * 16 + hex(text[1]));
            text.remove_prefix(2);
        }
        return result;
    }
    bool matches(std::uint32_t rva, std::string_view text) const {
        const auto p = pattern(text);
        auto offset = raw(rva, p.size());
        for (std::size_t i = 0; i < p.size(); ++i)
            if (p[i] >= 0 && bytes_[offset + i] != p[i]) return false;
        return true;
    }
    template <class Accept> std::uint32_t find(std::string_view text, Accept accept) const {
        const auto p = pattern(text);
        std::uint32_t found{};
        for (const auto &s : sections_) {
            if (!(s.flags & 0x20000000u) || s.size < p.size()) continue;
            const auto *begin = bytes_.data() + s.raw;
            const auto *end = begin + s.size - p.size() + 1;
            auto *candidate = begin;
            while (candidate < end) {
                candidate = static_cast<const unsigned char *>(
                    std::memchr(candidate, p[0], static_cast<std::size_t>(end - candidate)));
                if (!candidate) break;
                std::size_t j = 1;
                for (; j < p.size(); ++j)
                    if (p[j] >= 0 && candidate[j] != p[j]) break;
                if (j == p.size() && accept(s.rva + static_cast<std::uint32_t>(candidate - begin))) {
                    if (found) invalid();
                    found = s.rva + static_cast<std::uint32_t>(candidate - begin);
                }
                ++candidate;
            }
        }
        if (!found) invalid();
        return found;
    }
    std::uint32_t find(std::string_view text) const {
        return find(text, [](std::uint32_t) { return true; });
    }
    std::uint32_t relative(std::uint32_t instruction, unsigned operand, unsigned length) const {
        auto value = static_cast<std::int64_t>(instruction) + length +
            read<std::int32_t>(raw(instruction + operand, 4));
        if (value <= 0 || value >= size_) invalid();
        return static_cast<std::uint32_t>(value);
    }
    std::uint32_t pointer(std::uint32_t rva) const {
        const auto value = read<std::uint64_t>(raw(rva, 8));
        if (value < base_ || value - base_ >= size_) invalid();
        return static_cast<std::uint32_t>(value - base_);
    }
    void require(bool condition) const {
        if (!condition) invalid();
    }

    void anchor(Layout &result, unsigned index, std::uint32_t rva, unsigned size) const {
        auto &a = result.anchors[index];
        require(size <= a.bytes.size());
        a.rva = rva;
        a.size = size;
        std::memcpy(a.bytes.data(), bytes_.data() + raw(rva, size), size);
    }
    void resolveCore(Layout &result) const {
        const auto buildGetter = relative(find("E8 ? ? ? ? 66 39 85"), 1, 5);
        result.login = find(
            "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 54 41 56 41 57 48 83 EC 40 45 8B F1");
        result.play = find("48 89 74 24 ? 57 48 83 EC 40 48 8B F1 E8 ? ? ? ? 85 C0");
        require(matches(result.play + 18, "85 C0 75 35 48 8B 46 08 48 8D 4E 08"));
        require(matches(result.play + 75, "E8 ? ? ? ? 48 8B 08 48 8B 51 78 48 8B C8 FF D2"));
        const auto contextGetter = relative(result.play + 75, 1, 5);
        require(matches(contextGetter, "48 8D 05 ? ? ? ? C3"));
        result.context = relative(contextGetter, 3, 7);
        result.contextTable = pointer(result.context);
        require(pointer(result.contextTable) == result.login);
        const auto connectionGetter = relative(result.play + 13, 1, 5);
        require(matches(connectionGetter, "8B 05 ? ? ? ? 85 C0 74 0B 83 F8 03 74 06 B8 01 00 00 00 C3"));
        result.connection = relative(connectionGetter, 2, 6);
        const auto launcherRef = find("48 8B 3D ? ? ? ? 48 8B DA 4C 8B C1");
        result.launcher = relative(launcherRef, 3, 7);
        const auto constructor = find("48 8D 05 ? ? ? ? 33 ED 48 89 01 48 8D 79 18 48 8D 05");
        result.launcherTable = relative(constructor, 3, 7);
        // These offsets are layout contracts, checked in their owning routines.
        require(matches(constructor + 23, "48 89 69 28"));
        require(matches(constructor + 78, "48 89 6E 40"));
        require(matches(constructor + 108, "48 89 AE A0 00 00 00"));
        require(matches(result.login + 39, "E8"));
        const auto signingIn = relative(result.login + 39, 1, 5);
        require(matches(signingIn, "40 53 48 83 EC 20 F6 41 10 02"));
        const std::array<std::pair<std::uint32_t, unsigned>, 8> anchors{
            {{buildGetter, 6}, {result.login, 64}, {result.play, 100}, {contextGetter, 8},
                {connectionGetter, 24}, {launcherRef, 32}, {constructor, 120}, {signingIn, 100}}};
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            const auto [rva, size] = anchors[i];
            anchor(result, static_cast<unsigned>(i), rva, size);
        }
    }
    void resolveAgreement(Layout &result) const {
        require(matches(result.play + 92, "85 C0 0F 85"));
        const auto playContinuation = relative(result.play + 94, 2, 6);
        require(matches(playContinuation, "81 4E 28 00 01 00 00"));
        anchor(result, 15, playContinuation, 7);
        const auto acceptAgreement = pointer(result.contextTable + 104);
        const auto hasAgreement = pointer(result.contextTable + 120);
        require(matches(acceptAgreement,
            "48 83 EC 28 E8 ? ? ? ? 48 8B 08 48 8B 91 E8 02 00 00 48 8B C8 FF D2 8B C8 "
            "E8 ? ? ? ? 0F B6 C8 48 83 C4 28 E9"));
        require(matches(hasAgreement,
            "40 53 48 83 EC 20 E8 ? ? ? ? 48 8B 08 48 8B 91 E8 02 00 00 48 8B C8 FF D2 8B C8 "
            "E8 ? ? ? ? 8B D8 E8 ? ? ? ? 0F B6 C8 33 C0 3B CB 0F 94 C0 48 83 C4 20 5B C3"));
        require(relative(acceptAgreement + 4, 1, 5) == relative(hasAgreement + 6, 1, 5) &&
            relative(acceptAgreement + 26, 1, 5) == relative(hasAgreement + 28, 1, 5));
        const auto sendAgreement = relative(acceptAgreement + 38, 1, 5);
        require(matches(sendAgreement,
            "40 53 48 83 EC 20 0F B6 D9 33 D2 48 8D 4C 24 38 E8 ? ? ? ? 66 83 38 00 75 0E "
            "0F B6 CB E8 ? ? ? ? 88 1D ? ? ? ? 48 83 C4 20 5B C3"));
        const auto acceptedVersion = relative(hasAgreement + 35, 1, 5);
        require(matches(acceptedVersion, "0F B6 05 ? ? ? ? C3") &&
            relative(acceptedVersion, 3, 7) == relative(sendAgreement + 35, 2, 6));
        // Acceptance and its predicate must refer to the same current agreement.
        anchor(result, 17, acceptAgreement, 43);
        anchor(result, 18, hasAgreement, 56);
        anchor(result, 19, sendAgreement, 47);
    }
    void resolveToken(Layout &result) const {
        result.tokenLogin = pointer(result.contextTable + 16);
        require(matches(result.tokenLogin,
            "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 "
            "56 48 83 EC 40 41 8B F1 49 8B E8 4C 8B F2 E8"));
        require(relative(result.tokenLogin + 35, 1, 5) == result.anchors[7].rva);
        require(matches(result.tokenLogin + 40,
            "E8 ? ? ? ? 48 8B 08 4C 8B 91 B0 00 00 00 48 8B C8 41 FF D2 8B F8 "
            "E8 ? ? ? ? 48 8B 08 48 8B 91 90 00 00 00 48 8B C8 FF D2 48 8B D8 "
            "E8 ? ? ? ? 48 8B 08 48 8B 91 78 02 00 00 48 8B C8 FF D2"));
        require(matches(result.tokenLogin + 106, "C7 44 24 38 00 00 00 00 48 8D 0D"));
        require(matches(result.tokenLogin + 121,
            "89 7C 24 30 44 8B C8 48 89 4C 24 28 44 8B C6 49 8B CE 48 89 5C 24 20 48 8B D5 E8"));
        const auto provider = relative(result.tokenLogin + 114, 3, 7);
        constexpr char16_t expected[] = u"Portal.Token";
        require(!std::memcmp(bytes_.data() + raw(provider, sizeof(expected)), expected, sizeof(expected)));
        const auto prefsGetter = relative(result.tokenLogin + 40, 1, 5);
        require(relative(result.tokenLogin + 63, 1, 5) == prefsGetter &&
            relative(result.tokenLogin + 86, 1, 5) == prefsGetter &&
            matches(prefsGetter, "48 8D 05 ? ? ? ? C3"));
        result.preferences = relative(prefsGetter, 3, 7);
        result.tokenSubmit = relative(result.tokenLogin + 147, 1, 5);
        require(matches(result.tokenSubmit,
            "40 53 55 56 57 41 56 41 57 48 83 EC 78 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 68 "
            "4C 8B B4 24 D0 00 00 00 48 8B D9 4C 8B BC 24 D8 00 00 00 B9 FF FF FF DF "
            "41 8B E9 41 8B F0 48 8B FA E8"));
        anchor(result, 8, result.tokenLogin, 128);
        anchor(result, 9, result.tokenLogin + 128, 51);
        anchor(result, 10, prefsGetter, 8);
        anchor(result, 11, result.tokenSubmit, 64);
        // This query supplies the provider byte in both native game handshakes.
        // Its normal source is the embedded store SDK, which our session replaces.
        const auto channel = find("48 83 EC 28 E8 ? ? ? ? 85 C0 74 0A B8 01 00 00 00 48 83 C4 28 C3 "
                                  "E8 ? ? ? ? F7 D8 1B C0 83 E0 02 48 83 C4 28 C3");
        require(matches(relative(channel + 4, 1, 5), "8B 05 ? ? ? ? C3") &&
            matches(relative(channel + 23, 1, 5), "8B 05 ? ? ? ? C3"));
        anchor(result, 16, channel, 40);
    }
    void resolveNotifications(Layout &result) const {
        const auto subscribe = pointer(result.contextTable + 128);
        require(matches(subscribe, "48 89 5C 24 ? 57 48 83 EC 20 48 8B 41 40 48 8D 59 38 48 8B FA 48 85 C0"));
        require(matches(result.anchors[6].rva + 16, "48 8D 05 ? ? ? ? 48 89 69 28 48 89 41 08"));
        require(matches(result.anchors[6].rva + 35, "48 8D 05"));
        const auto noticeTable = relative(result.anchors[6].rva + 16, 3, 7);
        require(relative(result.anchors[6].rva + 35, 3, 7) - noticeTable == 17 * 8);
        const auto errorNotice = pointer(noticeTable + 88), ignoreNotice = pointer(noticeTable + 8);
        require(matches(errorNotice,
            "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 30 48 83 B9 98 00 00 00 00 "
            "41 8B E8 41 8B F1 48 8B FA 48 8B D9"));
        require(matches(errorNotice + 64, "0F BF 07 48 8D 15") && matches(ignoreNotice, "C2 00 00"));
        anchor(result, 12, subscribe, 128);
        anchor(result, 13, errorNotice, 128);
        anchor(result, 14, ignoreNotice, 3);
        const auto unsubscribe = pointer(result.contextTable + 136);
        require(matches(unsubscribe,
            "48 8B 41 40 4C 8B C9 48 85 C0 74 58 0F 1F 40 00 48 39 10 74 0A 48 8B 40 08 "
            "48 85 C0 75 F2 C3 48 8B 50 10 48 8B 48 08 48 85 D2 74 06 48 89 4A 08 EB 04 "
            "49 89 49 40 48 8B 50 08 48 8B 48 10 48 85 D2 74 11 48 89 4A 10 49 8B 49 50 "
            "48 89 48 08 49 89 41 50 C3 49 89 49 48 49 8B 49 50 48 89 48 08 49 89 41 50 C3"));
        anchor(result, 20, unsubscribe, 101);
        // GW2's codeAuth/SMS/TOTP browser callbacks all use context slot 11.
        // Notification slot 0 reports the server-selected authentication type.
        const auto verify = pointer(result.contextTable + 88);
        const auto challenge = pointer(noticeTable);
        require(matches(verify, "48 8B CA 41 8B D0 E9") &&
            matches(relative(verify + 6, 1, 5), "E9"));
        require(matches(challenge,
            "40 53 57 48 83 EC 58 48 83 B9 98 00 00 00 00 8B DA 48 8B F9"));
        anchor(result, 24, verify, 11);
        anchor(result, 25, challenge, 128);
    }

  public:
    explicit Image(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw GamePathRequired{};
        const auto size = stream.tellg();
        if (size < 4096 || size > 256 * 1024 * 1024) invalid();
        bytes_.resize(static_cast<std::size_t>(size));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char *>(bytes_.data()), size)) invalid();
        require(read<std::uint16_t>(0) == 0x5A4D);
        const auto nt = read<std::uint32_t>(60);
        require(nt <= 4096);
        require(read<std::uint32_t>(nt) == 0x4550 && read<std::uint16_t>(nt + 4) == 0x8664);
        const auto count = read<std::uint16_t>(nt + 6), optionalSize = read<std::uint16_t>(nt + 20);
        require(count > 0 && count <= 96 && optionalSize >= 112);
        require(read<std::uint16_t>(nt + 24) == 0x20B);
        base_ = read<std::uint64_t>(nt + 48);
        size_ = read<std::uint32_t>(nt + 80);
        timestamp_ = read<std::uint32_t>(nt + 8);
        for (unsigned i = 0; i < count; ++i) {
            const auto off = nt + 24 + optionalSize + i * 40;
            Section s{read<std::uint32_t>(off + 12), read<std::uint32_t>(off + 16),
                read<std::uint32_t>(off + 20), read<std::uint32_t>(off + 36)};
            require(s.rva <= size_ && s.size <= size_ - s.rva && s.raw <= bytes_.size() &&
                s.size <= bytes_.size() - s.raw);
            sections_.push_back(s);
        }
    }
    std::uint32_t build() const {
        const auto getter = relative(find("E8 ? ? ? ? 66 39 85"), 1, 5);
        require(matches(getter, "B8 ? ? ? ? C3"));
        const auto number = read<std::uint32_t>(raw(getter + 1, 4));
        require(number > 10000 && number < 10000000);
        return number;
    }
    std::array<std::string, 3> epicClient() const {
        // EOS option fields keep their relative positions when unrelated locals
        // move the stack frame. Validate that relationship and one unique data
        // group; never choose the first match or guess credentials from strings.
        const auto setup = find(
            "48 8B 05 ? ? ? ? 48 89 45 ? 48 8B 05 ? ? ? ? 48 89 45 ? "
            "48 8B 05 ? ? ? ? 48 89 ? ?",
            [&](std::uint32_t candidate) {
                const auto first = read<std::int8_t>(raw(candidate + 10));
                if (read<std::int8_t>(raw(candidate + 21)) != first + 8) return false;
                const auto mode = read<unsigned char>(raw(candidate + 31));
                std::int32_t deployment{};
                if (mode == 0x45)
                    deployment = read<std::int8_t>(raw(candidate + 32));
                else if (mode == 0x85)
                    deployment = read<std::int32_t>(raw(candidate + 32, 4));
                else
                    return false;
                if (deployment != first + 0x30) return false;
                const auto fields = static_cast<std::uint64_t>(relative(candidate, 3, 7));
                return relative(candidate + 11, 3, 7) == fields + 8 &&
                    relative(candidate + 22, 3, 7) == fields + 16;
            });
        std::array<std::string, 3> values;
        for (unsigned i = 0; i < values.size(); ++i) {
            const auto address = pointer(relative(setup + i * 11, 3, 7));
            for (unsigned j = 0; j < 256; ++j) {
                const auto c = read<unsigned char>(raw(address + j));
                if (!c) break;
                require(c >= 33 && c <= 126);
                values[i] += static_cast<char>(c);
            }
            require(!values[i].empty() && values[i].size() < 256);
        }
        return values; // Client ID, shipped client key, deployment ID; no user credentials.
    }
    Layout resolve(unsigned provider = 0) const {
        Layout result{};
        result.build = build();
        result.timestamp = timestamp_;
        result.imageSize = size_;
        resolveCore(result);
        resolveAgreement(result);
        resolveNotifications(result);
        if (provider) resolveToken(result);
        if (provider == 2) {
            // The browser caller changes independently of this token setter.
            // Resolve the setter itself, then prove it writes OpenIdQuery's data.
            const auto setter = find("48 8B D1 45 33 C9 48 8D 0D ? ? ? ? 41 B8 FF FF FF FF E9");
            const auto query =
                find("8B 05 ? ? ? ? 85 C0 74 2A 4C 8B 05 ? ? ? ? 83 F8 01 75 06 "
                     "41 80 38 00 74 18 4D 8B CF 4C 89 7C 24 20 48 8D 15 ? ? ? ? 48 8D 4D 90 E8");
            require(relative(setter + 6, 3, 7) == relative(query + 10, 3, 7));
            require(relative(query, 2, 6) == relative(setter + 6, 3, 7) + 8);
            constexpr char name[] = "OpenIdQuery";
            require(!std::memcmp(
                bytes_.data() + raw(relative(query + 36, 3, 7), sizeof(name)), name, sizeof(name)));
            const auto copy = relative(setter + 19, 1, 5);
            require(matches(copy, "40 53 55 41 57 48 83 EC 70 33 ED 4C 8B FA 48 8B D9 48 85 D2"));
            anchor(result, 21, setter, 24);
            anchor(result, 22, query, 52);
            anchor(result, 23, copy, 64);
        }
        return result;
    }
};
} // namespace gw2
