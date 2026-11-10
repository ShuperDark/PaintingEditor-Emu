# Toolchain: builds arm64 Mach-O dylibs (the format of the game) with the clang that
# ships with zig (pip package "ziglang", the same compiler the emulator's guest
# libraries are built with). Used by CMakePresets.json.
cmake_minimum_required(VERSION 3.20)

if (NOT ZIG_EXE)
  execute_process(COMMAND py -c "import ziglang, os; print(os.path.dirname(ziglang.__file__))"
                  OUTPUT_VARIABLE ZIG_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _r)
  if (NOT _r EQUAL 0 OR NOT ZIG_DIR)
    message(FATAL_ERROR "zig not found. Install it with:  py -m pip install ziglang")
  endif()
  file(TO_CMAKE_PATH "${ZIG_DIR}" ZIG_DIR)
  set(ZIG_EXE "${ZIG_DIR}/zig.exe" CACHE FILEPATH "zig compiler" FORCE)
  set(ZIG_DIR "${ZIG_DIR}" CACHE PATH "zig installation" FORCE)
endif()

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_SYSROOT "" CACHE STRING "" FORCE)
set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "" CACHE STRING "" FORCE)

set(CMAKE_C_COMPILER "${ZIG_EXE}")
set(CMAKE_C_COMPILER_ARG1 cc)
set(CMAKE_CXX_COMPILER "${ZIG_EXE}")
set(CMAKE_CXX_COMPILER_ARG1 c++)

# -mcpu=apple_a7: no ARMv8.1+ instructions (the emulator's JIT does not implement them)
set(_common "-target aarch64-macos.11.0 -mcpu=apple_a7 -fPIC -fno-stack-protector")
set(CMAKE_C_FLAGS_INIT "${_common}")
# C++: headers of the emulator's libc++ (same ABI as the game's), linked at load time
set(CMAKE_CXX_FLAGS_INIT "${_common} -nostdinc++ -I\"${ZIG_DIR}/lib/libcxx/include\" -I\"${ZIG_DIR}/lib/libcxxabi/include\" -include \"${CMAKE_CURRENT_LIST_DIR}/../include/libcxx_config_site.h\"")
# symbols (the game, libc, libc++, Foundation, the emulator's emu_* functions) are
# resolved by the emulator when the mod is loaded
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-target aarch64-macos.11.0 -nostdlib++ -Wl,-undefined,dynamic_lookup")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${CMAKE_SHARED_LINKER_FLAGS_INIT}")
# no archiver / system linker setup is needed for the dylib, skip the generic compiler checks
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)
