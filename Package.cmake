if(NOT GW2_BUILD_PACKAGES)
  return()
endif()

if(NOT WIN32 AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "Installation prefix" FORCE)
endif()
include(GNUInstallDirs)

function(gw2_fetch_innosetup destination)
  set(installer "${CMAKE_BINARY_DIR}/innosetup-7.1.0-x64.exe")
  message(STATUS "Downloading Inno Setup 7.1.0")
  file(DOWNLOAD
    "https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe"
    "${installer}"
    EXPECTED_HASH SHA256=0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f
    TLS_VERIFY ON
    STATUS download_status
    TIMEOUT 120)
  list(GET download_status 0 download_code)
  if(NOT download_code EQUAL 0)
    list(GET download_status 1 download_message)
    message(FATAL_ERROR "Inno Setup download failed: ${download_message}")
  endif()
  execute_process(
    COMMAND "${installer}"
      /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /PORTABLE=1
      "/DIR=${destination}"
    RESULT_VARIABLE setup_code
    OUTPUT_VARIABLE setup_output
    ERROR_VARIABLE setup_error)
  if(NOT setup_code EQUAL 0 OR NOT EXISTS "${destination}/ISCC.exe")
    message(FATAL_ERROR "Inno Setup 7.1.0 setup failed (${setup_code}): ${setup_output}${setup_error}")
  endif()
endfunction()

if(NOT GW2_SETUP_BASENAME)
  set(GW2_SETUP_BASENAME "gw2-multi-launcher_${PROJECT_VERSION}_win-x64_setup")
endif()
if(NOT GW2_DEB_FILE_NAME)
  set(GW2_DEB_FILE_NAME "gw2-multi-launcher_${PROJECT_VERSION}_amd64")
endif()
set(setup_dependencies gw2-multi-launcher ${native_targets} ${steam_setup_depends}
  proxy/kx_proxy.py proxy/kx_proxy_pipe.py proxy/kx_proxy_linux.py proxy/kx_proxy_tun.py
  src/icons/gw2-multi-launcher.png ${GW2_SETUP_DEPENDS})

