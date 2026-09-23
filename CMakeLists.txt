cmake_minimum_required(VERSION 3.30)
project(GW2MultiLauncher VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(FetchContent)

# Dependencies stay in the build tree. Keep exact revisions and archive hashes.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
foreach(feature AUDIO CAMERA JOYSTICK HAPTIC SENSOR GPU RENDER WAYLAND HIDAPI POWER TRAY)
  set(SDL_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.tar.gz
  URL_HASH SHA256=c2ee715e42ec520c4d11fd8d249ef1d2b2baf4ad31148b72b3b000276c0b3633)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/3bae66c735670619baf51391eba7f3d90a25d125.tar.gz
  URL_HASH SHA256=0b99c7baed551f76058bc6fe83d806dc55c912280e9d2d268879a1c9751cef20)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187)
# Linux packages also carry the license for their Windows game helper.
FetchContent_Declare(minhook
  URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
  URL_HASH SHA256=1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e
  SOURCE_SUBDIR source-only)
FetchContent_MakeAvailable(SDL3 imgui json minhook)
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
configure_file(src/icons/gw2-multi-launcher.bmp
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gw2-multi-launcher.bmp" COPYONLY)

add_library(game STATIC)
target_sources(game PUBLIC FILE_SET CXX_MODULES FILES src/game.cppm)
add_executable(gw2-multi-launcher WIN32 src/main.cpp src/accounts.cpp src/epic.cpp)
set_target_properties(gw2-multi-launcher PROPERTIES OUTPUT_NAME GW2MultiLauncher)
target_sources(gw2-multi-launcher PRIVATE FILE_SET CXX_MODULES FILES
  src/platform.cppm src/accounts.cppm src/epic.cppm src/session.cppm src/update.cppm src/app.cppm src/ui.cppm)
target_compile_definitions(gw2-multi-launcher PRIVATE GW2_APP_VERSION="${PROJECT_VERSION}")
target_include_directories(gw2-multi-launcher PRIVATE "${CMAKE_BINARY_DIR}")
target_link_libraries(gw2-multi-launcher PRIVATE game imgui nlohmann_json::nlohmann_json)

# Keep SteamKit dynamically replaceable, and isolate its runtime from the C++ UI.
set(GW2_STEAM_DIR "" CACHE PATH "Pre-published Steam helper for the target operating system")
set(steam_output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/steam")
if(GW2_STEAM_DIR)
  file(GLOB_RECURSE steam_files CONFIGURE_DEPENDS RELATIVE "${GW2_STEAM_DIR}" "${GW2_STEAM_DIR}/*")
  foreach(file IN LISTS steam_files)
    configure_file("${GW2_STEAM_DIR}/${file}" "${steam_output}/${file}" COPYONLY)
  endforeach()
else()
  find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
  if(WIN32)
    set(steam_rid win-x64)
    set(steam_executable GW2MultiLauncher.Steam.exe)
  else()
    set(steam_rid linux-x64)
    set(steam_executable GW2MultiLauncher.Steam)
  endif()
  add_custom_command(OUTPUT "${steam_output}/${steam_executable}" "${steam_output}/GW2MultiLauncher.Steam.dll"
    COMMAND "${DOTNET_EXECUTABLE}" publish "${CMAKE_SOURCE_DIR}/auth/GW2MultiLauncher.Steam.csproj"
      -c Release -r ${steam_rid} --self-contained true --nologo
      --artifacts-path "${CMAKE_BINARY_DIR}/auth" -o "${steam_output}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${steam_output}/${steam_executable}" "${steam_output}/GW2MultiLauncher.Steam.dll"
    DEPENDS auth/GW2MultiLauncher.Steam.csproj auth/Program.cs auth/packages.lock.json VERBATIM)
  add_custom_target(steam-auth DEPENDS "${steam_output}/${steam_executable}")
  add_dependencies(gw2-multi-launcher steam-auth)
endif()

if(WIN32)
  enable_language(RC)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/icon.rc" CONTENT "1 ICON \"${CMAKE_SOURCE_DIR}/src/icons/gw2-multi-launcher.ico\"\n")
  target_sources(gw2-multi-launcher PRIVATE "${CMAKE_BINARY_DIR}/icon.rc")
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use MSVC x64 for the Windows native bridge.")
  endif()
  target_link_libraries(gw2-multi-launcher PRIVATE crypt32 bcrypt winhttp shell32 ole32 user32)
  add_subdirectory(${minhook_SOURCE_DIR} ${minhook_BINARY_DIR} EXCLUDE_FROM_ALL)
  add_library(GW2MultiLauncher.Native SHARED native/Bridge.cpp)
  target_link_libraries(GW2MultiLauncher.Native PRIVATE game minhook user32 advapi32 bcrypt comctl32)
  add_executable(GW2MultiLauncher.Host WIN32 native/Host.cpp)
  target_link_libraries(GW2MultiLauncher.Host PRIVATE game GW2MultiLauncher.Native user32)
  target_link_options(GW2MultiLauncher.Host PRIVATE /ENTRY:mainCRTStartup)
  set(native_targets GW2MultiLauncher.Native GW2MultiLauncher.Host)
  foreach(target IN ITEMS gw2-multi-launcher game ${native_targets})
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
  foreach(file GW2MultiLauncher.Host.exe GW2MultiLauncher.Native.dll)
    if(NOT EXISTS "${GW2_NATIVE_DIR}/${file}")
      message(FATAL_ERROR "Set GW2_NATIVE_DIR to a matching Windows build's bin directory; missing ${file}.")
    endif()
    configure_file("${GW2_NATIVE_DIR}/${file}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${file}" COPYONLY)
  endforeach()
endif()

include(cmake/Package.cmake)
