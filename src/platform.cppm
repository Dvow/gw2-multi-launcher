module;
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libsecret/secret.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <curl/curl.h>
#endif

export module platform;
import game;

export namespace gw2 {
inline void wipe(void *memory, std::size_t size) noexcept {
#ifdef _WIN32
    SecureZeroMemory(memory, size);
#else
    OPENSSL_cleanse(memory, size);
#endif
}
struct Secret {
    Bytes bytes;
    explicit Secret(std::size_t size = 0) : bytes(size) {}
    ~Secret() { clear(); }
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
    void clear() noexcept {
        wipe(bytes.data(), bytes.size());
        bytes.clear();
    }
    std::string_view text() const { return {reinterpret_cast<const char *>(bytes.data()), bytes.size()}; }
};
inline void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Cancelled.");
}
inline std::filesystem::path path(std::string_view value) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
inline std::string utf8(const std::filesystem::path &value) {
    const auto text = value.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}
inline Secret utf16(std::string_view text) {
    if (text.find('\0') != text.npos) throw std::runtime_error("Text contains a null character.");
    Secret terminated(text.size() + 1);
    std::memcpy(terminated.bytes.data(), text.data(), text.size());
    auto converted = SDL_iconv_string("UTF-16LE", "UTF-8",
        reinterpret_cast<const char *>(terminated.bytes.data()), terminated.bytes.size());
    if (!converted) throw std::runtime_error("Text is not valid UTF-8.");
    std::size_t length{};
    while (converted[length] || converted[length + 1])
        length += 2;
    Secret result(length + 2);
    std::memcpy(result.bytes.data(), converted, length + 2);
    wipe(converted, length + 2);
    SDL_free(converted);
    return result;
}
inline std::string fromUtf16(std::span<const unsigned char> text) {
    auto value =
        SDL_iconv_string("UTF-8", "UTF-16LE", reinterpret_cast<const char *>(text.data()), text.size());
    if (!value) throw std::runtime_error("The account contains invalid text.");
    std::string result(value);
    wipe(value, std::strlen(value));
    SDL_free(value);
    return result;
}
inline void random(std::span<unsigned char> bytes) {
#ifdef _WIN32
    if (BCryptGenRandom(
            nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
#else
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
#endif
        throw std::runtime_error("Secure random generation failed.");
}
inline std::string identifier() {
    std::array<unsigned char, 16> bytes{};
    random(bytes);
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 15];
    }
    return result;
}
inline std::string base64(std::span<const unsigned char> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        auto n = static_cast<unsigned>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= static_cast<unsigned>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) n |= bytes[i + 2];
        result += alphabet[n >> 18];
        result += alphabet[(n >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(n >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
    }
    return result;
}
inline Secret unbase64(std::string_view text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (text.empty() || text.size() % 4 || text.size() > 16384)
        throw std::runtime_error("Invalid encrypted account.");
    Secret result;
    result.bytes.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        unsigned n{}, padding{};
        for (unsigned j = 0; j < 4; ++j) {
            n <<= 6;
            const auto c = text[i + j];
            if (c == '=' && i + 4 == text.size() && j >= 2) {
                ++padding;
                continue;
            }
            const auto found = alphabet.find(c);
            if (padding || found == alphabet.npos) throw std::runtime_error("Invalid encrypted account.");
            n |= static_cast<unsigned>(found);
        }
        result.bytes.push_back(static_cast<unsigned char>(n >> 16));
        if (padding < 2) result.bytes.push_back(static_cast<unsigned char>(n >> 8));
        if (!padding) result.bytes.push_back(static_cast<unsigned char>(n));
    }
    return result;
}
inline std::filesystem::path dataRoot() {
#ifdef _WIN32
    PWSTR directory{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &directory)))
        throw std::runtime_error("Cannot find the application data directory.");
    std::filesystem::path root(directory);
    CoTaskMemFree(directory);
#else
    const auto xdg = SDL_getenv("XDG_DATA_HOME"), home = SDL_getenv("HOME");
    auto root = xdg && *xdg && path(xdg).is_absolute() ? path(xdg)
        : home                                         ? path(home) / ".local/share"
                                                       : std::filesystem::path{};
    if (root.empty()) throw std::runtime_error("Cannot find your home directory.");
#endif
    root /= "GW2 Multi Launcher";
    std::filesystem::create_directories(root);
#ifndef _WIN32
    if (chmod(root.c_str(), 0700)) throw std::runtime_error("Cannot protect the account directory.");
