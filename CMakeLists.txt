cmake_minimum_required(VERSION 3.28)

project(discord_music_bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(music_bot
  main.cpp
  config_manager.cpp
  audio_streamer.cpp
  extractor.cpp
)

target_compile_options(music_bot PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
  -Wno-sign-conversion
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-march=native>
  $<$<CONFIG:Release>:-flto=auto>
)

target_link_options(music_bot PRIVATE
  -static-libgcc
  -static-libstdc++
  $<$<CONFIG:Release>:-flto=auto>
  $<$<CONFIG:Release>:-s>
)

include(FetchContent)
FetchContent_Declare(
  dpp
  GIT_REPOSITORY https://github.com/brainboxdotcc/DPP.git
  GIT_TAG        v10.1.4
  GIT_SHALLOW    TRUE
)

# DPP as shared library — static build has a known issue where MLS
# sub-libraries (mlspp, hpke, etc.) are linked by bare filename, breaking
# the linker when consumed via FetchContent.
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(DPP_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(RUN_LDCONFIG OFF CACHE BOOL "" FORCE)

# explicitly manifesting this is required for us
# even though default is ON
set(BUILD_VOICE_SUPPORT ON CACHE BOOL "" FORCE)

set(DPP_INSTALL OFF CACHE BOOL "" FORCE)
set(DPP_NO_VCPKG ON CACHE BOOL "" FORCE)
set(DPP_NO_CONAN ON CACHE BOOL "" FORCE)
set(DPP_USE_PCH ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dpp)

find_package(Opus REQUIRED)
find_package(RapidJSON REQUIRED)

target_link_libraries(music_bot PRIVATE
  dpp
  Opus::opus
  rapidjson
)

# Suppress warnings from DPP headers -Wshadow issues
get_target_property(DPP_INCLUDE_DIRS dpp INTERFACE_INCLUDE_DIRECTORIES)
if(DPP_INCLUDE_DIRS)
  target_include_directories(music_bot SYSTEM PRIVATE ${DPP_INCLUDE_DIRS})
endif()
