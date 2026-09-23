# GW2 Multi Launcher

A small Windows and Linux launcher for multiple GW2 accounts. One shared game installation, direct native sign-in, and one-click Launch or Launch all. Built with C++20 modules, SDL3, and Dear ImGui's docking branch.

Choose your installed `Gw2-64.exe` in Settings, then add your accounts. Settings and existing-account edits save automatically when you finish a field or leave the editor; toggles save immediately. Edit expands an account in place. New accounts still need Add to finish creating them. Click account rows to select or deselect them, then choose Launch selected. With nothing selected, Launch all opens every available account. Running accounts have Show to bring the game forward and Close to exit it normally. Global arguments apply to everyone; an account's matching arguments override them. Closing this app leaves your games running.

Actions use compact icons with tooltips after 300 ms. Buttons stay on one line at every supported window width. Close all exits every running account, regardless of selection. It becomes available while games are running once any current launch or update finishes.

The window starts narrow and remembers its size, position, and UI scale automatically. Moving between monitors keeps the same text and control sizes. Opening settings does not reset your chosen size.

Enable **Show PID** in Settings to display the game's process ID after its status, such as `Running · 18204` (Wine process ID on Linux). It is off by default and clears when the session ends.

The launcher checks ArenaNet's current build before launching and runs the official updater when needed. Launch all and Launch selected finish updating the shared installation before opening accounts one at a time. Close all clients when a download is required, then launch again. Interrupted downloads are resumed before sign-in; a failed update stops the launch queue. Native integration is discovered and checked against the executable; an incompatible game change stops login with an explanation.

Launching an account automatically accepts GW2's current user agreement when required.

The app checks GitHub for a newer stable launcher release once at startup, in the background. Turn off **Check automatically** under Launcher updates in Settings to disable this. **Check for updates** works manually; it becomes **Update now** when an update is available. Close your games before installing. Downloads are verified against GitHub's SHA-256 digest, then the launcher installs and reopens. Ubuntu asks for administrator authentication. A Windows copy run from a build folder installs into the normal per-user app location.

Passwords and saved Steam/Epic sessions are protected with Windows DPAPI or your Linux desktop keyring. They never go in command-line arguments. Account data stays in your user data directory under `KX/GW2MultiLauncher`, outside the installation. Existing saved accounts and settings are imported automatically on first launch.

## Install

- **Windows 10/11 x64:** run the setup executable. No administrator access required.
- **Ubuntu 26.04 x64:** install the `.deb` with `sudo apt install ./gw2-multi-launcher_0.3.0_amd64.deb`. Use a desktop with OpenGL, X11/XWayland, and an unlocked Secret Service keyring. Install GW2 through Wine or Proton first, then select that runner and prefix in Settings. Proton uses `umu-run` and your installed Proton directory. For other distributions, build from source.

Choose ArenaNet for email/password accounts, including when using game files installed through Steam or Epic. Choose Steam for mobile QR sign-in or password and Steam Guard. Choose Epic to sign in and approve Guild Wars 2 in your browser; the account appears in the launcher automatically. Name the account and choose Add account. Epic uses the selected game installation and requests only basic profile access. Saved accounts launch independently without repeating browser sign-in.

Steam and Epic launches have been verified on Windows. Linux platform logins are awaiting validation. This is an unofficial launcher; it does not bypass two-factor authentication. Linux runs the interface natively and the Windows game helper inside the selected compatibility layer.

## Build

Requires CMake 3.30+, Ninja, .NET 10 SDK for the Steam helper, and MSVC x64 with Inno Setup 6/7 (Windows) or GCC 15+ (Linux). CMake fetches pinned dependencies into the build directory.

```sh
cmake --preset debug
cmake --build --preset debug
```

Configure once before building: `--build` does not create a missing build directory. `cmake --workflow --preset debug` configures and builds in one command. Use `release` instead of `debug` for optimized builds.

Both `cmake --build --preset debug` and `cmake --build --preset release` build the complete app and its installer automatically. Executables go in `build/<preset>/bin`; the Windows setup `.exe` or Linux `.deb` goes directly in `build/<preset>`. Unchanged builds skip repackaging.

To publish a release, increase `project(... VERSION ...)` in `CMakeLists.txt` and push to `main`. GitHub Actions builds both installers, then publishes `v<version>` with generated release notes. Existing published versions are never replaced. Pull requests build the packages without publishing. Installers are available on the [Releases page](https://github.com/Dvow/gw2-multi-launcher/releases).

On Windows, run these commands in an x64 Visual Studio developer shell. Install Inno Setup 6/7 or set `GW2_ISCC` to its `ISCC.exe` when configuring.

On Ubuntu, install `g++ cmake ninja-build pkg-config dpkg-dev libsecret-1-dev libssl-dev libcurl4-openssl-dev libgl-dev libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev`. Configure with `-DCMAKE_INSTALL_PREFIX=/usr -DGW2_NATIVE_DIR=/path/to/matching/windows/bin` to include the Windows helper and DLL built from the same source. The normal build then creates the Debian package for that Ubuntu version.

MIT licensed. Dependency notices are included with installed packages.

Source layout: `src` contains the shared UI, account storage, launch coordination, and platform support; `native` contains the Windows game integration; `auth` contains the separate SteamKit helper; `cmake` contains installer rules. Formatting follows `.clang-format`.