#endif
    return root;
}
class FileLock {
#ifdef _WIN32
    HANDLE value_{INVALID_HANDLE_VALUE};
#else
    int value_{-1};
#endif
  public:
    explicit FileLock(const std::filesystem::path &file) {
#ifdef _WIN32
        value_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (value_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error(
                "GW2 Multi Launcher is already open, or the account directory is not writable.");
#else
        value_ = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (value_ < 0) throw std::runtime_error("Cannot open the account directory lock.");
        if (flock(value_, LOCK_EX | LOCK_NB)) {
            close(value_);
            value_ = -1;
            throw std::runtime_error("GW2 Multi Launcher is already open.");
        }
#endif
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    ~FileLock() {
#ifdef _WIN32
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
#else
        if (value_ >= 0) close(value_);
#endif
    }
};
inline void atomicWrite(const std::filesystem::path &file, std::string_view contents) {
    auto temporary = file;
    temporary += "." + identifier() + ".tmp";
    try {
#ifdef _WIN32
        auto handle = CreateFileW(
            temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot save the account catalog.");
        DWORD written{};
        const bool ok =
            WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
            written == contents.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        if (!ok ||
            !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot save the account catalog.");
#else
        int fd = open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0) throw std::runtime_error("Cannot save the account catalog.");
        std::size_t offset{};
        bool ok = true;
        while (offset < contents.size()) {
            auto n = write(fd, contents.data() + offset, contents.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ok = false;
                break;
            }
            offset += static_cast<std::size_t>(n);
        }
        ok = fsync(fd) == 0 && ok;
        close(fd);
        if (!ok || rename(temporary.c_str(), file.c_str()))
            throw std::runtime_error("Cannot save the account catalog.");
        int dir = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir >= 0) {
            fsync(dir);
            close(dir);
        }
#endif
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}

#ifndef _WIN32
namespace detail {
struct KeyringReply {
    bool done{}, stored{};
    char *value{};
    GError *error{};
    ~KeyringReply() {
        if (value) secret_password_free(value);
        if (error) g_error_free(error);
    }
};
Secret keyring(bool create, std::stop_token stop) {
    static const SecretSchema schema = {"io.github.Dvow.GW2MultiLauncher", SECRET_SCHEMA_NONE,
        {{"application", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"vault", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    auto context = g_main_context_new();
    g_main_context_push_thread_default(context);
    auto cancel = g_cancellable_new();
    struct Cleanup {
        GMainContext *context;
        GCancellable *cancel;
        ~Cleanup() {
            g_object_unref(cancel);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
        }
    } cleanup{context, cancel};
    std::stop_callback cancellation(stop, [cancel] { g_cancellable_cancel(cancel); });
    auto wait = [&](KeyringReply &reply) {
        const auto deadline = SDL_GetTicks() + 60000;
        while (!reply.done) {
            while (g_main_context_iteration(context, false)) {
            }
            if (SDL_GetTicks() >= deadline) g_cancellable_cancel(cancel);
            if (!reply.done) SDL_Delay(5);
        }
        cancelled(stop);
        if (reply.error) throw std::runtime_error("Unlock the desktop keyring and try again.");
    };
    KeyringReply reply;
    secret_password_lookup(
        &schema, cancel,
        +[](GObject *, GAsyncResult *result, gpointer data) {
            auto &r = *static_cast<KeyringReply *>(data);
            r.value = secret_password_lookup_finish(result, &r.error);
            r.done = true;
        },
        &reply, "application", "gw2-multi-launcher", "vault", "v1", nullptr);
    wait(reply);
    if (reply.value) {
        auto key = unbase64(reply.value);
        if (key.bytes.size() != 32)
            throw std::runtime_error("The desktop keyring contains an invalid launcher key.");
        return key;
    }
    if (!create)
        throw std::runtime_error("The launcher key is missing from your desktop keyring. Restore the keyring "
                                 "before opening these accounts.");
    Secret key(32);
    random(key.bytes);
    auto encoded = base64(key.bytes);
    struct Clear {
        std::string &text;
        ~Clear() { wipe(text.data(), text.size()); }
    } clear{encoded};
    KeyringReply saved;
    secret_password_store(
        &schema, SECRET_COLLECTION_DEFAULT, "GW2 Multi Launcher", encoded.c_str(), cancel,
        +[](GObject *, GAsyncResult *result, gpointer data) {
            auto &r = *static_cast<KeyringReply *>(data);
            r.stored = secret_password_store_finish(result, &r.error);
            r.done = true;
        },
        &saved, "application", "gw2-multi-launcher", "vault", "v1", nullptr);
    wait(saved);
    if (!saved.stored) throw std::runtime_error("The desktop keyring could not save the launcher key.");
    return key;
}
} // namespace detail
#endif
Secret crypt(std::span<const unsigned char> input, bool encrypt, bool create, std::stop_token stop) {
    cancelled(stop);
#ifdef _WIN32
    (void)create;
    DATA_BLOB source{static_cast<DWORD>(input.size()), const_cast<BYTE *>(input.data())}, output{};
    bool ok = encrypt
        ? CryptProtectData(&source, L"GW2 Multi Launcher account", nullptr, nullptr, nullptr,
              CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        throw std::runtime_error("Windows could not open this account. Use the Windows user who saved it.");
    struct Cleanup {
        DATA_BLOB &data;
        ~Cleanup() {
            wipe(data.pbData, data.cbData);
            LocalFree(data.pbData);
        }
    } cleanup{output};
    Secret result(output.cbData);
    std::memcpy(result.bytes.data(), output.pbData, output.cbData);
    return result;
#else
    if (!encrypt && (input.size() < 32 || std::memcmp(input.data(), "GML1", 4)))
        throw std::runtime_error(
            "This encrypted account format is unsupported. Add the account again on this system.");
    auto key = detail::keyring(encrypt && create, stop);
    Secret result(encrypt ? input.size() + 32 : input.size() - 32);
    auto context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context) throw std::runtime_error("Cannot initialize account encryption.");
    auto check = [](int value) {
        if (value != 1)
            throw std::runtime_error(
                "The account could not be decrypted or saved. Check your desktop keyring.");
    };
    int length{}, finalLength{};
    if (encrypt) {
        std::memcpy(result.bytes.data(), "GML1", 4);
        random(std::span(result.bytes).subspan(4, 12));
        check(EVP_EncryptInit_ex(
            context.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(), result.bytes.data() + 4));
        check(EVP_EncryptUpdate(context.get(), nullptr, &length, result.bytes.data(), 4));
        check(EVP_EncryptUpdate(
            context.get(), result.bytes.data() + 32, &length, input.data(), static_cast<int>(input.size())));
        check(EVP_EncryptFinal_ex(context.get(), result.bytes.data() + 32 + length, &finalLength));
        check(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, 16, result.bytes.data() + 16));
    } else {
        check(EVP_DecryptInit_ex(
            context.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(), input.data() + 4));
        check(EVP_DecryptUpdate(context.get(), nullptr, &length, input.data(), 4));
        check(EVP_DecryptUpdate(context.get(), result.bytes.data(), &length, input.data() + 32,
            static_cast<int>(input.size() - 32)));
        check(EVP_CIPHER_CTX_ctrl(
            context.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char *>(input.data()) + 16));
        check(EVP_DecryptFinal_ex(context.get(), result.bytes.data() + length, &finalLength));
    }
    return result;
#endif
}

// One worker owns each process and its nonblocking pipes. Destroying this wrapper
// detaches; it must never terminate a player's game or a patcher writing Gw2.dat.
class Process {
    SDL_Process *process_{};
#ifndef _WIN32
    // Proton may discard stdin. A 0600 FIFO in a private 0700 directory keeps
    // credentials in kernel memory without changing the runtime's isolation.
    struct Input {
        int fd{-1};
        std::string folder, file;
        ~Input() {
            if (fd >= 0) ::close(fd);
            if (!file.empty()) ::unlink(file.c_str());
            if (!folder.empty()) ::rmdir(folder.c_str());
        }
        void open(std::vector<std::string> &args) {
            folder = utf8(dataRoot() / "pipe-XXXXXX");
            if (!::mkdtemp(folder.data())) {
                folder.clear();
                throw std::runtime_error("Cannot create a private helper pipe.");
            }
            file = folder + "/input";
            if (::mkfifo(file.c_str(), 0600) != 0 ||
                (fd = ::open(file.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK)) < 0)
                throw std::runtime_error("Cannot open the private helper pipe.");
            args.insert(args.end(), {"--input", file});
        }
    } input_;
#endif

  public:
    Process(std::vector<std::string> args,
        const std::vector<std::pair<std::string, std::string>> &environment, bool privateInput = false) {
#ifndef _WIN32
        if (privateInput) input_.open(args);
#else
        (void)privateInput;
#endif
        auto props = SDL_CreateProperties();
        auto env = SDL_CreateEnvironment(true);
        struct Cleanup {
            SDL_PropertiesID props;
            SDL_Environment *env;
            ~Cleanup() {
                SDL_DestroyProperties(props);
                SDL_DestroyEnvironment(env);
            }
        } cleanup{props, env};
        if (!props || !env) throw std::runtime_error("Cannot prepare the game helper.");
        std::vector<const char *> pointers;
        for (const auto &arg : args)
            pointers.push_back(arg.c_str());
        pointers.push_back(nullptr);
        for (const auto &[name, value] : environment)
            if (!SDL_SetEnvironmentVariable(env, name.c_str(), value.c_str(), true))
                throw std::runtime_error("Cannot configure Wine/Proton.");
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, pointers.data());
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
            privateInput ? SDL_PROCESS_STDIO_NULL : SDL_PROCESS_STDIO_APP);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        process_ = SDL_CreateProcessWithProperties(props);
        if (!process_)
            throw std::runtime_error(
                "The game helper could not start. Check your installation and Wine/Proton runner.");
    }
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;
    ~Process() {
        if (process_) SDL_DestroyProcess(process_);
    }
    void allowForeground() const {
#ifdef _WIN32
        // A Show click grants focus only to its helper. Steam forwards the same
        // permission to its game helper before forwarding the control message.
        const auto pid =
            SDL_GetNumberProperty(SDL_GetProcessProperties(process_), SDL_PROP_PROCESS_PID_NUMBER, 0);
        if (pid > 0 && pid < MAXDWORD) AllowSetForegroundWindow(static_cast<DWORD>(pid));
#endif
    }
    std::size_t write(std::span<const unsigned char> bytes) {
#ifndef _WIN32
        if (input_.fd >= 0) {
            const auto count = ::write(input_.fd, bytes.data(), bytes.size());
            if (count >= 0) return static_cast<std::size_t>(count);
            if (errno == EAGAIN || errno == EINTR) return 0;
            throw std::runtime_error("The game helper stopped reading its private pipe.");
        }
#endif
        const auto stream = SDL_GetProcessInput(process_);
        if (!stream) throw std::runtime_error("The game helper input closed.");
        const auto count = SDL_WriteIO(stream, bytes.data(), bytes.size());
        if (count == 0 && SDL_GetIOStatus(stream) != SDL_IO_STATUS_NOT_READY)
            throw std::runtime_error("The game helper stopped reading its input.");
        return count;
    }
    std::size_t read(std::span<unsigned char> bytes) {
        const auto stream = SDL_GetProcessOutput(process_);
        if (!stream) throw std::runtime_error("The game helper output closed.");
#ifdef _WIN32
        // SDL 3.4's read-ahead can consume bytes then report zero when the next
        // nonblocking read is empty. Read this pipe directly so frames never lose bytes.
        const auto handle = SDL_GetPointerProperty(
            SDL_GetIOProperties(stream), SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, nullptr);
        if (!handle) throw std::runtime_error("The game helper output pipe is unavailable.");
        DWORD count{};
        const auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), MAXDWORD));
        const bool ready = ReadFile(handle, bytes.data(), size, &count, nullptr) != FALSE;
        if (!ready && GetLastError() == ERROR_NO_DATA) return 0;
        const bool closed = !ready || !count;
#else
        const auto count = SDL_ReadIO(stream, bytes.data(), bytes.size());
        const bool closed = !count && SDL_GetIOStatus(stream) != SDL_IO_STATUS_NOT_READY;
#endif
        if (closed) {
            int code{};
            if (SDL_WaitProcess(process_, false, &code))
                throw std::runtime_error("The game helper exited (code " + std::to_string(code) +
                    "). Check that your runner can access the installed helper.");
            throw std::runtime_error("The game helper closed. Check your Wine/Proton runner.");
        }
        return count;
    }
    void write(Secret &pending, std::size_t &written) {
        if (pending.bytes.empty()) return;
        written += write(std::span(pending.bytes).subspan(written));
        if (written != pending.bytes.size()) return;
        pending.clear();
        written = 0;
    }
};

