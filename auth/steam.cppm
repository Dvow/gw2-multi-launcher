module;
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <curl/curl.h>
#endif
#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module steam_auth;

namespace detail {
using Clock = std::chrono::steady_clock;

struct Failure : std::runtime_error {
    int code;
    Failure(std::string message, int failureCode)
        : std::runtime_error(std::move(message)), code(failureCode) {}
};

void wipe(void *data, std::size_t size) {
    auto *bytes = static_cast<volatile unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}

struct Secret {
    std::vector<unsigned char> bytes;
    Secret() = default;
    explicit Secret(std::size_t size) : bytes(size) {}
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;
    Secret(Secret &&other) noexcept : bytes(std::move(other.bytes)) {}
    Secret &operator=(Secret &&other) noexcept {
        if (this != &other) {
            clear();
            bytes = std::move(other.bytes);
        }
        return *this;
    }
    ~Secret() { clear(); }
    void clear() {
        if (!bytes.empty()) wipe(bytes.data(), bytes.size());
        bytes.clear();
    }
};

void appendU32(std::vector<unsigned char> &out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void appendU64(std::vector<unsigned char> &out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
std::uint32_t readU32(std::span<const unsigned char> bytes, std::size_t offset) {
    if (bytes.size() - offset < 4) throw Failure("Steam sent an incomplete message.", 1010);
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return value;
}
std::uint64_t readU64(std::span<const unsigned char> bytes, std::size_t offset) {
    if (bytes.size() - offset < 8) throw Failure("Steam sent an incomplete message.", 1010);
    std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}

std::uint32_t crc32(std::span<const unsigned char> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void protoKey(std::vector<unsigned char> &out, int field, int wire) {
    auto value = static_cast<std::uint32_t>((field << 3) | wire);
    while (value > 0x7F) {
        out.push_back(static_cast<unsigned char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<unsigned char>(value));
}
void protoVarint(std::vector<unsigned char> &out, std::uint64_t value) {
    while (value > 0x7F) {
        out.push_back(static_cast<unsigned char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<unsigned char>(value));
}
void protoUint(std::vector<unsigned char> &out, int field, std::uint64_t value) {
    protoKey(out, field, 0);
    protoVarint(out, value);
}
void protoFixed64(std::vector<unsigned char> &out, int field, std::uint64_t value) {
    protoKey(out, field, 1);
    appendU64(out, value);
}
void protoBytes(std::vector<unsigned char> &out, int field, std::span<const unsigned char> bytes) {
    protoKey(out, field, 2);
    protoVarint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}
void protoString(std::vector<unsigned char> &out, int field, std::string_view text) {
    protoBytes(out, field, std::span(reinterpret_cast<const unsigned char *>(text.data()), text.size()));
}

struct Field {
    int id{};
    int wire{};
    std::uint64_t number{};
    std::vector<unsigned char> bytes;
};
std::vector<Field> protoFields(std::span<const unsigned char> bytes) {
    std::vector<Field> fields;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::uint64_t key{};
        int shift = 0;
        while (offset < bytes.size() && shift < 35) {
            const auto byte = bytes[offset++];
            key |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }
        Field field;
        field.id = static_cast<int>(key >> 3);
        field.wire = static_cast<int>(key & 7);
        if (field.wire == 0) {
            std::uint64_t value{};
            shift = 0;
            while (offset < bytes.size() && shift < 70) {
                const auto byte = bytes[offset++];
                value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) break;
                shift += 7;
            }
            field.number = value;
        } else if (field.wire == 1) {
            field.number = readU64(bytes, offset);
            offset += 8;
        } else if (field.wire == 5) {
            field.number = readU32(bytes, offset);
            offset += 4;
        } else if (field.wire == 2) {
            std::uint64_t length{};
            shift = 0;
            while (offset < bytes.size() && shift < 35) {
                const auto byte = bytes[offset++];
                length |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) break;
                shift += 7;
            }
            if (length > bytes.size() - offset) throw Failure("Steam sent an invalid message.", 1010);
            field.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += static_cast<std::size_t>(length);
        } else
            throw Failure("Steam sent an invalid message.", 1010);
        fields.push_back(std::move(field));
    }
    return fields;
}
const Field *findField(const std::vector<Field> &fields, int id) {
    for (const auto &field : fields)
        if (field.id == id) return &field;
    return nullptr;
}
std::string fieldText(const Field *field) {
    if (!field) return {};
    return std::string(field->bytes.begin(), field->bytes.end());
}

std::string base64(std::span<const unsigned char> bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned value = (bytes[i] << 16) |
            ((i + 1 < bytes.size() ? bytes[i + 1] : 0) << 8) |
            (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        text.push_back(alphabet[(value >> 18) & 63]);
        text.push_back(alphabet[(value >> 12) & 63]);
        text.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        text.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return text;
}
std::vector<unsigned char> base64UrlDecode(std::string_view text) {
    std::string padded;
    padded.reserve(text.size() + 3);
    for (const auto c : text) {
        if (c == '-') padded.push_back('+');
        else if (c == '_') padded.push_back('/');
        else padded.push_back(static_cast<char>(c));
    }
    while (padded.size() % 4) padded.push_back('=');
    auto value = [](char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    out.reserve(padded.size() / 4 * 3);
    for (std::size_t i = 0; i + 3 < padded.size(); i += 4) {
        const auto a = value(padded[i]);
        const auto b = value(padded[i + 1]);
        const auto c = padded[i + 2] == '=' ? 0 : value(padded[i + 2]);
        const auto d = padded[i + 3] == '=' ? 0 : value(padded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            wipe(padded.data(), padded.size());
            throw Failure("Steam sign-in failed. Sign in again.", 1010);
        }
        out.push_back(static_cast<unsigned char>((a << 2) | (b >> 4)));
        if (padded[i + 2] != '=') out.push_back(static_cast<unsigned char>(((b & 15) << 4) | (c >> 2)));
        if (padded[i + 3] != '=') out.push_back(static_cast<unsigned char>(((c & 3) << 6) | d));
    }
    wipe(padded.data(), padded.size());
    return out;
}
std::string urlEncode(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size() * 3);
    for (const auto c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~')
            encoded.push_back(static_cast<char>(byte));
        else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4]);
            encoded.push_back(hex[byte & 15]);
        }
    }
    return encoded;
}
std::vector<unsigned char> hexBytes(std::string_view hex) {
    if (hex.empty() || hex.size() % 2) throw Failure("Steam returned an invalid sign-in key.", 1013);
    std::vector<unsigned char> bytes(hex.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        auto nibble = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const auto high = nibble(hex[i * 2]), low = nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) throw Failure("Steam returned an invalid sign-in key.", 1013);
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return bytes;
}
std::string hexLower(std::span<const unsigned char> bytes) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string text(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        text[i * 2] = alphabet[bytes[i] >> 4];
        text[i * 2 + 1] = alphabet[bytes[i] & 15];
    }
    return text;
}
void randomBytes(std::span<unsigned char> bytes) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
#else
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
#endif
}

