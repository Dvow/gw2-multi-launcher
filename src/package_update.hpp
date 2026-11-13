#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifndef _WIN32
#include <SDL3/SDL.h>
#include <memory>
#endif

namespace gw2::package {
enum class Format { deb, arch };

inline Format format(const std::filesystem::path &directory) {
    std::ifstream input(directory / "package-format");
    std::string value, extra;
    if (input >> value && !(input >> extra)) {
        if (value == "DEB") return Format::deb;
        if (value == "ARCH") return Format::arch;
    }
    throw std::runtime_error("Install a launcher package to enable updates.");
}

inline std::string_view channel(Format format) {
    return format == Format::deb ? "linux-deb/" : "linux-arch/";
}

inline std::vector<std::string> installCommand(Format format, const std::filesystem::path &file) {
    const auto name = file.filename().string();
    if (!file.is_absolute() || (format == Format::deb ? !name.ends_with(".deb") : !name.ends_with(".pkg.tar.zst")))
        throw std::runtime_error("The update package does not match this installation.");
    if (format == Format::arch)
        return {"/usr/bin/pkexec", "--disable-internal-agent", "/usr/bin/pacman", "-U", "--noconfirm", "--", file.string()};
    return {"/usr/bin/pkexec", "--disable-internal-agent", "/usr/bin/apt-get", "install", "--yes", "--", file.string()};
}

#ifndef _WIN32
inline void install(const std::vector<std::string> &args, const std::filesystem::path &log) {
    std::vector<const char *> pointers;
    for (const auto &arg : args) pointers.push_back(arg.c_str());
    pointers.push_back(nullptr);
    auto output = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>(
        SDL_IOFromFile(log.c_str(), "w"), SDL_CloseIO);
    if (!output) throw std::runtime_error("Cannot create the installation log.");
    const auto properties = SDL_CreateProperties();
    if (!properties) throw std::runtime_error("Could not start the package installer.");
    const bool configured = SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, pointers.data()) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT) &&
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, output.get()) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    auto process = std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)>(
        configured ? SDL_CreateProcessWithProperties(properties) : nullptr, SDL_DestroyProcess);
    SDL_DestroyProperties(properties);
    output.reset();
    if (!process) throw std::runtime_error("Could not start the package installer. Check that pkexec is installed.");
    // Once authorization starts, let the package manager finish even if the app closes.
    int result{};
    if (!SDL_WaitProcess(process.get(), true, &result))
        throw std::runtime_error("Cannot read the installation result. See " + log.string());
    if (result == 0) return;
    if (result == 126) throw std::runtime_error("Installation cancelled.");
    if (result == 127)
        throw std::runtime_error("Authorization failed. Check your desktop's polkit agent. See " + log.string());
    throw std::runtime_error("Package installation failed (" + std::to_string(result) + "). See " + log.string());
}
#endif
}
