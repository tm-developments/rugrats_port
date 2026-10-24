# Rugrats Adventure Game - unofficial native 64-bit port (Mohawk LB v3). GPL-3.0-or-later.
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Needs SDL2 and FFmpeg (libavformat, libavcodec, libswscale, libavutil) via pkg-config.
cmake_minimum_required(VERSION 3.16)
project(RugratsPort CXX)
option(RUGRATS_DEV "test switches and trace environment variables" OFF)
if(WIN32)
  enable_language(RC)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "configure a 64-bit build")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)

add_library(mohawk STATIC src/prepare.cpp src/mohawk.cpp src/assets.cpp src/video_tbmp.cpp src/video_sprites.cpp src/script_interpreter.cpp src/video_player.cpp src/video_decoder_libav.cpp src/audio.cpp src/lb/lb_code.cpp src/lb/lb_items.cpp src/lb/lb_engine.cpp)
target_include_directories(mohawk PUBLIC src)
if(RUGRATS_DEV)
  target_compile_definitions(mohawk PUBLIC RUGRATS_DEV)
endif()
target_link_libraries(mohawk PUBLIC PkgConfig::FFMPEG PkgConfig::SDL2)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mohawk PRIVATE -Wall -Wextra)
endif()

if(WIN32)
  add_executable(rugrats_port WIN32 src/main.cpp RESOURCE.RC)
  target_link_libraries(rugrats_port PRIVATE mohawk PkgConfig::SDL2 dbghelp)
  set_target_properties(rugrats_port PROPERTIES OUTPUT_NAME "RUGRATS" SUFFIX ".EXE")
  if(MINGW)
    target_link_options(rugrats_port PRIVATE -static)   # no libstdc++ / libgcc / winpthread DLLs to ship
  endif()
else()
  add_executable(rugrats_port src/main.cpp)
  target_link_libraries(rugrats_port PRIVATE mohawk PkgConfig::SDL2)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/test)
  add_executable(assets_test test/assets_test.cpp)
  target_link_libraries(assets_test PRIVATE mohawk)

  add_executable(tbmp_test test/tbmp_test.cpp)
  target_link_libraries(tbmp_test PRIVATE mohawk)

  add_executable(sprite_test test/sprite_test.cpp)
  target_link_libraries(sprite_test PRIVATE mohawk)

  add_executable(script_test test/script_test.cpp)
  target_link_libraries(script_test PRIVATE mohawk)
endif()
