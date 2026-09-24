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

set(setup_dependencies gw2-multi-launcher ${native_targets} ${steam_setup_depends}
  src/icons/gw2-multi-launcher.png)

if(WIN32)
  file(REMOVE "${CMAKE_BINARY_DIR}/ThirdPartyNotices.txt")
  install(TARGETS gw2-multi-launcher ${native_targets} RUNTIME DESTINATION . LIBRARY DESTINATION .)
  install(PROGRAMS "${GW2_STEAM_EXECUTABLE}" DESTINATION .)
  set(innosetup_dir "${CMAKE_SOURCE_DIR}/build/tools/Inno Setup 7")
  if(DEFINED CACHE{GW2_ISCC} AND NOT EXISTS "${GW2_ISCC}")
    unset(GW2_ISCC CACHE)
  endif()
  find_program(GW2_ISCC NAMES ISCC.exe ISCC HINTS
    "${innosetup_dir}"
    "$ENV{LOCALAPPDATA}/GW2MultiLauncher-BuildTools/Inno Setup 7"
    "$ENV{ProgramFiles}/Inno Setup 7"
    "$ENV{ProgramFiles}/Inno Setup 6"
    "$ENV{ProgramFiles\(x86\)}/Inno Setup 7"
    "$ENV{ProgramFiles\(x86\)}/Inno Setup 6")
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
AppPublisher=GW2 Multi Launcher contributors
DefaultDirName={localappdata}\Programs\@GW2_PRODUCT_NAME@
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
UninstallDisplayName=@GW2_PRODUCT_NAME@
UninstallDisplayIcon={app}\@GW2_PROGRAM_FILE@.exe
OutputDir=@CMAKE_BINARY_DIR@
OutputBaseFilename=gw2-multi-launcher_@PROJECT_VERSION@_win-x64_setup
Compression=lzma2/max
SolidCompression=yes

[Files]
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.Host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@/@GW2_PROGRAM_FILE@.Native.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "@GW2_STEAM_EXECUTABLE@"; DestDir: "{app}"; Flags: ignoreversion
Source: "@CMAKE_BINARY_DIR@/uninstall-stub.exe"; Flags: dontcopy

[InstallDelete]
Type: files; Name: "{app}\gw2-multi-launcher.bmp"
Type: files; Name: "{app}\gw2-multi-launcher.png"
Type: files; Name: "{app}\GW2MultiLauncher.Host.exe"
Type: files; Name: "{app}\GW2MultiLauncher.Native.dll"
Type: filesandordirs; Name: "{app}\steam"
Type: files; Name: "{app}\README.md"
Type: files; Name: "{app}\LICENSE"
Type: files; Name: "{app}\ThirdPartyNotices.txt"

[UninstallDelete]
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.exe"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.dat"
Type: files; Name: "{app}\Uninstall @GW2_PRODUCT_NAME@.exe.removing"

[Icons]
Name: "{userprograms}\@GW2_PRODUCT_NAME@"; Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; Description: "Open @GW2_PRODUCT_NAME@"; Flags: nowait postinstall skipifsilent; Check: not IsLauncherUpdate
Filename: "{app}\@GW2_PROGRAM_FILE@.exe"; Flags: nowait; Check: IsLauncherUpdate

[Code]
function OpenProcess(Access: LongWord; Inherit: Integer; ProcessId: LongWord): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Integer;
  external 'CloseHandle@kernel32.dll stdcall';
function MoveFileEx(ExistingFile, NewFile: String; Flags: Cardinal): Integer;
  external 'MoveFileExW@kernel32.dll stdcall';

procedure ParkInstalledFile(Name: String);
var
  Path, Backup: String;
  Index: Integer;
begin
  Path := ExpandConstant(Name);
  if not FileExists(Path) then Exit;
  if DeleteFile(Path) then Exit;
  Index := 0;
  while Index < 20 do
  begin
    if Index = 0 then Backup := Path + '.old' else Backup := Path + '.old' + IntToStr(Index);
    if not FileExists(Backup) then Break;
    if DeleteFile(Backup) then Break;
    Index := Index + 1;
  end;
  if Index < 20 then MoveFileEx(Path, Backup, 0);
end;

procedure AppendUninstall(Dest: TFileStream; Path: String; var Size: Int64);
var
  Source: TFileStream;
begin
  Source := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
  try
    Size := Source.Size;
    if Size > 0 then Dest.CopyFrom(Source, Size, 65536);
  finally
    Source.Free;
  end;
end;

procedure WriteInt64(Dest: TFileStream; Value: Int64);
var
  Packed: AnsiString;
  Index: Integer;
  Piece: Integer;
  Rest: Int64;
begin
  SetLength(Packed, 8);
  Rest := Value;
  for Index := 1 to 8 do
  begin
    Piece := Integer(Rest) and 255;
    Packed[Index] := Chr(Piece);
    Rest := Rest div 256;
  end;
  Dest.WriteBuffer(Packed, 8);