if(WIN32)
  if(NOT GW2_SETUP_VERSION_DESCRIPTION)
    set(GW2_SETUP_VERSION_DESCRIPTION "${GW2_PRODUCT_NAME}")
  endif()
  file(REMOVE "${CMAKE_BINARY_DIR}/ThirdPartyNotices.txt")
  install(TARGETS gw2-multi-launcher ${native_targets} RUNTIME DESTINATION . LIBRARY DESTINATION .)
  install(PROGRAMS "${GW2_STEAM_EXECUTABLE}" DESTINATION .)
  install(FILES proxy/kx_proxy.py proxy/kx_proxy_pipe.py DESTINATION .)
  set(innosetup_dir "${CMAKE_SOURCE_DIR}/build/tools/Inno Setup 7")
  # The setup script needs Inno Setup 7 (ApplyPathRedirRules for the C++ runtime
  # check). GitHub's Windows runners ship Inno Setup 6, which must not be picked up;
  # without a 7 compiler the pinned 7.1.0 is fetched.
  function(gw2_iscc_is_7 result candidate)
    execute_process(COMMAND "${candidate}" /? OUTPUT_VARIABLE banner ERROR_VARIABLE banner
      RESULT_VARIABLE ignored TIMEOUT 30)
    if(NOT banner MATCHES "Inno Setup 7")
      set(${result} FALSE PARENT_SCOPE)
    endif()
  endfunction()
  if(DEFINED CACHE{GW2_ISCC})
    set(cached_7 TRUE)
    if(EXISTS "${GW2_ISCC}")
      gw2_iscc_is_7(cached_7 "${GW2_ISCC}")
    endif()
    if(NOT EXISTS "${GW2_ISCC}" OR NOT cached_7)
      unset(GW2_ISCC CACHE)
    endif()
  endif()
  find_program(GW2_ISCC NAMES ISCC.exe ISCC VALIDATOR gw2_iscc_is_7 HINTS
    "${innosetup_dir}"
    "$ENV{LOCALAPPDATA}/GW2MultiLauncher-BuildTools/Inno Setup 7"
    "$ENV{ProgramFiles}/Inno Setup 7"
    "$ENV{ProgramFiles\(x86\)}/Inno Setup 7")
  if(NOT GW2_ISCC)
    gw2_fetch_innosetup("${innosetup_dir}")
    set(GW2_ISCC "${innosetup_dir}/ISCC.exe" CACHE FILEPATH "Path to ISCC.exe" FORCE)
    set(GW2_ISCC "${innosetup_dir}/ISCC.exe")
  endif()
  set(setup [=[
[Setup]
AppId=@GW2_APP_ID@
AppName=@GW2_PRODUCT_NAME@
AppVersion=@PROJECT_VERSION@
VersionInfoDescription=@GW2_SETUP_VERSION_DESCRIPTION@
AppPublisher=GW2 Multi Launcher contributors
DefaultDirName={localappdata}\Programs\@GW2_PRODUCT_NAME@
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousTasks=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19045
WizardStyle=modern
SetupIconFile=@CMAKE_BINARY_DIR@/icon.ico
CloseApplications=no
RestartApplications=no
UninstallDisplayName=@GW2_PRODUCT_NAME@
UninstallDisplayIcon={app}\@GW2_PROGRAM_FILE@.exe
OutputDir=@CMAKE_BINARY_DIR@
OutputBaseFilename=@GW2_SETUP_BASENAME@
Compression=lzma2/max
SolidCompression=yes

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Check: not IsLauncherUpdate

[Files]
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.Host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.Native.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "@GW2_STEAM_EXECUTABLE@"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/kx_proxy.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/kx_proxy_pipe.py"; DestDir: "{app}"; Flags: ignoreversion
@GW2_SETUP_FILES@

[InstallDelete]
Type: files; Name: "{app}\gw2-multi-launcher.bmp"
Type: files; Name: "{app}\gw2-multi-launcher.png"
Type: files; Name: "{app}\GW2MultiLauncher.Host.exe"
Type: files; Name: "{app}\GW2MultiLauncher.Native.dll"
Type: files; Name: "{app}\steam\@GW2_PROGRAM_FILE@.Steam.exe"
Type: files; Name: "{app}\steam\GW2MultiLauncher.Steam.exe"
Type: files; Name: "{app}\steam\GW2MultiLauncher.Steam.dll"
Type: dirifempty; Name: "{app}\steam"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.exe"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.dat"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.exe.removing"

[UninstallDelete]
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.exe"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.dat"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.exe.removing"

[Icons]
Name: "{userprograms}\@GW2_PRODUCT_NAME@"; Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; WorkingDir: "{app}"; AppUserModelID: "@GW2_APP_ID@"
Name: "{userdesktop}\@GW2_PRODUCT_NAME@"; Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; WorkingDir: "{app}"; AppUserModelID: "@GW2_APP_ID@"; Tasks: desktopicon; Check: not IsLauncherUpdate

[Run]
Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; Description: "Open @GW2_PRODUCT_NAME@"; Flags: nowait postinstall skipifsilent; Check: not IsLauncherUpdate
Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; Flags: nowait; Check: IsLauncherUpdate

[Code]
@GW2_SETUP_CODE@
function OpenProcess(Access: LongWord; Inherit: Integer; ProcessId: LongWord): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Integer;
  external 'CloseHandle@kernel32.dll stdcall';
function MoveFileEx(ExistingFile, NewFile: String; Flags: Cardinal): Integer;
  external 'MoveFileExW@kernel32.dll stdcall';
function GetTickCount: Cardinal;
  external 'GetTickCount@kernel32.dll stdcall';

var
  ParkedCount: Integer;
  ParkedFrom: array[0..15] of String;
  ParkedTo: array[0..15] of String;

procedure UndoParks;
var
  Index: Integer;
begin
  { A failed rename must restore earlier files. The copy step has not run. }
  for Index := ParkedCount - 1 downto 0 do
    MoveFileEx(ParkedTo[Index], ParkedFrom[Index], 0);
  ParkedCount := 0;
end;

function BackupName(Path: String): String;
var
  Index: Integer;
  Candidate: String;
begin
  Result := '';
  Candidate := Path + '.old';
  if not FileExists(Candidate) then
  begin
    Result := Candidate;
    Exit;
  end;
  Index := 0;
  while Index < 1000 do
  begin
    Candidate := Path + '.old' + IntToStr(GetTickCount) + IntToStr(Index);
    if not FileExists(Candidate) then
    begin
      Result := Candidate;
      Exit;
    end;
    Index := Index + 1;
  end;
end;

procedure ParkInstalledFile(Name: String);
var
  Path, Backup: String;
begin
  Path := ExpandConstant(Name);
  if not FileExists(Path) then Exit;
  if ParkedCount > 15 then
  begin
    UndoParks;
    RaiseException('Could not prepare the update.');
  end;
  Backup := BackupName(Path);
  if (Backup = '') or (MoveFileEx(Path, Backup, 0) = 0) then
  begin
    UndoParks;
    RaiseException('Could not replace ' + ExtractFileName(Path) + '. Close that game and run the update again.');
  end;
  ParkedFrom[ParkedCount] := Path;
  ParkedTo[ParkedCount] := Backup;
  ParkedCount := ParkedCount + 1;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    @GW2_SETUP_POSTINSTALL@
    Exit;
  end;
  if CurStep <> ssInstall then Exit;
  ParkedCount := 0;
  ParkInstalledFile('{app}\@GW2_PROGRAM_FILE@.exe');
  ParkInstalledFile('{app}\GW2MultiLauncher.exe');
  ParkInstalledFile('{app}\@GW2_PROGRAM_FILE@.Host.exe');
  ParkInstalledFile('{app}\GW2MultiLauncher.Host.exe');
  ParkInstalledFile('{app}\@GW2_PROGRAM_FILE@.Native.dll');
  ParkInstalledFile('{app}\GW2MultiLauncher.Native.dll');
  ParkInstalledFile('{app}\@GW2_PROGRAM_FILE@.Steam.exe');
  ParkInstalledFile('{app}\steam\@GW2_PROGRAM_FILE@.Steam.exe');
  ParkInstalledFile('{app}\steam\GW2MultiLauncher.Steam.exe');
  ParkInstalledFile('{app}\steam\GW2MultiLauncher.Steam.dll');
end;

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
    MsgBox('Close @GW2_PRODUCT_NAME@, then run the update again.', mbError, MB_OK);
end;
]=])
  string(CONFIGURE "${setup}" setup @ONLY)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/setup.iss" CONTENT "${setup}")
  set(setup_file "${CMAKE_BINARY_DIR}/${GW2_SETUP_BASENAME}.exe")
  add_custom_command(OUTPUT "${setup_file}"
    COMMAND "${GW2_ISCC}" /Qp "${CMAKE_BINARY_DIR}/setup.iss"
    DEPENDS ${setup_dependencies} "${CMAKE_BINARY_DIR}/setup.iss" "${CMAKE_BINARY_DIR}/icon.ico"
    COMMENT "Building Windows installer" VERBATIM)
