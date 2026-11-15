cmake_minimum_required(VERSION 3.16)
project(RDR2Coop VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  # Static runtime: the .asi must not depend on a VC++ redistributable being installed.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---- portable core: protocol, networking, co-op logic
add_library(coop_core STATIC
  core/protocol.cpp
  core/socket.cpp
  core/link.cpp
  core/relay.cpp
  core/session.cpp
  core/config.cpp)
target_include_directories(coop_core PUBLIC core)
if(WIN32)
  target_link_libraries(coop_core PUBLIC ws2_32)
endif()

# ---- relay server (any OS)
add_executable(rdr2coop-relay server/main.cpp)
target_link_libraries(rdr2coop-relay PRIVATE coop_core)

# ---- tests
add_executable(coop_tests tests/test_main.cpp)
target_link_libraries(coop_tests PRIVATE coop_core)
enable_testing()
add_test(NAME coop_tests COMMAND coop_tests)

# ---- the game plugin (Windows x64 only)
if(WIN32)
  add_subdirectory(plugin)
endif()

if(MINGW)
  # Self-contained binaries: no libstdc++/libgcc/winpthread DLLs next to the game.
  foreach(target rdr2coop-relay coop_tests)
    target_link_options(${target} PRIVATE -static)
  endforeach()
endif()