end;

procedure WriteUninstallFooter(Dest: TFileStream; ExeSize, DatSize: Int64);
var
  Magic: AnsiString;
begin
  SetLength(Magic, 8);
  Magic[1] := 'K';
  Magic[2] := 'X';
  Magic[3] := 'U';
  Magic[4] := 'N';
  Magic[5] := 'I';
  Magic[6] := 'N';
  Magic[7] := 'S';
  Magic[8] := 'T';
  Dest.WriteBuffer(Magic, 8);
  WriteInt64(Dest, ExeSize);
  WriteInt64(Dest, DatSize);
end;

function WritePackedUninstaller(StubPath, ExePath, DatPath, TargetPath: String): Boolean;
var
  Dest: TFileStream;
  ExeSize, DatSize: Int64;
begin
  Result := False;
  Dest := TFileStream.Create(TargetPath, fmCreate);
  try
    AppendUninstall(Dest, StubPath, ExeSize);
    if ExeSize <= 0 then Exit;
    AppendUninstall(Dest, ExePath, ExeSize);
    if ExeSize <= 0 then Exit;
    AppendUninstall(Dest, DatPath, DatSize);
    if DatSize <= 0 then Exit;
    WriteUninstallFooter(Dest, ExeSize, DatSize);
    Result := True;
  finally
    Dest.Free;
  end;
end;

procedure PackUninstaller;
var
  StubPath, ExePath, DatPath, TargetPath, OldDat: String;
begin
  StubPath := ExpandConstant('{tmp}\uninstall-stub.exe');
  ExePath := ExpandConstant('{app}\unins000.exe');
  DatPath := ExpandConstant('{app}\unins000.dat');
  TargetPath := ExpandConstant('{app}\Uninstall @GW2_PRODUCT_NAME@.exe');
  OldDat := ExpandConstant('{app}\Uninstall @GW2_PRODUCT_NAME@.dat');
  if not FileExists(ExePath) then Exit;
  if not FileExists(DatPath) then Exit;
  try
    ExtractTemporaryFile('uninstall-stub.exe');
  except
    Exit;
  end;
  if not FileExists(StubPath) then Exit;
  DeleteFile(TargetPath);
  if not WritePackedUninstaller(StubPath, ExePath, DatPath, TargetPath) then
  begin
    DeleteFile(TargetPath);
    Exit;
  end;
  if not DeleteFile(ExePath) then Exit;
  if not DeleteFile(DatPath) then Exit;
  DeleteFile(OldDat);
  RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\@GW2_APP_ID@_is1',
    'UninstallString', '"' + TargetPath + '"');
  RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\@GW2_APP_ID@_is1',
    'QuietUninstallString', '"' + TargetPath + '" /SILENT');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    PackUninstaller;
    Exit;
  end;
  if CurStep <> ssInstall then Exit;
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
  set(setup_file "${CMAKE_BINARY_DIR}/gw2-multi-launcher_${PROJECT_VERSION}_win-x64_setup.exe")
  add_custom_command(OUTPUT "${setup_file}"
    COMMAND "${GW2_ISCC}" /Qp "${CMAKE_BINARY_DIR}/setup.iss"
    DEPENDS ${setup_dependencies} uninstall-stub "${CMAKE_BINARY_DIR}/setup.iss" "${CMAKE_BINARY_DIR}/icon.ico"
    COMMENT "Building Windows installer" VERBATIM)
else()
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
  find_program(DPKG_SHLIBDEPS_EXECUTABLE dpkg-shlibdeps REQUIRED)
  install(TARGETS gw2-multi-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  install(PROGRAMS "${GW2_STEAM_EXECUTABLE}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.png" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Host.exe" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Native.dll" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher)
  file(WRITE "${CMAKE_BINARY_DIR}/gw2-multi-launcher" "#!/bin/sh\nexec \"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/gw2-multi-launcher/${GW2_PROGRAM_FILE}\" \"$@\"\n")
  install(PROGRAMS "${CMAKE_BINARY_DIR}/gw2-multi-launcher" DESTINATION ${CMAKE_INSTALL_BINDIR})
  file(WRITE "${CMAKE_BINARY_DIR}/gw2-multi-launcher.desktop"
    "[Desktop Entry]\nType=Application\nName=${GW2_PRODUCT_NAME}\nComment=Launch your Anet accounts\nExec=gw2-multi-launcher\nIcon=gw2-multi-launcher\nTerminal=false\nCategories=Game;\nStartupWMClass=${GW2_PROGRAM_FILE}\n")
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
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Host.exe"
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.Native.dll"
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.png"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Building Debian package" VERBATIM)
endif()

add_custom_target(setup ALL DEPENDS "${setup_file}")
