cmake_minimum_required(VERSION 3.30)
project(GW2MultiLauncher VERSION 0.6.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT CPM_SOURCE_CACHE)
  if(DEFINED ENV{CPM_SOURCE_CACHE} AND NOT "$ENV{CPM_SOURCE_CACHE}" STREQUAL "")
    set(CPM_SOURCE_CACHE "$ENV{CPM_SOURCE_CACHE}" CACHE PATH "Directory to download CPM dependencies")
  else()
    set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm" CACHE PATH "Directory to download CPM dependencies")
  endif()
endif()
include("${CMAKE_SOURCE_DIR}/cmake/CPM.cmake")

# Hashed archives live in the shared CPM cache. Keep the exact revisions and hashes.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
foreach(feature AUDIO CAMERA JOYSTICK HAPTIC SENSOR GPU RENDER WAYLAND HIDAPI POWER TRAY)
  set(SDL_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
CPMAddPackage(
  NAME SDL3
  VERSION 3.4.16
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.tar.gz
  URL_HASH SHA256=c2ee715e42ec520c4d11fd8d249ef1d2b2baf4ad31148b72b3b000276c0b3633
  EXCLUDE_FROM_ALL YES
  SYSTEM YES)
CPMAddPackage(
  NAME imgui
  URL https://github.com/ocornut/imgui/archive/3bae66c735670619baf51391eba7f3d90a25d125.tar.gz
  URL_HASH SHA256=0b99c7baed551f76058bc6fe83d806dc55c912280e9d2d268879a1c9751cef20
  DOWNLOAD_ONLY YES)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
CPMAddPackage(
  NAME json
  VERSION 3.12.0
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
  EXCLUDE_FROM_ALL YES
  SYSTEM YES)
# Linux packages also carry the license for their Windows game helper.
CPMAddPackage(
  NAME minhook
  VERSION 1.3.4
  URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
  URL_HASH SHA256=1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e
  DOWNLOAD_ONLY YES)
CPMAddPackage(
  NAME qrcodegen
  VERSION 1.8.0
  URL https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz
  URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
  DOWNLOAD_ONLY YES)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
CPMAddPackage(
  NAME zlib
  VERSION 1.3.2
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
  URL_HASH SHA256=b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
  OPTIONS "ZLIB_BUILD_TESTING OFF" "ZLIB_BUILD_SHARED OFF" "ZLIB_BUILD_STATIC ON" "ZLIB_INSTALL OFF")
foreach(json_header_patch
    "include/nlohmann/detail/input/binary_reader.hpp|static inline bool little_endianness|inline bool little_endianness"
    "include/nlohmann/detail/string_escape.hpp|static void unescape|inline void unescape")
  string(REPLACE "|" ";" json_header_patch "${json_header_patch}")
  list(GET json_header_patch 0 json_header)
  list(GET json_header_patch 1 json_old)
  list(GET json_header_patch 2 json_new)
  set(json_header "${json_SOURCE_DIR}/${json_header}")
  file(READ "${json_header}" json_text)
  string(FIND "${json_text}" "${json_old}" json_at)
  if(NOT json_at EQUAL -1)
    string(REPLACE "${json_old}" "${json_new}" json_text "${json_text}")
    file(WRITE "${json_header}" "${json_text}")
  endif()
endforeach()
find_package(OpenGL REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static OpenGL::GL)
set_property(TARGET imgui PROPERTY CXX_SCAN_FOR_MODULES OFF)

file(READ "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" font HEX)
string(REGEX REPLACE "(..)" "0x\\1," font "${font}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/font.hpp" CONTENT "inline constexpr unsigned char launcherFont[] = {${font}};\n")

add_library(game STATIC)
target_sources(game PUBLIC FILE_SET CXX_MODULES FILES src/game.cppm)
add_executable(gw2-multi-launcher WIN32)
target_sources(gw2-multi-launcher PRIVATE src/main.cpp)
target_sources(gw2-multi-launcher PRIVATE FILE_SET CXX_MODULES FILES
  src/data_folder.cppm src/ui_surface.cppm src/app_extension.cppm
  src/platform.cppm src/accounts.cppm src/epic.cppm src/session.cppm src/update_common.cppm src/app.cppm src/ui.cppm)
target_compile_definitions(gw2-multi-launcher PRIVATE GW2_APP_VERSION="${PROJECT_VERSION}")
target_include_directories(gw2-multi-launcher PRIVATE "${CMAKE_BINARY_DIR}")
target_link_libraries(gw2-multi-launcher PRIVATE game imgui nlohmann_json::nlohmann_json)

set(gw2_app_icon "")
if(EXISTS "${CMAKE_SOURCE_DIR}/private/Icon.cmake")
  include("${CMAKE_SOURCE_DIR}/private/Icon.cmake")
endif()
if(NOT GW2_PRODUCT_NAME)
  set(GW2_PRODUCT_NAME "GW2 Multi Launcher")
endif()
if(NOT GW2_APP_ID)
  set(GW2_APP_ID "GW2MultiLauncher")
endif()
if(NOT GW2_PROGRAM_FILE)
  set(GW2_PROGRAM_FILE "GW2MultiLauncher")
endif()
if(NOT GW2_RELEASE_TITLE)
  set(GW2_RELEASE_TITLE "Gw2 Multi")
endif()
if(NOT GW2_HELPER_MAGIC)
  set(GW2_HELPER_MAGIC "0x374C4D47")
endif()
if(NOT GW2_PACKET_MAGIC)
  set(GW2_PACKET_MAGIC "0x4E4C4D47")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/release-title.txt" "${GW2_RELEASE_TITLE}\n")
set_target_properties(gw2-multi-launcher PROPERTIES OUTPUT_NAME "${GW2_PROGRAM_FILE}")
target_compile_definitions(gw2-multi-launcher PRIVATE GW2_PROGRAM_FILE="${GW2_PROGRAM_FILE}" GW2_APP_ID="${GW2_APP_ID}"
  GW2_HELPER_MAGIC=${GW2_HELPER_MAGIC})
if(NOT gw2_app_icon)
  configure_file(src/icons/gw2-multi-launcher.png
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${GW2_PROGRAM_FILE}.png" COPYONLY)
endif()

# The Steam helper is a separate process so a sign-in failure cannot take the UI with it.
set(GW2_STEAM_DIR "" CACHE PATH "Pre-published Steam helper for the target operating system")
set(steam_output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(WIN32)
  set(steam_executable "${GW2_PROGRAM_FILE}.Steam.exe")
else()
  set(steam_executable "${GW2_PROGRAM_FILE}.Steam")
endif()
set(GW2_STEAM_EXECUTABLE "${steam_output}/${steam_executable}")
if(GW2_STEAM_DIR)
  file(GLOB_RECURSE steam_files CONFIGURE_DEPENDS RELATIVE "${GW2_STEAM_DIR}" "${GW2_STEAM_DIR}/*")
  foreach(file IN LISTS steam_files)
    configure_file("${GW2_STEAM_DIR}/${file}" "${steam_output}/${file}" COPYONLY)
  endforeach()
  set(steam_setup_depends "${GW2_STEAM_EXECUTABLE}")
else()
  add_library(qrcodegen STATIC EXCLUDE_FROM_ALL "${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp")
  target_include_directories(qrcodegen PUBLIC "${qrcodegen_SOURCE_DIR}/cpp")
  set_property(TARGET qrcodegen PROPERTY CXX_SCAN_FOR_MODULES OFF)
  add_executable(steam-helper)
  target_sources(steam-helper PRIVATE FILE_SET CXX_MODULES FILES auth/steam.cppm)
  set_target_properties(steam-helper PROPERTIES OUTPUT_NAME "${GW2_PROGRAM_FILE}.Steam"
    RUNTIME_OUTPUT_DIRECTORY "${steam_output}")
  target_compile_definitions(steam-helper PRIVATE GW2_PRODUCT_NAME="${GW2_PRODUCT_NAME}")
  target_link_libraries(steam-helper PRIVATE qrcodegen ZLIB::ZLIBSTATIC nlohmann_json::nlohmann_json)
  if(WIN32)
    set_target_properties(steam-helper PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(steam-helper PRIVATE /ENTRY:mainCRTStartup)
    target_link_libraries(steam-helper PRIVATE winhttp ws2_32 bcrypt)
  else()
    target_link_libraries(steam-helper PRIVATE OpenSSL::Crypto CURL::libcurl)
  endif()
  add_dependencies(gw2-multi-launcher steam-helper)
  set(steam_setup_depends steam-helper)
endif()

if(WIN32)
  enable_language(RC)
  if(NOT gw2_app_icon)
  # Keep one source image; Windows resources and Inno Setup require an ICO wrapper.
  find_program(POWERSHELL_EXECUTABLE powershell REQUIRED)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/icon.ps1" CONTENT [=[
param([string]$Source, [string]$Destination)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$image = [Drawing.Image]::FromFile($Source)
$sizes = 16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256
$output = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($output)
try {
    $writer.Write([byte[]]((0, 0, 1, 0) + [BitConverter]::GetBytes([uint16]$sizes.Count)))
    $output.SetLength(6 + 16 * $sizes.Count)
    for ($i = 0; $i -lt $sizes.Count; ++$i) {
        $size = $sizes[$i]
        $bitmap = [Drawing.Bitmap]::new($size, $size)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $data = [IO.MemoryStream]::new()
        try {
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage($image, 0, 0, $size, $size)
            $bitmap.Save($data, [Drawing.Imaging.ImageFormat]::Png)
            $offset = $output.Length
            $output.Position = 6 + 16 * $i
            # ICO encodes 256px as zero; each entry contains its own 32-bit PNG.
            $dimension = $size % 256
            $writer.Write([byte[]](($dimension, $dimension, 0, 0, 1, 0, 32, 0) +
                [BitConverter]::GetBytes([uint32]$data.Length) + [BitConverter]::GetBytes([uint32]$offset)))
            $output.Position = $offset
            $writer.Write($data.ToArray())
        } finally {
            $data.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
    [IO.File]::WriteAllBytes($Destination, $output.ToArray())
} finally {
    $writer.Dispose()
    $output.Dispose()
    $image.Dispose()
}
]=])
  add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/icon.ico"
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_BINARY_DIR}/icon.ps1"
      "${CMAKE_SOURCE_DIR}/src/icons/gw2-multi-launcher.png" "${CMAKE_BINARY_DIR}/icon.ico"
    DEPENDS src/icons/gw2-multi-launcher.png "${CMAKE_BINARY_DIR}/icon.ps1" VERBATIM)
  endif()
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/icon.rc" CONTENT "1 ICON \"${CMAKE_BINARY_DIR}/icon.ico\"\n")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/icon-refresh.ps1" CONTENT [=[
param([Parameter(Mandatory = $true)][string]$Path)
$signature = @'
using System;
using System.Runtime.InteropServices;
public static class Gw2ShellNotify {
    [DllImport("shell32.dll", EntryPoint = "SHChangeNotify", CharSet = CharSet.Unicode)]
    public static extern void PathChanged(int eventId, uint flags, string item, System.IntPtr unused);
    [DllImport("shell32.dll", EntryPoint = "SHChangeNotify")]
    public static extern void Broadcast(int eventId, uint flags, System.IntPtr item, System.IntPtr unused);
}
'@
if (-not ('Gw2ShellNotify' -as [type])) { Add-Type -TypeDefinition $signature }
[Gw2ShellNotify]::PathChanged(0x2000, 0x1005, $Path, [IntPtr]::Zero)
[Gw2ShellNotify]::Broadcast(0x08000000, 0x1000, [IntPtr]::Zero, [IntPtr]::Zero)
]=])
  set_source_files_properties("${CMAKE_BINARY_DIR}/icon.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/icon.ico")
  target_sources(gw2-multi-launcher PRIVATE "${CMAKE_BINARY_DIR}/icon.rc" "${CMAKE_BINARY_DIR}/icon.ico")
  if(TARGET steam-helper)
    target_sources(steam-helper PRIVATE "${CMAKE_BINARY_DIR}/icon.rc" "${CMAKE_BINARY_DIR}/icon.ico")
  endif()
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use MSVC x64 for the Windows native bridge.")
  endif()
  target_link_libraries(gw2-multi-launcher PRIVATE crypt32 bcrypt winhttp shell32 ole32 user32)
  add_subdirectory(${minhook_SOURCE_DIR} ${minhook_BINARY_DIR} EXCLUDE_FROM_ALL)
  add_library(GW2MultiLauncher.Native SHARED)
  target_sources(GW2MultiLauncher.Native PRIVATE FILE_SET CXX_MODULES FILES native/Bridge.cppm)
  set_target_properties(GW2MultiLauncher.Native PROPERTIES OUTPUT_NAME "${GW2_PROGRAM_FILE}.Native")
  target_link_libraries(GW2MultiLauncher.Native PRIVATE game minhook user32 advapi32 bcrypt comctl32)
  target_sources(GW2MultiLauncher.Native PRIVATE "${CMAKE_BINARY_DIR}/icon.rc" "${CMAKE_BINARY_DIR}/icon.ico")
  add_executable(GW2MultiLauncher.Host WIN32)
  target_sources(GW2MultiLauncher.Host PRIVATE FILE_SET CXX_MODULES FILES native/Host.cppm)
  set_target_properties(GW2MultiLauncher.Host PROPERTIES OUTPUT_NAME "${GW2_PROGRAM_FILE}.Host")
  target_link_libraries(GW2MultiLauncher.Host PRIVATE game GW2MultiLauncher.Native user32 bcrypt)
  target_sources(GW2MultiLauncher.Host PRIVATE "${CMAKE_BINARY_DIR}/icon.rc" "${CMAKE_BINARY_DIR}/icon.ico")
  target_link_options(GW2MultiLauncher.Host PRIVATE /ENTRY:mainCRTStartup)
  set(native_targets GW2MultiLauncher.Native GW2MultiLauncher.Host)
  target_compile_definitions(GW2MultiLauncher.Host PRIVATE GW2_HELPER_MAGIC=${GW2_HELPER_MAGIC})
  target_compile_definitions(GW2MultiLauncher.Native PRIVATE GW2_PACKET_MAGIC=${GW2_PACKET_MAGIC})
  if(GW2_NATIVE_CHANNEL)
    foreach(target IN LISTS native_targets)
      target_compile_definitions(${target} PRIVATE "GW2_NATIVE_CHANNEL_W=L\"${GW2_NATIVE_CHANNEL}\"")
    endforeach()
  endif()
  if(GW2_NATIVE_EXECUTE)
    target_compile_definitions(GW2MultiLauncher.Host PRIVATE GW2_NATIVE_EXECUTE=${GW2_NATIVE_EXECUTE})
    target_compile_definitions(GW2MultiLauncher.Native PRIVATE
      GW2_NATIVE_EXECUTE=${GW2_NATIVE_EXECUTE} GW2_NATIVE_HOOK=${GW2_NATIVE_HOOK})
  endif()
  foreach(target IN ITEMS gw2-multi-launcher ${native_targets})
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_BINARY_DIR}/icon-refresh.ps1" "$<TARGET_FILE:${target}>"
      VERBATIM)
  endforeach()
  if(TARGET steam-helper)
    add_custom_command(TARGET steam-helper POST_BUILD
      COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_BINARY_DIR}/icon-refresh.ps1" "$<TARGET_FILE:steam-helper>"
      VERBATIM)
  endif()
  set(warned_targets gw2-multi-launcher game ${native_targets})
  if(TARGET steam-helper)
    list(APPEND warned_targets steam-helper)
  endif()
  foreach(target IN LISTS warned_targets)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /guard:cf /sdl)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_options(${target} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  target_link_libraries(gw2-multi-launcher PRIVATE PkgConfig::SECRET OpenSSL::Crypto CURL::libcurl)
  foreach(target IN ITEMS gw2-multi-launcher game)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endforeach()
  set(GW2_NATIVE_DIR "" CACHE PATH "Directory containing matching Windows helper and bridge built from this source")
  foreach(file "${GW2_PROGRAM_FILE}.Host.exe" "${GW2_PROGRAM_FILE}.Native.dll")
    if(NOT EXISTS "${GW2_NATIVE_DIR}/${file}")
      message(FATAL_ERROR "Set GW2_NATIVE_DIR to a matching Windows build's bin directory; missing ${file}.")
    endif()
    configure_file("${GW2_NATIVE_DIR}/${file}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${file}" COPYONLY)
  endforeach()
endif()

set(gw2_update_source "${CMAKE_SOURCE_DIR}/src/update.cppm")
if(EXISTS "${CMAKE_SOURCE_DIR}/private/CMakeLists.txt")
  add_subdirectory(private "${CMAKE_BINARY_DIR}/gw2-private")
endif()
target_sources(gw2-multi-launcher PRIVATE FILE_SET CXX_MODULES FILES "${gw2_update_source}")

include(Package.cmake)