#ifdef _WIN32
struct Algorithm {
    BCRYPT_ALG_HANDLE handle{};
    explicit Algorithm(const wchar_t *name, ULONG flags = 0) {
        if (BCryptOpenAlgorithmProvider(&handle, name, nullptr, flags) != 0)
            throw Failure("Steam sign-in could not start. Try again.", 1010);
    }
    ~Algorithm() {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
    Algorithm(const Algorithm &) = delete;
    Algorithm &operator=(const Algorithm &) = delete;
    Algorithm(Algorithm &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
    Algorithm &operator=(Algorithm &&) = delete;
};
struct Key {
    BCRYPT_KEY_HANDLE handle{};
    ~Key() {
        if (handle) BCryptDestroyKey(handle);
    }
    Key() = default;
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
    Key(Key &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
    Key &operator=(Key &&) = delete;
};
Key importRsa(std::span<const unsigned char> modulus, std::span<const unsigned char> exponent) {
    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = static_cast<ULONG>(modulus.size() * 8);
    header.cbPublicExp = static_cast<ULONG>(exponent.size());
    header.cbModulus = static_cast<ULONG>(modulus.size());
    std::vector<unsigned char> blob(sizeof(header) + exponent.size() + modulus.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), exponent.data(), exponent.size());
    std::memcpy(blob.data() + sizeof(header) + exponent.size(), modulus.data(), modulus.size());
    Algorithm algorithm(BCRYPT_RSA_ALGORITHM);
    Key key;
    if (BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key.handle, blob.data(),
            static_cast<ULONG>(blob.size()), 0) != 0)
        throw Failure("Steam returned an invalid sign-in key.", 1013);
    wipe(blob.data(), blob.size());
    return key;
}
struct Der {
    std::span<const unsigned char> value;
    std::size_t total{};
};
Der readDer(std::span<const unsigned char> input, unsigned char tag) {
    if (input.size() < 2 || input[0] != tag)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    std::size_t length = input[1], header = 2;
    if (length & 0x80) {
        const auto count = length & 0x7f;
        if (count == 0 || count > 3 || input.size() < 2 + count)
            throw Failure("Steam sign-in could not start. Try again.", 1010);
        length = 0;
        for (std::size_t i = 0; i < count; ++i) length = (length << 8) | input[2 + i];
        header += count;
    }
    if (input.size() < header + length)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    return {input.subspan(header, length), header + length};
}
Key importSpki(std::span<const unsigned char> spki) {
    const auto outer = readDer(spki, 0x30);
    const auto algorithm = readDer(outer.value, 0x30);
    const auto bits = readDer(outer.value.subspan(algorithm.total), 0x03);
    if (bits.value.empty() || bits.value[0] != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    const auto key = readDer(bits.value.subspan(1), 0x30);
    auto modulus = readDer(key.value, 0x02);
    const auto exponent = readDer(key.value.subspan(modulus.total), 0x02);
    if (!modulus.value.empty() && modulus.value[0] == 0) modulus.value = modulus.value.subspan(1);
    return importRsa(modulus.value, exponent.value);
}
Secret rsaEncrypt(Key &key, std::span<const unsigned char> plain, bool oaep) {
    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA1_ALGORITHM, nullptr, 0};
    ULONG size{};
    auto *pad = oaep ? &padding : nullptr;
    const auto flags = oaep ? BCRYPT_PAD_OAEP : BCRYPT_PAD_PKCS1;
    if (BCryptEncrypt(key.handle, const_cast<unsigned char *>(plain.data()), static_cast<ULONG>(plain.size()),
            pad, nullptr, 0, nullptr, 0, &size, flags) != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    Secret secret(size);
    if (BCryptEncrypt(key.handle, const_cast<unsigned char *>(plain.data()), static_cast<ULONG>(plain.size()),
            pad, nullptr, 0, secret.bytes.data(), size, &size, flags) != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    secret.bytes.resize(size);
    return secret;
}
void sha1Digest(bool hmac, std::span<const unsigned char> key, std::span<const unsigned char> data,
    std::span<unsigned char, 20> out) {
    Algorithm algorithm(BCRYPT_SHA1_ALGORITHM, hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm.handle, &hash, nullptr, 0,
            hmac ? const_cast<unsigned char *>(key.data()) : nullptr, hmac ? static_cast<ULONG>(key.size()) : 0,
            0) != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    const auto hashed = BCryptHashData(hash, const_cast<unsigned char *>(data.data()),
                            static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, out.data(), 20, 0) == 0;
    BCryptDestroyHash(hash);
    if (!hashed) throw Failure("Steam sign-in could not start. Try again.", 1010);
}
void sha1Hmac(std::span<const unsigned char> key, std::span<const unsigned char> data,
    std::span<unsigned char, 20> out) {
    sha1Digest(true, key, data, out);
}
void sha1(std::span<const unsigned char> data, std::span<unsigned char, 20> out) {
    sha1Digest(false, {}, data, out);
}
Secret aes(std::span<const unsigned char, 32> key, std::span<const unsigned char> input,
    std::span<unsigned char> iv, bool encrypt, bool cbc) {
    Algorithm algorithm(BCRYPT_AES_ALGORITHM);
    std::wstring mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
    if (BCryptSetProperty(algorithm.handle, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode.data()),
            static_cast<ULONG>((mode.size() + 1) * sizeof(wchar_t)), 0) != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    Key symmetric;
    if (BCryptGenerateSymmetricKey(algorithm.handle, &symmetric.handle, nullptr, 0,
            const_cast<unsigned char *>(key.data()), 32, 0) != 0)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
    ULONG size{};
    const auto flags = cbc ? BCRYPT_BLOCK_PADDING : 0u;
    auto *ivData = cbc ? iv.data() : nullptr;
    const auto ivSize = cbc ? static_cast<ULONG>(iv.size()) : 0u;
    auto crypt = [&](unsigned char *output, ULONG outputSize, ULONG &written) {
        return encrypt ? BCryptEncrypt(symmetric.handle, const_cast<unsigned char *>(input.data()),
                             static_cast<ULONG>(input.size()), nullptr, ivData, ivSize, output, outputSize,
                             &written, flags)
                       : BCryptDecrypt(symmetric.handle, const_cast<unsigned char *>(input.data()),
                             static_cast<ULONG>(input.size()), nullptr, ivData, ivSize, output, outputSize,
                             &written, flags);
    };
    if (crypt(nullptr, 0, size) != 0) throw Failure("The Steam connection could not be secured.", 1010);
    Secret secret(size);
    if (crypt(secret.bytes.data(), size, size) != 0)
        throw Failure("The Steam connection could not be secured.", 1010);
    secret.bytes.resize(size);
    return secret;
}
#else
struct Key {
    EVP_PKEY *handle{};
    ~Key() { EVP_PKEY_free(handle); }
    Key() = default;
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
    Key(Key &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
    Key &operator=(Key &&other) noexcept {
        if (this == &other) return *this;
        EVP_PKEY_free(handle);
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }
};
Key importRsa(std::span<const unsigned char> modulus, std::span<const unsigned char> exponent) {
    BIGNUM *mod = BN_bin2bn(modulus.data(), static_cast<int>(modulus.size()), nullptr);
    BIGNUM *exp = BN_bin2bn(exponent.data(), static_cast<int>(exponent.size()), nullptr);
    OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, mod);
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, exp);
    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    Key key;
    const auto ok = context && EVP_PKEY_fromdata_init(context) == 1 &&
        EVP_PKEY_fromdata(context, &key.handle, EVP_PKEY_PUBLIC_KEY, params) == 1;
    EVP_PKEY_CTX_free(context);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_free(mod);
    BN_free(exp);
    if (!ok) throw Failure("Steam returned an invalid sign-in key.", 1013);
    return key;
}
Key importSpki(std::span<const unsigned char> spki) {
    const unsigned char *cursor = spki.data();
    Key key;
    key.handle = d2i_PUBKEY(nullptr, &cursor, static_cast<long>(spki.size()));
    if (!key.handle) throw Failure("Steam sign-in could not start. Try again.", 1010);
    return key;
}
Secret rsaEncrypt(Key &key, std::span<const unsigned char> plain, bool oaep) {
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(key.handle, nullptr);
    std::size_t size = 0;
    const auto ready = context && EVP_PKEY_encrypt_init(context) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(context, oaep ? RSA_PKCS1_OAEP_PADDING : RSA_PKCS1_PADDING) == 1 &&
        (!oaep || (EVP_PKEY_CTX_set_rsa_oaep_md(context, EVP_sha1()) == 1 &&
                      EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha1()) == 1)) &&
        EVP_PKEY_encrypt(context, nullptr, &size, plain.data(), plain.size()) == 1;
    Secret secret(ready ? size : 0);
    const auto encrypted = ready &&
        EVP_PKEY_encrypt(context, secret.bytes.data(), &size, plain.data(), plain.size()) == 1;
    EVP_PKEY_CTX_free(context);
    if (!encrypted) throw Failure("Steam sign-in could not start. Try again.", 1010);
    secret.bytes.resize(size);
    return secret;
}
void sha1Hmac(std::span<const unsigned char> key, std::span<const unsigned char> data,
    std::span<unsigned char, 20> out) {
    EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    EVP_MAC_CTX *context = EVP_MAC_CTX_new(mac);
    OSSL_PARAM params[]{OSSL_PARAM_construct_utf8_string("digest", const_cast<char *>("SHA1"), 0),
        OSSL_PARAM_construct_end()};
    const auto ok = context &&
        EVP_MAC_init(context, key.data(), key.size(), params) == 1 &&
        EVP_MAC_update(context, data.data(), data.size()) == 1 &&
        EVP_MAC_final(context, out.data(), nullptr, out.size()) == 1;
    EVP_MAC_CTX_free(context);
    EVP_MAC_free(mac);
    if (!ok) throw Failure("Steam sign-in could not start. Try again.", 1010);
}
void sha1(std::span<const unsigned char> data, std::span<unsigned char, 20> out) {
    unsigned int size = 20;
    if (EVP_Digest(data.data(), data.size(), out.data(), &size, EVP_sha1(), nullptr) != 1)
        throw Failure("Steam sign-in could not start. Try again.", 1010);
}
Secret aes(std::span<const unsigned char, 32> key, std::span<const unsigned char> input,
    std::span<unsigned char> iv, bool encrypt, bool cbc) {
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    const auto *cipher = cbc ? EVP_aes_256_cbc() : EVP_aes_256_ecb();
    Secret secret(input.size() + 16);
    int written = 0, finalSize = 0;
    const auto ok = context &&
        EVP_CipherInit_ex(context, cipher, nullptr, key.data(), cbc ? iv.data() : nullptr, encrypt ? 1 : 0) ==
            1 &&
        EVP_CIPHER_CTX_set_padding(context, cbc ? 1 : 0) == 1 &&
        EVP_CipherUpdate(context, secret.bytes.data(), &written, input.data(), static_cast<int>(input.size())) ==
            1 &&
        EVP_CipherFinal_ex(context, secret.bytes.data() + written, &finalSize) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) throw Failure("The Steam connection could not be secured.", 1010);
    secret.bytes.resize(static_cast<std::size_t>(written + finalSize));
    return secret;
}
#endif