else()
  if(NOT GW2_LINUX_PACKAGE_NAME)
    set(GW2_LINUX_PACKAGE_NAME gw2-multi-launcher)
  endif()
  if(NOT GW2_PACKAGE_README)
    set(GW2_PACKAGE_README "${CMAKE_SOURCE_DIR}/docs/README.md")
  endif()
  # Application runtime files use the same path in Debian and Arch packages.
  set(linux_install_dir "lib/${GW2_LINUX_PACKAGE_NAME}")
  if(EXISTS /etc/arch-release)
    set(default_package_format ARCH)
  else()
    set(default_package_format DEB)
  endif()
  set(GW2_LINUX_PACKAGE_FORMAT "${default_package_format}" CACHE STRING "Linux package format: DEB or ARCH")
  set_property(CACHE GW2_LINUX_PACKAGE_FORMAT PROPERTY STRINGS DEB ARCH)
  if(NOT GW2_LINUX_PACKAGE_FORMAT MATCHES "^(DEB|ARCH)$")
    message(FATAL_ERROR "GW2_LINUX_PACKAGE_FORMAT must be DEB or ARCH")
  endif()
  set(linux_command "${CMAKE_BINARY_DIR}/${GW2_LINUX_PACKAGE_NAME}")
  set(linux_desktop "${linux_command}.desktop")
  set(notices "${CMAKE_BINARY_DIR}/ThirdPartyNotices.txt")
  file(WRITE "${notices}" "GW2 Multi Launcher third-party notices\n\n")
  foreach(license "${SDL3_SOURCE_DIR}/LICENSE.txt" "${imgui_SOURCE_DIR}/LICENSE.txt" "${json_SOURCE_DIR}/LICENSE.MIT" "${minhook_SOURCE_DIR}/LICENSE.txt")
    file(READ "${license}" text)
    file(APPEND "${notices}" "${text}\n\n")
  endforeach()
  file(READ "${imgui_SOURCE_DIR}/LICENSE.txt" mit)
  string(REGEX REPLACE "Copyright \\(c\\) 2014-2026 Omar Cornut"
    "ProggyClean: Copyright (c) 2004, 2005 Tristan Grimmer\nProggyForever: Copyright (c) 2026 Disco Hello; Copyright (c) 2019, 2023 Tristan Grimmer" font_mit "${mit}")
  file(APPEND "${notices}" "Embedded ImGui fonts\n${font_mit}\n\nRoboto-Medium.ttf: Copyright 2011 Google Inc. All Rights Reserved.\n")
  file(READ "${json_SOURCE_DIR}/LICENSES/Apache-2.0.txt" apache)
  file(APPEND "${notices}" "${apache}\n")
  file(READ "${zlib_SOURCE_DIR}/LICENSE" zlib_license)
  file(APPEND "${notices}" "\nzlib\n${zlib_license}\n")
  file(READ "${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.hpp" qr_license)
  string(FIND "${qr_license}" "*/" qr_end)
  string(SUBSTRING "${qr_license}" 0 "${qr_end}" qr_notice)
  file(APPEND "${notices}" "\nQR Code generator\n${qr_notice}*/\n")
  if(GW2_STATIC_LIBCXX_NOTICES)
    file(READ "${GW2_STATIC_LIBCXX_NOTICES}" libcxx_notice)
    file(APPEND "${notices}" "\nStatically linked LLVM libc++ / libc++abi\n${libcxx_notice}\n${apache}\n")
  endif()
  install(TARGETS gw2-multi-launcher RUNTIME DESTINATION ${linux_install_dir})
  file(WRITE "${CMAKE_BINARY_DIR}/package-format" "${GW2_LINUX_PACKAGE_FORMAT}\n")
  install(FILES "${CMAKE_BINARY_DIR}/package-format" DESTINATION ${linux_install_dir})
  install(FILES proxy/kx_proxy.py proxy/kx_proxy_linux.py proxy/kx_proxy_tun.py DESTINATION ${linux_install_dir})
  install(PROGRAMS "${GW2_STEAM_EXECUTABLE}" DESTINATION ${linux_install_dir})
  install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.png" DESTINATION ${linux_install_dir})
  install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Host.exe" DESTINATION ${linux_install_dir})
  install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Native.dll" DESTINATION ${linux_install_dir})
  if(IS_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/net")
    install(DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/net/"
      DESTINATION "${linux_install_dir}/net"
      FILES_MATCHING PATTERN "*.dll")
  endif()
  file(WRITE "${linux_command}" "#!/bin/sh\nexec \"${CMAKE_INSTALL_PREFIX}/${linux_install_dir}/${GW2_PROGRAM_FILE}\" \"$@\"\n")
  install(PROGRAMS "${linux_command}" DESTINATION ${CMAKE_INSTALL_BINDIR})
  file(WRITE "${linux_desktop}"
    "[Desktop Entry]\nType=Application\nName=${GW2_PRODUCT_NAME}\nComment=Launch your Anet accounts\nExec=${GW2_LINUX_PACKAGE_NAME}\nIcon=${GW2_LINUX_PACKAGE_NAME}\nTerminal=false\nCategories=Game;\nStartupWMClass=${GW2_PROGRAM_FILE}\n")
  install(FILES "${linux_desktop}" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.png"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps RENAME "${GW2_LINUX_PACKAGE_NAME}.png")
  install(FILES "${GW2_PACKAGE_README}" LICENSE "${notices}" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${GW2_LINUX_PACKAGE_NAME})
  if(GW2_LINUX_PACKAGE_FORMAT STREQUAL "DEB")
    find_program(DPKG_SHLIBDEPS_EXECUTABLE dpkg-shlibdeps REQUIRED)
    set(CPACK_GENERATOR DEB)
  else()
    find_program(MAKEPKG_EXECUTABLE makepkg REQUIRED)
    set(CPACK_GENERATOR TGZ)
  endif()
  set(CPACK_PACKAGE_NAME "${GW2_LINUX_PACKAGE_NAME}")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_CONTACT "GW2 Multi Launcher contributors")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Compact native Guild Wars 2 account launcher")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
  set(CPACK_PACKAGE_FILE_NAME "${GW2_DEB_FILE_NAME}")
  set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
  set(CPACK_DEBIAN_PACKAGE_SECTION games)
  set(CPACK_DEBIAN_PACKAGE_RELEASE 4)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  if(NOT DEFINED GW2_PACKAGE_UPDATES)
    set(GW2_PACKAGE_UPDATES ON)
  endif()
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsecret-1-0, libgl1, libx11-6, libxcursor1, libxrandr2, libxi6, libxfixes3, libxss1, libxtst6")
  if(GW2_PACKAGE_UPDATES)
    string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", pkexec | policykit-1")
  endif()
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gnome-keyring, xwayland, xdg-desktop-portal, zenity")
  set(CPACK_DEBIAN_PACKAGE_SUGGESTS "umu-launcher | wine, mitmproxy (>= 12.2), bubblewrap, iproute2, python3")
  set(CPACK_STRIP_FILES ON)
  include(CPack)
  set(linux_package_dependencies ${setup_dependencies}
    "${CMAKE_BINARY_DIR}/CPackConfig.cmake" "${CMAKE_BINARY_DIR}/cmake_install.cmake"
    "${linux_command}" "${linux_desktop}" "${GW2_PACKAGE_README}" "${notices}" "${CMAKE_BINARY_DIR}/package-format"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Host.exe"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Native.dll"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.png")
  set(arch_payload_name "${GW2_LINUX_PACKAGE_NAME}-${PROJECT_VERSION}-linux-x86_64")
  set(arch_payload "${CMAKE_BINARY_DIR}/${arch_payload_name}.tar.gz")
  set(arch_extra_dependencies "")
  if(GW2_PACKAGE_UPDATES)
    set(arch_extra_dependencies "'polkit'")
  endif()
  configure_file(cmake/ArchPKGBUILD.in "${CMAKE_BINARY_DIR}/PKGBUILD.in" @ONLY)
  add_custom_command(OUTPUT "${arch_payload}" "${CMAKE_BINARY_DIR}/PKGBUILD"
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
      -G TGZ -D "CPACK_PACKAGE_FILE_NAME=${arch_payload_name}" -D CPACK_INCLUDE_TOPLEVEL_DIRECTORY=OFF
    COMMAND "${CMAKE_COMMAND}" "-DPAYLOAD=${arch_payload}"
      "-DTEMPLATE=${CMAKE_BINARY_DIR}/PKGBUILD.in" "-DOUTPUT=${CMAKE_BINARY_DIR}/PKGBUILD"
      -P "${CMAKE_SOURCE_DIR}/cmake/WriteArchRecipe.cmake"
    DEPENDS ${linux_package_dependencies} "${CMAKE_BINARY_DIR}/PKGBUILD.in" cmake/WriteArchRecipe.cmake
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Preparing Arch package files" VERBATIM)
  add_custom_target(arch-payload DEPENDS "${arch_payload}" "${CMAKE_BINARY_DIR}/PKGBUILD")
  if(GW2_LINUX_PACKAGE_FORMAT STREQUAL "ARCH")
    set(setup_file "${CMAKE_BINARY_DIR}/${GW2_LINUX_PACKAGE_NAME}-${PROJECT_VERSION}-1-x86_64.pkg.tar.zst")
    add_custom_command(OUTPUT "${setup_file}"
      COMMAND "${CMAKE_COMMAND}" -E env PKGEXT=.pkg.tar.zst "${MAKEPKG_EXECUTABLE}" --force --clean
      DEPENDS "${arch_payload}" "${CMAKE_BINARY_DIR}/PKGBUILD"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Building Arch package (run as a regular user)" VERBATIM)
  else()
    set(setup_file "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}.deb")
    add_custom_command(OUTPUT "${setup_file}"
      COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake" -C "${CMAKE_BUILD_TYPE}"
      DEPENDS ${linux_package_dependencies}
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Building Debian package" VERBATIM)
  endif()
endif()

add_custom_target(setup ALL DEPENDS "${setup_file}")
