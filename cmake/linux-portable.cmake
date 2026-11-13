# Build the release package in Ubuntu 22.04 with LLVM 21.
# New C++ features must not impose a newer system libstdc++ on package users.
set(CMAKE_C_COMPILER clang-21 CACHE STRING "")
set(CMAKE_CXX_COMPILER clang++-21 CACHE STRING "")
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++ -pthread")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++")
set(GW2_STATIC_LIBCXX_NOTICES "/usr/share/doc/libc++-21-dev/copyright" CACHE FILEPATH "LLVM runtime notices")
