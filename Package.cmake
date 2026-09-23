if(NOT WIN32 AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "Installation prefix" FORCE)
endif()
include(GNUInstallDirs)

# Generate distribution metadata in the build tree, not the source checkout.
set(notices "${CMAKE_BINARY_DIR}/ThirdPartyNotices.txt")
file(WRITE "${notices}" "GW2 Multi Launcher third-party notices\n\n")
foreach(license "${sdl3_SOURCE_DIR}/LICENSE.txt" "${imgui_SOURCE_DIR}/LICENSE.txt" "${json_SOURCE_DIR}/LICENSE.MIT" "${minhook_SOURCE_DIR}/LICENSE.txt")
  file(READ "${license}" text)
  file(APPEND "${notices}" "${text}\n\n")
endforeach()
file(READ "${imgui_SOURCE_DIR}/LICENSE.txt" mit)
string(REGEX REPLACE "Copyright \\(c\\) 2014-2026 Omar Cornut"
  "ProggyClean: Copyright (c) 2004, 2005 Tristan Grimmer\nProggyForever: Copyright (c) 2026 Disco Hello; Copyright (c) 2019, 2023 Tristan Grimmer" font_mit "${mit}")
file(APPEND "${notices}" "Embedded ImGui fonts\n${font_mit}\n\nRoboto-Medium.ttf: Copyright 2011 Google Inc. All Rights Reserved.\n")
file(READ "${json_SOURCE_DIR}/LICENSES/Apache-2.0.txt" apache)
file(APPEND "${notices}" "${apache}\n")

function(include_notice name url hash)
  set(file "${CMAKE_BINARY_DIR}/licenses/${hash}.txt")
  file(DOWNLOAD "${url}" "${file}" EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON TIMEOUT 30)
  file(READ "${file}" text)
  file(APPEND "${notices}" "\n${name}\nSource: ${url}\n${text}\n")
endfunction()
include_notice("SteamKit2"
  "https://raw.githubusercontent.com/SteamRE/SteamKit/1c7bc9c41a529e8fbb1e6890f1e4dbcdc5200cb7/SteamKit2/SteamKit2/license.txt"
  d0c87e66d00e92e13cfa07158bf07e9be65e18aa5220d06af8532092521e78fa)
include_notice("SteamKit2 LGPL"
  "https://raw.githubusercontent.com/SteamRE/SteamKit/1c7bc9c41a529e8fbb1e6890f1e4dbcdc5200cb7/LICENSE"
  00a89b0d18aacd4114decf79122db87bf35bddaf2bc50e383c9c9f4c263390b2)
include_notice("QRCoder"
  "https://raw.githubusercontent.com/Shane32/QRCoder/443d5a1f76debf203b1e252efee6996a15d41f5c/LICENSE.txt"
  22e4c25e35c416b15f655a62378d964597c15c6c27fe2ff123444491eec0649b)
include_notice("protobuf-net"
  "https://raw.githubusercontent.com/protobuf-net/protobuf-net/dfdfce61a739cfd76f05fcdacf8a4b3b9e94e684/Licence.txt"
  2054377b5c04fb70c67fe91ca12417e9d413e533169f1270ef69bd815f3053f3)
include_notice("ZstdSharp"
  "https://raw.githubusercontent.com/oleg-st/ZstdSharp/0ee6121aaa173b42e68d3c6c8816a68e910e0557/LICENSE"
  9a6a7216e532c4ee4f78881d31fb641c27148eb01b2d0e8e3235ffe9a70a662c)
include_notice(".NET runtime"
  "https://raw.githubusercontent.com/dotnet/runtime/v10.0.11/LICENSE.TXT"
  cfc21f5e8bd655ae997eec916138b707b1d290b83272c02a95c9f821b8c87310)
include_notice(".NET third parties"
  "https://raw.githubusercontent.com/dotnet/runtime/v10.0.11/THIRD-PARTY-NOTICES.TXT"
  66f1d4e44973185519bb4aa8a9718eb22fc7af2cc532e3ae9cfc4c127ee7fc54)