struct SessionCipher {
    std::array<unsigned char, 32> key{};
    ~SessionCipher() { wipe(key.data(), key.size()); }
    Secret process(std::span<const unsigned char> input, bool encrypt) const {
        auto keySpan = std::span<const unsigned char, 32>(key);
        std::array<unsigned char, 16> iv{};
        if (encrypt) {
            randomBytes(std::span(iv).subspan(13));
            std::vector<unsigned char> macInput(input.size() + 3);
            std::copy(iv.begin() + 13, iv.end(), macInput.begin());
            std::copy(input.begin(), input.end(), macInput.begin() + 3);
            std::array<unsigned char, 20> mac{};
            sha1Hmac(std::span(key).first(16), macInput, mac);
            wipe(macInput.data(), macInput.size());
            std::copy(mac.begin(), mac.begin() + 13, iv.begin());
            auto sealedIv = aes(keySpan, iv, {}, true, false);
            auto sealed = aes(keySpan, input, iv, true, true);
            sealedIv.bytes.insert(sealedIv.bytes.end(), sealed.bytes.begin(), sealed.bytes.end());
            return sealedIv;
        }
        if (input.size() < 16) throw Failure("The Steam connection could not be secured.", 1010);
        auto openedIv = aes(keySpan, input.first(16), {}, false, false);
        if (openedIv.bytes.size() != 16) throw Failure("The Steam connection could not be secured.", 1010);
        std::copy(openedIv.bytes.begin(), openedIv.bytes.end(), iv.begin());
        auto cipherIv = iv;
        auto plain = aes(keySpan, input.subspan(16), cipherIv, false, true);
        std::vector<unsigned char> macInput(plain.bytes.size() + 3);
        std::copy(iv.begin() + 13, iv.end(), macInput.begin());
        std::copy(plain.bytes.begin(), plain.bytes.end(), macInput.begin() + 3);
        std::array<unsigned char, 20> mac{};
        sha1Hmac(std::span(key).first(16), macInput, mac);
        wipe(macInput.data(), macInput.size());
        if (!std::equal(mac.begin(), mac.begin() + 13, iv.begin()))
            throw Failure("The Steam connection could not be secured.", 1010);
        return plain;
    }
};

constexpr unsigned char kSteamKey[] = {
    0x30, 0x81, 0x9D, 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01,
    0x05, 0x00, 0x03, 0x81, 0x8B, 0x00, 0x30, 0x81, 0x87, 0x02, 0x81, 0x81, 0x00, 0xDF, 0xEC, 0x1A,
    0xD6, 0x2C, 0x10, 0x66, 0x2C, 0x17, 0x35, 0x3A, 0x14, 0xB0, 0x7C, 0x59, 0x11, 0x7F, 0x9D, 0xD3,
    0xD8, 0x2B, 0x7A, 0xE3, 0xE0, 0x15, 0xCD, 0x19, 0x1E, 0x46, 0xE8, 0x7B, 0x87, 0x74, 0xA2, 0x18,
    0x46, 0x31, 0xA9, 0x03, 0x14, 0x79, 0x82, 0x8E, 0xE9, 0x45, 0xA2, 0x49, 0x12, 0xA9, 0x23, 0x68,
    0x73, 0x89, 0xCF, 0x69, 0xA1, 0xB1, 0x61, 0x46, 0xBD, 0xC1, 0xBE, 0xBF, 0xD6, 0x01, 0x1B, 0xD8,
    0x81, 0xD4, 0xDC, 0x90, 0xFB, 0xFE, 0x4F, 0x52, 0x73, 0x66, 0xCB, 0x95, 0x70, 0xD7, 0xC5, 0x8E,
    0xBA, 0x1C, 0x7A, 0x33, 0x75, 0xA1, 0x62, 0x34, 0x46, 0xBB, 0x60, 0xB7, 0x80, 0x68, 0xFA, 0x13,
    0xA7, 0x7A, 0x8A, 0x37, 0x4B, 0x9E, 0xC6, 0xF4, 0x5D, 0x5F, 0x3A, 0x99, 0xF9, 0x9E, 0xC4, 0x3A,
    0xE9, 0x63, 0xA2, 0xBB, 0x88, 0x19, 0x28, 0xE0, 0xE7, 0x14, 0xC0, 0x42, 0x89, 0x02, 0x01, 0x11};