struct HttpReply {
    unsigned status{};
    Secret body;
};
HttpReply https(std::string_view host, std::string_view resource, std::string_view authorization,
    std::string_view body, std::stop_token stop, std::size_t limit = 32768) {
    cancelled(stop);
    HttpReply reply;
    auto &response = reply.body.bytes;
    response.reserve(32768);
    const bool download = host == "github.com" && authorization.empty() && body.empty();
#ifdef _WIN32
    struct Internet {
        HINTERNET value{};
        ~Internet() {
            if (value) WinHttpCloseHandle(value);
        }
    };
    Internet session{WinHttpOpen(L"GW2MultiLauncher/" GW2_APP_VERSION,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (!session.value) throw std::runtime_error("Could not open a secure connection.");
    WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 5000);
    auto server = utf16(host), route = utf16(resource);
    std::string headerText;
    headerText.reserve(authorization.size() + 80);
    headerText = "Content-Type: application/x-www-form-urlencoded\r\n";
    if (!authorization.empty()) {
        headerText += "Authorization: ";
        headerText += authorization;
        headerText += "\r\n";
    }
    auto headers = utf16(headerText);
    wipe(headerText.data(), headerText.size());
    Internet connection{WinHttpConnect(session.value, reinterpret_cast<const wchar_t *>(server.bytes.data()),
        INTERNET_DEFAULT_HTTPS_PORT, 0)};
    Internet request{connection.value ? WinHttpOpenRequest(connection.value, body.empty() ? L"GET" : L"POST",
                                            reinterpret_cast<const wchar_t *>(route.bytes.data()), nullptr,
                                            nullptr, nullptr, WINHTTP_FLAG_SECURE)
                                      : nullptr};
    DWORD policy = download ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
                            : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (request.value)
        WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!request.value ||
        !WinHttpSendRequest(request.value, reinterpret_cast<const wchar_t *>(headers.bytes.data()),
            static_cast<DWORD>(headers.bytes.size() / 2 - 1), const_cast<char *>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        throw std::runtime_error("Could not reach the service. Check your connection and try again.");
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
            &status, &size, nullptr))
        throw std::runtime_error("Invalid service response.");
    reply.status = status;
    const auto deadline = SDL_GetTicks() + (download ? 600000 : 10000);
    Secret buffer(16384);
    for (;;) {
        cancelled(stop);
        DWORD count{};
        if (SDL_GetTicks() > deadline ||
            !WinHttpReadData(
                request.value, buffer.bytes.data(), static_cast<DWORD>(buffer.bytes.size()), &count))
            throw std::runtime_error("The service request timed out.");
        if (!count) break;
        if (count > limit - response.size()) throw std::runtime_error("The service response is too large.");
        response.insert(response.end(), buffer.bytes.begin(), buffer.bytes.begin() + count);
    }
#else
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Could not open a secure connection.");
    const auto url = "https://" + std::string(host) + std::string(resource);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    struct Headers {
        curl_slist *value{};
        ~Headers() {
            for (auto p = value; p; p = p->next)
                wipe(p->data, std::strlen(p->data));
            curl_slist_free_all(value);
        }
    } headers;
    headers.value = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
    std::string auth;
    auth.reserve(authorization.size() + 15);
    auth = "Authorization: ";
    auth += authorization;
    headers.value = curl_slist_append(headers.value, auth.c_str());
    wipe(auth.data(), auth.size());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.value);
    if (!body.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "GW2MultiLauncher/" GW2_APP_VERSION);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, download ? 600L : 15L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, download ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    struct Output {
        Bytes &bytes;
        std::size_t limit;
    } output{response, limit};
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char *data, std::size_t size, std::size_t count, void *context) -> std::size_t {
            const auto length = size * count;
            auto &out = *static_cast<Output *>(context);
            if (length > out.limit - out.bytes.size()) return 0;
            try {
                out.bytes.insert(out.bytes.end(), data, data + length);
                return length;
            } catch (...) {
                return 0;
            }
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<std::stop_token *>(context)->stop_requested() ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &stop);
    if (curl_easy_perform(curl.get()) != CURLE_OK)
        throw std::runtime_error("Could not reach the service. Check your connection and try again.");
    long status{};
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    reply.status = static_cast<unsigned>(status);
#endif
    cancelled(stop);
    return reply;
}
std::string sha256(std::span<const unsigned char> bytes) {
    std::array<unsigned char, 32> digest{};
#ifdef _WIN32
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<unsigned char *>(bytes.data()),
            static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) != 0)
#else
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), nullptr, EVP_sha256(), nullptr) != 1)
#endif
        throw std::runtime_error("Could not verify the update download.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
std::string latestBuild(std::stop_token stop) {
    auto reply = https("api.guildwars2.com", "/v2/build", {}, {}, stop);
    if (reply.status != 200)
        throw std::runtime_error("The GW2 update service is unavailable. Try again shortly.");
    return std::string(reply.body.text());
}
} // namespace gw2