set(setup_dependencies gw2-multi-launcher ${native_targets}
  "${steam_output}/GW2MultiLauncher.Steam.dll"
  docs/README.md LICENSE "${notices}" src/icons/gw2-multi-launcher.png)

if(WIN32)
  install(TARGETS gw2-multi-launcher ${native_targets} RUNTIME DESTINATION . LIBRARY DESTINATION .)
  install(DIRECTORY "${steam_output}/" DESTINATION steam USE_SOURCE_PERMISSIONS)
  install(FILES src/icons/gw2-multi-launcher.png DESTINATION .)
  install(FILES docs/README.md LICENSE "${notices}" DESTINATION .)
  find_program(GW2_ISCC NAMES ISCC.exe ISCC HINTS
    "$ENV{LOCALAPPDATA}/GW2MultiLauncher-BuildTools/Inno Setup 7"
    "$ENV{LOCALAPPDATA}/GW2Launcher-BuildTools/Inno Setup 7"
    "$ENV{ProgramFiles}/Inno Setup 7"
    "$ENV{ProgramFiles}/Inno Setup 6"
    "$ENV{ProgramFiles\(x86\)}/Inno Setup 7"
    "$ENV{ProgramFiles\(x86\)}/Inno Setup 6" REQUIRED)
  set(setup [=[
[Setup]
AppId=GW2MultiLauncher
AppName=GW2 Multi Launcher
AppVersion=@PROJECT_VERSION@
AppPublisher=GW2 Multi Launcher contributors
DefaultDirName={localappdata}\Programs\GW2 Multi Launcher
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19045
WizardStyle=modern
SetupIconFile=@CMAKE_BINARY_DIR@/icon.ico
CloseApplications=no
RestartApplications=no
UninstallDisplayName=GW2 Multi Launcher
UninstallDisplayIcon={app}\GW2MultiLauncher.exe
OutputDir=@CMAKE_BINARY_DIR@
OutputBaseFilename=gw2-multi-launcher_@PROJECT_VERSION@_win-x64_setup
Compression=lzma2/max
SolidCompression=yes
LicenseFile=@CMAKE_SOURCE_DIR@/LICENSE

[Files]
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/GW2MultiLauncher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/GW2MultiLauncher.Host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/GW2MultiLauncher.Native.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/gw2-multi-launcher.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "@steam_output@/*"; DestDir: "{app}/steam"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "@CMAKE_SOURCE_DIR@/docs/README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_SOURCE_DIR@/LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_BINARY_DIR@/ThirdPartyNotices.txt"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{app}\gw2-multi-launcher.bmp"
Type: files; Name: "{app}\Gw2Launcher.exe"
Type: files; Name: "{app}\Gw2Launcher.Host.exe"
Type: files; Name: "{app}\Gw2Launcher.Native.dll"
Type: files; Name: "{app}\steam\Gw2Launcher.Steam.*"

[Icons]
Name: "{userprograms}\GW2 Multi Launcher"; Filename: "{app}\GW2MultiLauncher.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\GW2MultiLauncher.exe"; Description: "Open GW2 Multi Launcher"; Flags: nowait postinstall skipifsilent; Check: not IsLauncherUpdate
Filename: "{app}\GW2MultiLauncher.exe"; Flags: nowait; Check: IsLauncherUpdate

[Code]
function OpenProcess(Access: LongWord; Inherit: Integer; ProcessId: LongWord): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Integer;
  external 'CloseHandle@kernel32.dll stdcall';

function IsLauncherUpdate: Boolean;
begin
  Result := ExpandConstant('{param:UPDATEPID|0}') <> '0';
end;

function InitializeSetup: Boolean;
var
  Handle: THandle;
  ProcessId: LongWord;
begin
  Result := True;
  ProcessId := StrToIntDef(ExpandConstant('{param:UPDATEPID|0}'), 0);
  if ProcessId = 0 then Exit;
  Handle := OpenProcess($100000, 0, ProcessId);
  if Handle = 0 then Exit;
  { Wait for the actual launcher exit so settings are flushed before replacing files. }
  Result := WaitForSingleObject(Handle, 30000) = 0;
  CloseHandle(Handle);
  if not Result then
    MsgBox('Close GW2 Multi Launcher, then run the update again.', mbError, MB_OK);
end;
]=])
  string(CONFIGURE "${setup}" setup @ONLY)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/setup.iss" CONTENT "${setup}")
  set(setup_file "${CMAKE_BINARY_DIR}/gw2-multi-launcher_${PROJECT_VERSION}_win-x64_setup.exe")
  add_custom_command(OUTPUT "${setup_file}"
    COMMAND "${GW2_ISCC}" /Qp "${CMAKE_BINARY_DIR}/setup.iss"
    DEPENDS ${setup_dependencies} "${CMAKE_BINARY_DIR}/setup.iss" "${CMAKE_BINARY_DIR}/icon.ico"
    COMMENT "Building Windows installer" VERBATIM)
