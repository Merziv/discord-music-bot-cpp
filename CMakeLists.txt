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
  $<$<CONFIG:Release>:-flto>
)

target_link_options(music_bot PRIVATE
  -static-libgcc
  -static-libstdc++
  $<$<CONFIG:Release>:-flto>
  $<$<CONFIG:Release>:-s>
)

find_package(dpp REQUIRED)
find_package(Opus REQUIRED)
find_package(RapidJSON REQUIRED)

target_link_libraries(music_bot PRIVATE
  dpp::dpp
  Opus::opus
  rapidjson
)
