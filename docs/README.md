# GW2 Multi Launcher

Several Guild Wars 2 accounts. One copy of the game.

Launch one account, or all of them. Each account opens in its own window. Closing the launcher leaves the game open.

<p align="center">
  <img src="account-list.png" width="280" alt="Account list">
</p>

Add an account with ArenaNet, Steam, or Epic. You sign in once, then just launch.

<p align="center">
  <img src="arenanet.png" width="220" alt="Add an ArenaNet account">
  <img src="steam.png" width="220" alt="Sign in to Steam">
  <img src="epic.png" width="220" alt="Sign in to Epic">
</p>

In Settings, choose your `Gw2-64.exe`. The game updates before your accounts open. If it needs to download, close Guild Wars 2 first.

Use the **DLLs** folder button in Settings or an account's editor to choose DLLs
to load at startup. Global DLLs load first, then account DLLs; duplicate files load
once. Click a tag to replace its file or its cross to remove it. The row stays on
one line. Changes apply on the next launch.

<p align="center">
  <img src="settings.png" width="280" alt="Settings">
</p>

## Install

- **Windows 10/11:** download the setup from [Releases](https://github.com/Dvow/gw2-multi-launcher/releases).
- **Ubuntu 26.04:** install the `.deb` from that page with `sudo apt install ./gw2-multi-launcher_*_amd64.deb`. Use Wine or Proton, then select it in Settings.

Not an official ArenaNet app. Two-factor still works like a normal login.

<details>
<summary>Build</summary>

CMake 3.30+, Ninja, and the .NET 10 SDK. Windows also needs MSVC x64 and Inno Setup 6 or 7.

```sh
cmake --preset release
cmake --build --preset release
```

The installer is written to `build/release`. On Windows, run that from an x64 Visual Studio developer shell. Set `GW2_ISCC` if Inno Setup is not on `PATH`.

On Ubuntu, install `g++ cmake ninja-build pkg-config dpkg-dev libsecret-1-dev libssl-dev libcurl4-openssl-dev libgl-dev libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev`, then configure with `-DCMAKE_INSTALL_PREFIX=/usr -DGW2_NATIVE_DIR=/path/to/matching/windows/bin`.

MIT licensed.

</details>