struct SteamMessage {
    std::uint32_t emsg{};
    std::uint64_t steamId{};
    std::uint32_t sessionId{};
    std::uint64_t jobTarget{};
    std::vector<unsigned char> body;
};
struct SteamLink {
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
#else
    int socket = -1;
#endif
    bool secure{};
    SessionCipher cipher;
    std::vector<unsigned char> pending;
    std::uint64_t steamId{};
    std::uint32_t sessionId{};
    std::uint64_t nextJob = 1;
    std::vector<Secret> tokens;
    std::deque<SteamMessage> inbox;
    ~SteamLink() { close(); }
    SteamLink() = default;
    SteamLink(const SteamLink &) = delete;
    SteamLink &operator=(const SteamLink &) = delete;
    void close() {
#ifdef _WIN32
        if (socket != INVALID_SOCKET) closesocket(socket);
        socket = INVALID_SOCKET;
#else
        if (socket >= 0) ::close(socket);
        socket = -1;
#endif
    }
    bool waitRead(int milliseconds) {
#ifdef _WIN32
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket, &set);
        timeval time{milliseconds / 1000, (milliseconds % 1000) * 1000};
        return select(0, &set, nullptr, nullptr, &time) > 0;
#else
        pollfd pollValue{socket, POLLIN, 0};
        return poll(&pollValue, 1, milliseconds) > 0;
#endif
    }
    void sendRaw(std::span<const unsigned char> bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
#ifdef _WIN32
            const auto count = ::send(socket, reinterpret_cast<const char *>(bytes.data() + sent),
                static_cast<int>(bytes.size() - sent), 0);
#else
            const auto count = ::send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
            if (count <= 0) throw Failure("Steam disconnected. Sign in again.", 1010);
            sent += static_cast<std::size_t>(count);
        }
    }
    void sendPacket(std::span<const unsigned char> message) {
        std::array<unsigned char, 8> header{};
        const auto size = static_cast<std::uint32_t>(message.size());
        for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<unsigned char>(size >> (8 * i));
        header[4] = 'V';
        header[5] = 'T';
        header[6] = '0';
        header[7] = '1';
        sendRaw(header);
        sendRaw(message);
    }
    std::optional<std::vector<unsigned char>> nextPacket(Clock::time_point deadline) {
        while (pending.size() < 8 || pending.size() < 8 + readU32(pending, 0)) {
            if (Clock::now() >= deadline) return {};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (!waitRead(static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining.count(), 200))))
                continue;
            unsigned char buffer[16384];
#ifdef _WIN32
            const auto count = recv(socket, reinterpret_cast<char *>(buffer), sizeof(buffer), 0);
#else
            const auto count = recv(socket, buffer, sizeof(buffer), 0);
#endif
            if (count <= 0) throw Failure("Steam disconnected. Sign in again.", 1010);
            pending.insert(pending.end(), buffer, buffer + count);
            if (pending.size() >= 4 && readU32(pending, 0) > 4 * 1024 * 1024)
                throw Failure("Steam sent a message that is too large.", 1010);
        }
        const auto size = readU32(pending, 0);
        if (readU32(pending, 4) != 0x31305456u)
            throw Failure("Steam sent an invalid message.", 1010);
        std::vector<unsigned char> packet(pending.begin() + 8, pending.begin() + 8 + size);
        pending.erase(pending.begin(), pending.begin() + 8 + size);
        return packet;
    }
};

void sendPlain(SteamLink &link, std::uint32_t emsg, std::span<const unsigned char> body) {
    std::vector<unsigned char> message;
    appendU32(message, emsg);
    appendU64(message, ~0ull);
    appendU64(message, ~0ull);
    message.insert(message.end(), body.begin(), body.end());
    link.sendPacket(message);
}
void handshake(SteamLink &link) {
    auto packet = link.nextPacket(Clock::now() + std::chrono::seconds(15));
    if (!packet || packet->size() < 28 || readU32(*packet, 0) != 1303)
        throw Failure("Steam did not respond in time. Check your connection and try again.", 1010);
    const auto challenge = std::span(*packet).subspan(28);
    if (challenge.size() < 16) throw Failure("The Steam connection could not be secured.", 1010);
    std::array<unsigned char, 32> sessionKey{};
    randomBytes(sessionKey);
    std::vector<unsigned char> blob(sessionKey.size() + challenge.size());
    std::copy(sessionKey.begin(), sessionKey.end(), blob.begin());
    std::copy(challenge.begin(), challenge.end(), blob.begin() + sessionKey.size());
    auto key = importSpki(kSteamKey);
    auto encrypted = rsaEncrypt(key, blob, true);
    wipe(blob.data(), blob.size());
    if (encrypted.bytes.size() != 128) throw Failure("The Steam connection could not be secured.", 1010);
    std::copy(sessionKey.begin(), sessionKey.end(), link.cipher.key.begin());
    wipe(sessionKey.data(), sessionKey.size());
    std::vector<unsigned char> body;
    appendU32(body, 1);
    appendU32(body, 128);
    body.insert(body.end(), encrypted.bytes.begin(), encrypted.bytes.end());
    appendU32(body, crc32(encrypted.bytes));
    appendU32(body, 0);
    sendPlain(link, 1304, body);
    auto result = link.nextPacket(Clock::now() + std::chrono::seconds(15));
    if (!result || result->size() < 24 || readU32(*result, 0) != 1305 || readU32(*result, 20) != 1)
        throw Failure("The Steam connection could not be secured.", 1010);
    link.secure = true;
}
void sendProto(SteamLink &link, std::uint32_t emsg, std::span<const unsigned char> body, std::uint64_t job) {
    std::vector<unsigned char> header;
    if (link.steamId) protoFixed64(header, 1, link.steamId);
    if (link.sessionId) protoUint(header, 2, link.sessionId);
    protoFixed64(header, 10, job);
    protoFixed64(header, 11, ~0ull);
    std::vector<unsigned char> message;
    appendU32(message, emsg | 0x80000000u);
    appendU32(message, static_cast<std::uint32_t>(header.size()));
    message.insert(message.end(), header.begin(), header.end());
    message.insert(message.end(), body.begin(), body.end());
    auto sealed = link.cipher.process(message, true);
    link.sendPacket(sealed.bytes);
}
SteamMessage decode(std::span<const unsigned char> packet) {
    SteamMessage message;
    const auto raw = readU32(packet, 0);
    message.emsg = raw & 0x7FFFFFFFu;
    if ((raw & 0x80000000u) == 0) {
        if (packet.size() >= 20) message.body.assign(packet.begin() + 20, packet.end());
        return message;
    }
    if (packet.size() < 8) throw Failure("Steam sent an incomplete message.", 1010);
    const auto headerSize = readU32(packet, 4);
    if (packet.size() < 8 + headerSize) throw Failure("Steam sent an incomplete message.", 1010);
    const auto header = protoFields(packet.subspan(8, headerSize));
    if (const auto id = findField(header, 1); id && id->wire == 1) message.steamId = id->number;
    if (const auto session = findField(header, 2)) message.sessionId = static_cast<std::uint32_t>(session->number);
    if (const auto target = findField(header, 11); target && target->wire == 1 && target->number != ~0ull)
        message.jobTarget = target->number;
    else if (const auto source = findField(header, 10); source && source->wire == 1 && source->number != ~0ull)
        message.jobTarget = source->number;
    message.body.assign(packet.begin() + 8 + headerSize, packet.end());
    return message;
}

