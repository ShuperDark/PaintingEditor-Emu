# PaintingEditor for the iOS MCPE (v0.16.2) Emulator: replaces the art of a painting with a
# picture (PNG / JPEG / BMP / animated GIF) and changes its size.
# Builds an arm64 dylib and copies it to the emulator's mods folder.
cmake_minimum_required(VERSION 3.20)
project(PaintingEditor C CXX)

set(CMAKE_CXX_STANDARD 20)

add_library(PaintingEditor SHARED
    src/main.cpp
    src/paintings.cpp
    src/assets.cpp
    src/screen.cpp
    src/net.cpp
    src/image.cpp)
target_include_directories(PaintingEditor PRIVATE include src third_party/stb)
set_target_properties(PaintingEditor PROPERTIES PREFIX "" SUFFIX ".dylib")
target_compile_options(PaintingEditor PRIVATE -fno-rtti -fno-exceptions)

# Emulator data folder (the mod goes to <data>/mods). Change it if you start the
# emulator with --data.
set(MCPE_DATA_DIR "$ENV{LOCALAPPDATA}/iOS-MCPE-0.16.2" CACHE PATH "iOS MCPE emulator data folder")
add_custom_command(TARGET PaintingEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MCPE_DATA_DIR}/mods"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:PaintingEditor>" "${MCPE_DATA_DIR}/mods/"
    COMMENT "Installing the mod into ${MCPE_DATA_DIR}/mods")
