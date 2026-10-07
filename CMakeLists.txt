cmake_minimum_required(VERSION 3.16)
project(ac2native C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

include(gen/sources.cmake)
# generated code: big switch-heavy functions; -O1 keeps compile times sane
set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-w;-fno-strict-aliasing")

set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_executable(ac2native
  runtime/main.cpp frontend/config.cpp frontend/ui.cpp frontend/launcher.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp
  runtime/core.c runtime/bios.c runtime/devices.c runtime/cdrom.c runtime/gpu.c
  runtime/spu.c runtime/mdec.c runtime/gte.c runtime/iso.c runtime/movie.c runtime/platform.cpp
  ${RECOMP_SOURCES})
target_include_directories(ac2native PRIVATE runtime gen frontend ${IMGUI} ${IMGUI}/backends)
target_compile_options(ac2native PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-label -Wno-unused-function>)
target_link_libraries(ac2native PRIVATE SDL2::SDL2 OpenGL::GL Threads::Threads m)