std::optional<std::vector<unsigned char>> inflateGzip(std::span<const unsigned char> bytes, std::size_t expected) {
    if (expected == 0 || expected > 4 * 1024 * 1024 || bytes.size() > 4 * 1024 * 1024) return std::nullopt;
    std::vector<unsigned char> out(expected);
    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(bytes.data()));
    stream.avail_in = static_cast<uInt>(bytes.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(expected);
    if (inflateInit2(&stream, 16 + 15) != Z_OK) return std::nullopt;
    const auto result = inflate(&stream, Z_FINISH);
    const auto written = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || written != expected) return std::nullopt;
    return out;
}
void noteMessage(SteamLink &link, SteamMessage message);
void expandMulti(SteamLink &link, std::span<const unsigned char> body) {
    const auto fields = protoFields(body);
    const auto payload = findField(fields, 2);
    if (!payload || payload->bytes.empty()) return;
    std::vector<unsigned char> plain;
    std::span<const unsigned char> bytes = payload->bytes;
    if (const auto unzipped = findField(fields, 1); unzipped && unzipped->number) {
        if (unzipped->number > 4 * 1024 * 1024) throw Failure("Steam sent a message that is too large.", 1010);
        auto inflated = inflateGzip(payload->bytes, static_cast<std::size_t>(unzipped->number));
        if (!inflated) throw Failure("Steam sent an invalid message.", 1010);
        plain = std::move(*inflated);
        bytes = plain;
    }
    std::size_t offset = 0;
    while (offset + 4 <= bytes.size()) {
        const auto size = readU32(bytes, offset);
        offset += 4;
        if (!size || size > bytes.size() - offset) throw Failure("Steam sent an invalid message.", 1010);
        noteMessage(link, decode(bytes.subspan(offset, size)));
        offset += size;
    }
}
void noteMessage(SteamLink &link, SteamMessage message) {
    if (message.emsg == 1) {
        expandMulti(link, message.body);
        return;
    }
    if (message.emsg == 703) {
        sendProto(link, 703, {}, link.nextJob++);
        return;
    }
    if (message.emsg == 757) throw Failure("Steam disconnected. Sign in again.", 1010);
    if (message.emsg == 779) {
        for (const auto &field : protoFields(message.body)) {
            if (field.id != 2 || field.bytes.empty()) continue;
            Secret token;
            token.bytes = field.bytes;
            link.tokens.push_back(std::move(token));
        }
    }
    if (message.steamId) link.steamId = message.steamId;
    if (message.sessionId) link.sessionId = message.sessionId;
    link.inbox.push_back(std::move(message));
}
std::optional<SteamMessage> pollSteam(SteamLink &link, Clock::time_point deadline) {
    if (!link.inbox.empty()) {
        auto message = std::move(link.inbox.front());
        link.inbox.pop_front();
        return message;
    }
    auto packet = link.nextPacket(deadline);
    if (!packet) return {};
    auto plain = link.secure ? link.cipher.process(*packet, false) : Secret{};
    auto &bytes = link.secure ? plain.bytes : *packet;
    if (!link.secure) return decode(bytes);
    noteMessage(link, decode(bytes));
    if (link.inbox.empty()) return {};
    auto message = std::move(link.inbox.front());
    link.inbox.pop_front();
    return message;
}

SteamMessage waitSteam(SteamLink &link, std::uint32_t emsg, std::uint64_t job, int seconds) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        auto message = pollSteam(link, std::min(deadline, Clock::now() + std::chrono::milliseconds(200)));
        if (!message) continue;
        if (message->emsg == emsg && (job == 0 || message->jobTarget == job)) return std::move(*message);
    }
    throw Failure("Steam did not respond in time. Check your connection and try again.", 1010);
}

bool connectEndpoint(SteamLink &link, const std::string &endpoint) {
    const auto colon = endpoint.rfind(':');
    if (colon == endpoint.npos || colon == 0) return false;
    const auto host = endpoint.substr(0, colon);
    const auto port = endpoint.substr(colon + 1);
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *result{};
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) return false;
    for (auto *info = result; info; info = info->ai_next) {
        link.close();
#ifdef _WIN32
        link.socket = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (link.socket == INVALID_SOCKET) continue;
        if (connect(link.socket, info->ai_addr, static_cast<int>(info->ai_addrlen)) != 0) continue;
#else
        link.socket = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (link.socket < 0) continue;
        if (connect(link.socket, info->ai_addr, info->ai_addrlen) != 0) continue;
#endif
        freeaddrinfo(result);
        try {
            handshake(link);
            return true;
        } catch (...) {
            link.close();
            link.secure = false;
            return false;
        }
    }
    freeaddrinfo(result);
    return false;
}

struct HttpReply {
    int status{};
    int result{1};
    std::vector<unsigned char> body;
};

#ifdef _WIN32
HttpReply https(std::wstring path, std::string_view body, bool post) {
    std::wstring agent;
    for (const auto c : std::string_view(GW2_PRODUCT_NAME)) agent.push_back(static_cast<unsigned char>(c));
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);
    HINTERNET connection = WinHttpConnect(session, L"api.steampowered.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, post ? L"POST" : L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                  : nullptr;
    const auto sent = request &&
        WinHttpSendRequest(request, post ? L"Content-Type: application/x-www-form-urlencoded\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
            post ? (DWORD)-1 : 0, post ? const_cast<char *>(body.data()) : nullptr,
            post ? static_cast<DWORD>(body.size()) : 0, post ? static_cast<DWORD>(body.size()) : 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    HttpReply reply;
    if (sent) {
        DWORD status{}, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX);
        reply.status = static_cast<int>(status);
        wchar_t eresult[16]{};
        size = sizeof(eresult);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"x-eresult", eresult, &size, WINHTTP_NO_HEADER_INDEX))
            reply.result = _wtoi(eresult);
        Secret buffer(8192);
        for (;;) {
            DWORD count{};
            if (!WinHttpReadData(request, buffer.bytes.data(), static_cast<DWORD>(buffer.bytes.size()), &count))
                break;
            if (!count) break;
            if (reply.body.size() + count > 1024 * 1024)
                throw Failure("Steam sent a message that is too large.", 1010);
            reply.body.insert(reply.body.end(), buffer.bytes.begin(), buffer.bytes.begin() + count);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!sent) throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
    return reply;
}
#else
HttpReply https(std::string_view path, std::string_view body, bool post) {
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
    const auto url = "https://api.steampowered.com" + std::string(path);
    HttpReply reply;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_slist *headers = post ? curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded") : nullptr;
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    if (post) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, GW2_PRODUCT_NAME);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION,
        +[](char *data, std::size_t size, std::size_t count, void *context) -> std::size_t {
            const auto length = size * count;
            auto *reply = static_cast<HttpReply *>(context);
            std::string_view line(data, length);
            constexpr std::string_view name = "x-eresult:";
            if (line.size() >= name.size() &&
                std::equal(name.begin(), name.end(), line.begin(),
                    [](char a, char b) { return std::tolower(a) == b; })) {
                reply->result = std::atoi(line.substr(name.size()).data());
            }
            return length;
        });
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &reply);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char *data, std::size_t size, std::size_t count, void *context) -> std::size_t {
            const auto length = size * count;
            auto *reply = static_cast<HttpReply *>(context);
            if (reply->body.size() + length > 1024 * 1024) return 0;
            reply->body.insert(reply->body.end(), data, data + length);
            return length;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &reply);
    const auto code = curl_easy_perform(curl.get());
    long status{};
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    if (code != CURLE_OK) throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
    reply.status = static_cast<int>(status);
    return reply;
}
#endif