else()
  find_program(DPKG_SHLIBDEPS_EXECUTABLE dpkg-shlibdeps REQUIRED)
  install(TARGETS gw2-multi-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  # Optional .NET LTTng tracing requires an obsolete library absent on current Ubuntu.
  install(DIRECTORY "${steam_output}/" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher/steam
    USE_SOURCE_PERMISSIONS PATTERN "libcoreclrtraceptprovider.so" EXCLUDE)
  install(FILES src/icons/gw2-multi-launcher.png DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GW2MultiLauncher.Host.exe" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GW2MultiLauncher.Native.dll" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  file(WRITE "${CMAKE_BINARY_DIR}/gw2-multi-launcher" "#!/bin/sh\nexec \"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher/GW2MultiLauncher\" \"$@\"\n")
  install(PROGRAMS "${CMAKE_BINARY_DIR}/gw2-multi-launcher" DESTINATION ${CMAKE_INSTALL_BINDIR})
  file(WRITE "${CMAKE_BINARY_DIR}/gw2-multi-launcher.desktop"
    "[Desktop Entry]\nType=Application\nName=GW2 Multi Launcher\nComment=Launch your ArenaNet accounts\nExec=gw2-multi-launcher\nIcon=gw2-multi-launcher\nTerminal=false\nCategories=Game;\nStartupWMClass=GW2MultiLauncher\n")
  install(FILES "${CMAKE_BINARY_DIR}/gw2-multi-launcher.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES src/icons/gw2-multi-launcher.png DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps)
  install(FILES docs/README.md LICENSE "${notices}" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/gw2-multi-launcher)
  set(CPACK_GENERATOR DEB)
  set(CPACK_PACKAGE_NAME gw2-multi-launcher)
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_CONTACT "GW2 Multi Launcher contributors")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Compact native Guild Wars 2 account launcher")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
  set(CPACK_PACKAGE_FILE_NAME "gw2-multi-launcher_${PROJECT_VERSION}_amd64")
  set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
  set(CPACK_DEBIAN_PACKAGE_SECTION games)
  set(CPACK_DEBIAN_PACKAGE_REPLACES "gw2-launcher")
  set(CPACK_DEBIAN_PACKAGE_CONFLICTS "gw2-launcher")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsecret-1-0, libgl1, libx11-6, libxcursor1, libxrandr2, libxi6, libxfixes3, libxss1, libxtst6, xwayland, pkexec")
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gnome-keyring")
  set(CPACK_STRIP_FILES ON)
  set(setup_file "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}.deb")
  include(CPack)
  add_custom_command(OUTPUT "${setup_file}"
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake" -C "${CMAKE_BUILD_TYPE}"
    DEPENDS ${setup_dependencies} "${CMAKE_BINARY_DIR}/CPackConfig.cmake" "${CMAKE_BINARY_DIR}/cmake_install.cmake"
      "${CMAKE_BINARY_DIR}/gw2-multi-launcher" "${CMAKE_BINARY_DIR}/gw2-multi-launcher.desktop"
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GW2MultiLauncher.Host.exe"
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GW2MultiLauncher.Native.dll" src/icons/gw2-multi-launcher.png
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Building Debian package" VERBATIM)
endif()

add_custom_target(setup ALL DEPENDS "${setup_file}")