std::vector<unsigned char> steamCall(std::string_view method, std::span<const unsigned char> request) {
    const auto encoded = urlEncode(base64(request));
    const auto body = "input_protobuf_encoded=" + encoded;
#ifdef _WIN32
    const auto path = std::wstring(L"/IAuthenticationService/") +
        std::wstring(method.begin(), method.end()) + L"/v1/";
#else
    const auto path = "/IAuthenticationService/" + std::string(method) + "/v1/";
#endif
    auto reply = https(path, body, true);
    if (reply.status != 200 || reply.result != 1)
        throw Failure("Steam sign-in failed (" + std::to_string(reply.result) + "). Try signing in again.", 1013);
    return std::move(reply.body);
}

struct PipeMessage {
    std::uint32_t kind{};
    std::vector<std::string> fields;
};
class Pipe {
  public:
    bool available() {
#ifdef _WIN32
        DWORD bytes{};
        if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &bytes, nullptr))
            throw Failure("The Steam sign-in request is not ready.", 1010);
        return bytes >= 4;
#else
        pollfd pollValue{STDIN_FILENO, POLLIN, 0};
        return poll(&pollValue, 1, 0) > 0;
#endif
    }
    PipeMessage receive(Clock::time_point deadline) {
        auto header = read(4, deadline);
        const auto size = readU32(header, 0);
        if (size < 4 || size > 65536) throw Failure("The Steam sign-in request is not ready.", 1010);
        auto body = read(size, deadline);
        PipeMessage message;
        message.kind = readU32(body, 0);
        std::size_t offset = 4;
        while (offset < body.size()) {
            if (body.size() - offset < 4 || message.fields.size() == 4)
                throw Failure("The Steam sign-in request is not ready.", 1010);
            const auto length = readU32(body, offset);
            offset += 4;
            if (length > body.size() - offset) throw Failure("The Steam sign-in request is not ready.", 1010);
            message.fields.emplace_back(reinterpret_cast<char *>(body.data() + offset), length);
            offset += length;
        }
        wipe(body.data(), body.size());
        return message;
    }
    void send(std::uint32_t kind, std::initializer_list<std::string_view> fields) {
        std::vector<unsigned char> body;
        appendU32(body, kind);
        for (const auto field : fields) {
            appendU32(body, static_cast<std::uint32_t>(field.size()));
            body.insert(body.end(), field.begin(), field.end());
        }
        if (body.size() > 65536) throw Failure("Steam sent a message that is too large.", 1010);
        std::array<unsigned char, 4> header{};
        const auto size = static_cast<std::uint32_t>(body.size());
        for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<unsigned char>(size >> (8 * i));
        write(header);
        write(body);
        wipe(body.data(), body.size());
    }

  private:
    std::vector<unsigned char> read(std::size_t size, Clock::time_point deadline) {
        std::vector<unsigned char> bytes(size);
        std::size_t got = 0;
        while (got < size) {
            if (Clock::now() >= deadline) throw Failure("Steam did not respond in time. Check your connection and try again.", 1010);
#ifdef _WIN32
            DWORD available{};
            if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr))
                throw Failure("The Steam sign-in request is not ready.", 1010);
            if (!available) {
                Sleep(10);
                continue;
            }
            DWORD count{};
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes.data() + got,
                    static_cast<DWORD>(std::min<std::size_t>(size - got, available)), &count, nullptr) ||
                !count)
                throw Failure("The Steam sign-in request is not ready.", 1010);
            got += count;
#else
            pollfd pollValue{STDIN_FILENO, POLLIN, 0};
            if (poll(&pollValue, 1, 10) <= 0) continue;
            const auto count = ::read(STDIN_FILENO, bytes.data() + got, size - got);
            if (count <= 0) throw Failure("The Steam sign-in request is not ready.", 1010);
            got += static_cast<std::size_t>(count);
#endif
        }
        return bytes;
    }
    void write(std::span<const unsigned char> bytes) {
#ifdef _WIN32
        DWORD count{};
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &count,
                nullptr) ||
            count != bytes.size())
            throw Failure("The Steam sign-in request is not ready.", 1010);
        FlushFileBuffers(GetStdHandle(STD_OUTPUT_HANDLE));
#else
        if (::write(STDOUT_FILENO, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()))
            throw Failure("The Steam sign-in request is not ready.", 1010);
#endif
    }
};

std::uint64_t clientOs() {
#ifdef _WIN32
    return 20;
#else
    return static_cast<std::uint64_t>(static_cast<std::int32_t>(-184));
#endif
}
std::string computerName() {
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameA(name, &size) || size == 0) return "DESKTOP";
    return std::string(name, size);
#else
    char name[256]{};
    if (gethostname(name, sizeof(name) - 1) != 0 || name[0] == 0) return "DESKTOP";
    return name;
#endif
}
std::vector<unsigned char> deviceDetails() {
    std::vector<unsigned char> details;
    protoString(details, 1, computerName());
    protoUint(details, 2, 1);
    protoUint(details, 3, clientOs());
    protoUint(details, 4, 1);
    return details;
}
std::string qrBits(std::string_view url) {
    const auto code = qrcodegen::QrCode::encodeText(std::string(url).c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = code.getSize();
    const int side = modules + 8;
    std::string bits(static_cast<std::size_t>(side * side), '0');
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (code.getModule(x, y)) bits[static_cast<std::size_t>((y + 4) * side + x + 4)] = '1';
    return bits;
}
void showQr(Pipe &pipe, std::string_view url) {
    if (url.size() < 16 || url.substr(0, 8) != "https://" || url.substr(8, 6) != "s.team")
        throw Failure("Steam returned an invalid QR code.", 1010);
    pipe.send(1, {qrBits(url)});
}

struct Login {
    std::string name;
    Secret token;
};
Login signIn(Pipe &pipe, const PipeMessage &request) {
    const auto details = deviceDetails();
    std::vector<unsigned char> begin;
    std::uint64_t clientId{};
    std::vector<unsigned char> requestId;
    std::uint64_t steamId{};
    int guard = 1;
    if (request.kind == 1) {
        protoBytes(begin, 3, details);
        const auto response = protoFields(steamCall("BeginAuthSessionViaQR", begin));
        const auto id = findField(response, 1);
        clientId = id ? id->number : 0;
        if (const auto challenge = findField(response, 3)) requestId = challenge->bytes;
        if (const auto url = findField(response, 2)) showQr(pipe, fieldText(url));
    } else {
        std::vector<unsigned char> keyRequest;
        protoString(keyRequest, 1, request.fields[0]);
        const auto keyReply = protoFields(steamCall("GetPasswordRSAPublicKey", keyRequest));
        auto modulus = hexBytes(fieldText(findField(keyReply, 1)));
        auto exponent = hexBytes(fieldText(findField(keyReply, 2)));
        const auto stamp = findField(keyReply, 3);
        const auto timestamp = stamp ? stamp->number : 0;
        Secret password(request.fields[1].size());
        std::copy(request.fields[1].begin(), request.fields[1].end(), password.bytes.begin());
        auto rsa = importRsa(modulus, exponent);
        auto encrypted = rsaEncrypt(rsa, password.bytes, false);
        password.clear();
        wipe(modulus.data(), modulus.size());
        const auto encoded = base64(encrypted.bytes);
        protoString(begin, 1, GW2_PRODUCT_NAME);
        protoString(begin, 2, request.fields[0]);
        protoString(begin, 3, encoded);
        protoUint(begin, 4, timestamp);
        protoUint(begin, 6, 1);
        protoUint(begin, 7, 1);
        protoBytes(begin, 9, details);
        const auto response = protoFields(steamCall("BeginAuthSessionViaCredentials", begin));
        const auto id = findField(response, 1);
        clientId = id ? id->number : 0;
        if (const auto value = findField(response, 2)) requestId = value->bytes;
        if (const auto value = findField(response, 5)) steamId = value->number;
        for (const auto &field : response) {
            if (field.id != 4) continue;
            const auto confirmation = protoFields(field.bytes);
            const auto type = findField(confirmation, 1);
            if (!type || type->number <= 1) continue;
            guard = static_cast<int>(type->number);
            break;
        }
    }
    if (!clientId || requestId.empty()) throw Failure("Steam sign-in failed. Try signing in again.", 1013);
    if (guard == 2 || guard == 3) {
        pipe.send(2, {guard == 3 ? "Enter your Steam Guard code." : "Enter the code from Steam's email."});
        auto code = pipe.receive(Clock::now() + std::chrono::seconds(300));
        if (code.kind != 4 || code.fields.size() != 1 || code.fields[0].size() != 5 ||
            !std::all_of(code.fields[0].begin(), code.fields[0].end(),
                [](unsigned char c) { return std::isalnum(c) != 0; }))
            throw Failure("The Steam sign-in request is not ready.", 1010);
        std::vector<unsigned char> update;
        protoUint(update, 1, clientId);
        protoFixed64(update, 2, steamId);
        protoString(update, 3, code.fields[0]);
        protoUint(update, 4, static_cast<std::uint64_t>(guard));
        steamCall("UpdateAuthSessionWithSteamGuardCode", update);
        wipe(code.fields[0].data(), code.fields[0].size());
    } else if (guard == 4)
        pipe.send(6, {"Approve sign-in in your Steam mobile app."});
    const auto deadline = Clock::now() + std::chrono::seconds(300);
    while (Clock::now() < deadline) {
        std::vector<unsigned char> query;
        protoUint(query, 1, clientId);
        protoBytes(query, 2, requestId);
        const auto response = protoFields(steamCall("PollAuthSessionStatus", query));
        if (const auto url = findField(response, 2); url && !url->bytes.empty()) showQr(pipe, fieldText(url));
        if (const auto id = findField(response, 1); id && id->number) clientId = id->number;
        auto token = fieldText(findField(response, 3));
        if (!token.empty()) {
            Login login;
            login.name = fieldText(findField(response, 6));
            if (login.name.empty() && request.kind == 2) login.name = request.fields[0];
            login.token.bytes.assign(token.begin(), token.end());
            wipe(token.data(), token.size());
            return login;
        }
#ifdef _WIN32
        Sleep(1000);
#else
        pollfd unused{};
        poll(&unused, 0, 1000);
#endif
    }
    throw Failure("Steam did not respond in time. Check your connection and try again.", 1010);
}

bool validSteamId(std::uint64_t id) {
    const auto account = static_cast<std::uint32_t>(id);
    return account != 0 && ((id >> 56) & 0xFF) == 1 && ((id >> 52) & 0xF) == 1;
}
bool clientAudience(const nlohmann::json &aud) {
    if (aud.is_string()) return aud.get<std::string>() == "client";
    if (!aud.is_array()) return false;
    for (const auto &item : aud) {
        if (!item.is_string()) continue;
        if (item.get<std::string>() == "client") return true;
    }
    return false;
}
std::uint64_t steamIdFromToken(std::string_view token) {
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || second <= first + 1)
        throw Failure("Steam sign-in failed. Sign in again.", 1010);
    auto payload = base64UrlDecode(token.substr(first + 1, second - first - 1));
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(payload.begin(), payload.end());
    } catch (const nlohmann::json::exception &) {
        wipe(payload.data(), payload.size());
        throw Failure("Steam sign-in failed. Sign in again.", 1010);
    }
    wipe(payload.data(), payload.size());
    if (!clientAudience(json.value("aud", nlohmann::json{})))
        throw Failure("Steam did not issue a client sign-in. Try again.", 1010);
    const auto sub = json.value("sub", std::string{});
    std::uint64_t id{};
    const auto parsed = std::from_chars(sub.data(), sub.data() + sub.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != sub.data() + sub.size() || !validSteamId(id))
        throw Failure("Steam sign-in failed. Sign in again.", 1010);
    return id;
}
void writeCString(std::vector<unsigned char> &out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}
std::string sha1Hex(std::string_view text) {
    std::array<unsigned char, 20> hash{};
    sha1(std::span(reinterpret_cast<const unsigned char *>(text.data()), text.size()), hash);
    return hexLower(hash);
}
std::vector<unsigned char> machineId(std::string_view account) {
    if (account.empty()) account = "steam";
    std::vector<unsigned char> out{0};
    writeCString(out, "MessageObject");
    for (const auto *label : {"BB3", "FF2", "3B3"}) {
        out.push_back(1);
        writeCString(out, label);
        writeCString(out, sha1Hex(std::string(account) + label));
    }
    out.push_back(8);
    out.push_back(8);
    return out;
}
void logOn(SteamLink &link, std::string_view name, std::string_view token) {
    link.steamId = steamIdFromToken(token);
    std::vector<unsigned char> body;
    protoUint(body, 1, 65580);
    protoString(body, 6, "english");
    protoUint(body, 7, clientOs());
    protoUint(body, 8, 1);
    protoUint(body, 21, 2);
    const auto id = machineId(name);
    protoBytes(body, 30, id);
    protoUint(body, 102, 1);
    protoString(body, 108, token);
    const auto job = link.nextJob++;
    sendProto(link, 5514, body, job);
    auto response = waitSteam(link, 751, 0, 30);
    const auto fields = protoFields(response.body);
    const auto result = findField(fields, 1);
    if (!result || result->number != 1)
        throw Failure("Steam sign-in failed (" + std::to_string(result ? result->number : 2) + "). Sign in again.", 1010);
    if (!validSteamId(link.steamId)) throw Failure("Steam returned an invalid account session.", 1010);
}
std::string authTicket(SteamLink &link) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (link.tokens.empty() && Clock::now() < deadline) pollSteam(link, Clock::now() + std::chrono::milliseconds(200));
    if (link.tokens.empty()) throw Failure("Steam did not respond in time. Check your connection and try again.", 1010);
    Secret connectToken = std::move(link.tokens.front());
    link.tokens.erase(link.tokens.begin());
    std::vector<unsigned char> ownership;
    protoUint(ownership, 1, 1284210);
    const auto job = link.nextJob++;
    sendProto(link, 857, ownership, job);
    auto owned = waitSteam(link, 858, job, 30);
    const auto fields = protoFields(owned.body);
    const auto result = findField(fields, 1);
    if (!result || result->number != 1) throw Failure("Add the free Guild Wars 2 game to this Steam account's library, then launch again.", 1011);
    const auto appTicket = findField(fields, 3);
    if (!appTicket || appTicket->bytes.empty())
        throw Failure("Add the free Guild Wars 2 game to this Steam account's library, then launch again.", 1011);
    static std::uint32_t sequence = 0;
    std::array<unsigned char, 8> entropy{};
    randomBytes(entropy);
    std::vector<unsigned char> session;
    appendU32(session, 24);
    appendU32(session, 1);
    appendU32(session, 2);
    session.insert(session.end(), entropy.begin(), entropy.end());
    appendU32(session, static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    appendU32(session, ++sequence);
    std::vector<unsigned char> auth;
    appendU32(auth, static_cast<std::uint32_t>(connectToken.bytes.size()));
    auth.insert(auth.end(), connectToken.bytes.begin(), connectToken.bytes.end());
    auth.insert(auth.end(), session.begin(), session.end());
    const auto crc = crc32(auth);
    std::vector<unsigned char> ticketMessage;
    protoFixed64(ticketMessage, 4, 1284210);
    protoUint(ticketMessage, 6, crc);
    protoBytes(ticketMessage, 7, auth);
    std::vector<unsigned char> list;
    protoUint(list, 1, static_cast<std::uint64_t>(link.tokens.size()));
    protoBytes(list, 4, ticketMessage);
    protoUint(list, 5, 1284210);
    sendProto(link, 5432, list, link.nextJob++);
    const auto ackDeadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < ackDeadline) {
        auto message = pollSteam(link, Clock::now() + std::chrono::milliseconds(200));
        if (message && message->emsg == 5575) break;
    }
    appendU32(auth, static_cast<std::uint32_t>(appTicket->bytes.size()));
    auth.insert(auth.end(), appTicket->bytes.begin(), appTicket->bytes.end());
    if (auth.empty() || auth.size() > 299)
        throw Failure("Steam returned a ticket larger than GW2's native login accepts.", 1012);
    auto hex = hexLower(auth);
    wipe(auth.data(), auth.size());
    return hex;
}

void connectSteam(SteamLink &link) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
#endif
    auto reply = https(
#ifdef _WIN32
        L"/ISteamDirectory/GetCMListForConnect/v1/?format=json&cellid=0"
#else
        "/ISteamDirectory/GetCMListForConnect/v1/?format=json&cellid=0"
#endif
        ,
        {}, false);
    if (reply.status != 200) throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
    const auto json = nlohmann::json::parse(reply.body.begin(), reply.body.end());
    int tried = 0;
    for (const auto &server : json.at("response").at("serverlist")) {
        if (server.value("type", "") != "netfilter") continue;
        if (connectEndpoint(link, server.value("endpoint", ""))) return;
        if (++tried == 4) break;
    }
    throw Failure("Could not reach Steam. Check your connection and try again.", 1010);
}

bool redirected(int argc) {
    if (argc != 1) return false;
#ifdef _WIN32
    const auto input = GetFileType(GetStdHandle(STD_INPUT_HANDLE));
    const auto output = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    return (input == FILE_TYPE_PIPE || input == FILE_TYPE_DISK) &&
        (output == FILE_TYPE_PIPE || output == FILE_TYPE_DISK);
#else
    return !isatty(STDIN_FILENO) && !isatty(STDOUT_FILENO);
#endif
}

PipeMessage nextCommand(Pipe &pipe, SteamLink &link, std::optional<Clock::time_point> deadline) {
    while (!deadline || Clock::now() < *deadline) {
        pollSteam(link, Clock::now() + std::chrono::milliseconds(50));
        if (!pipe.available()) continue;
        const auto limit = Clock::now() + std::chrono::seconds(5);
        return pipe.receive(deadline && *deadline < limit ? *deadline : limit);
    }
    throw Failure("Steam did not respond in time. Check your connection and try again.", 1010);
}

void runSession(Pipe &pipe) {
    auto request = pipe.receive(Clock::now() + std::chrono::seconds(15));
    if ((request.kind == 1 && !request.fields.empty()) || (request.kind == 2 && request.fields.size() != 2) ||
        (request.kind == 3 && request.fields.size() != 3) || request.kind < 1 || request.kind > 3)
        throw Failure("The Steam sign-in request is not ready.", 1010);
    Login login;
    std::string expected;
    if (request.kind == 3) {
        expected = request.fields[0];
        login.name = request.fields[1];
        login.token.bytes.assign(request.fields[2].begin(), request.fields[2].end());
        std::uint64_t id{};
        const auto parsed = std::from_chars(expected.data(), expected.data() + expected.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != expected.data() + expected.size() || !validSteamId(id) ||
            login.name.size() < 1 || login.name.size() > 64 || login.token.bytes.size() < 1 ||
            login.token.bytes.size() > 8192)
            throw Failure("The Steam sign-in request is not ready.", 1010);
    } else {
        if (request.kind == 2 &&
            (request.fields[0].size() < 1 || request.fields[0].size() > 64 || request.fields[1].size() < 1 ||
                request.fields[1].size() > 1024))
            throw Failure("The Steam sign-in request is not ready.", 1010);
        login = signIn(pipe, request);
        if (request.kind == 2) wipe(request.fields[1].data(), request.fields[1].size());
    }
    SteamLink link;
    connectSteam(link);
    const std::string token(login.token.bytes.begin(), login.token.bytes.end());
    logOn(link, login.name, token);
    const auto identity = std::to_string(link.steamId);
    if (!expected.empty() && identity != expected)
        throw Failure("Steam signed in to a different account. Reconnect this saved account.", 1014);
    pipe.send(3, {identity, login.name, token});
    wipe(const_cast<char *>(token.data()), token.size());
    login.token.clear();
    bool issued = false;
    for (;;) {
        const auto deadline = issued ? std::nullopt : std::optional{Clock::now() + std::chrono::seconds(600)};
        auto command = nextCommand(pipe, link, deadline);
        if (command.kind != 5 || !command.fields.empty())
            throw Failure("The Steam sign-in request is not ready.", 1010);
        auto ticket = authTicket(link);
        pipe.send(4, {ticket});
        wipe(ticket.data(), ticket.size());
        issued = true;
    }
}
int steamHelper(int argc) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (!redirected(argc)) return 2;
    Pipe pipe;
    try {
        runSession(pipe);
        return 0;
    } catch (const Failure &failure) {
        try {
            pipe.send(5, {failure.what(), std::to_string(failure.code)});
        } catch (...) {}
        return 1;
    } catch (const std::exception &) {
        try {
            pipe.send(5, {"The Steam session could not continue. Check your connection and sign in again.", "1010"});
        } catch (...) {}
        return 1;
    }
}
} // namespace detail

extern "C" int main(int argc, char **) {
    return detail::steamHelper(argc);
}
