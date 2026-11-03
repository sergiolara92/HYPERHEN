cmake_minimum_required(VERSION 3.20)

set(basename "onion_sprx")
project(${basename} C CXX)

onion_module_prologue(C_STANDARD 17 CXX_STANDARD 20)

set(_game_runtime "${HYPERHEN_THIRD_PARTY_DIR}/tachen-plugin-runtime/game-runtime")
set(_worker_dir "${CMAKE_CURRENT_BINARY_DIR}/worker")
file(MAKE_DIRECTORY "${_worker_dir}")
add_custom_command(OUTPUT "${_worker_dir}/worker.bin"
  COMMAND "${CMAKE_C_COMPILER}" -c -O2 -ffreestanding -fPIC
    -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
    -mno-red-zone -mno-avx -I "${_game_runtime}"
    "${_game_runtime}/game-gate.c" -o "${_worker_dir}/worker.o"
  COMMAND "${CMAKE_LINKER}" --no-undefined
    -T "${_game_runtime}/game-gate.ld"
    "${_worker_dir}/worker.o" -o "${_worker_dir}/worker.elf"
  COMMAND "${CMAKE_OBJCOPY}" -O binary --only-section=.text
    "${_worker_dir}/worker.elf" "${_worker_dir}/worker.bin"
  DEPENDS "${_game_runtime}/game-gate.c" "${_game_runtime}/game-gate.ld" "${_game_runtime}/game-gate.h"
  VERBATIM)
set_source_files_properties(source/plugin_worker_blob.c PROPERTIES
  OBJECT_DEPENDS "${_worker_dir}/worker.bin")

add_library(${PROJECT_NAME} STATIC
  source/sprx_loader.cpp
  source/sprx_catalog.cpp
  source/sprx_discovery.cpp
  source/sprx_plugin_manager.cpp
  source/ptrace_sprx_runtime.cpp
  source/plugin_worker_blob.c
  source/plugin_staging.cpp
  source/plugin_metadata.cpp
  "${_game_runtime}/game-inject.c"
  "${_game_runtime}/game-lease.c"
  "${_game_runtime}/game-imports.c"
  "${_game_runtime}/memory-map.c"
  "${_game_runtime}/publication.c"
  "${_game_runtime}/game-trampoline.cpp"
  "${_game_runtime}/relocate.cpp"
  "${_game_runtime}/hde64.cpp"
  "${HYPERHEN_THIRD_PARTY_DIR}/tachen-plugin-runtime/prx-image.c"
  "${HYPERHEN_THIRD_PARTY_DIR}/tachen-plugin-runtime/process-info.c"
  "${HYPERHEN_THIRD_PARTY_DIR}/tachen-plugin-runtime/game-info.c"
  "${HYPERHEN_THIRD_PARTY_DIR}/tachen-plugin-runtime/game-source.c"
  "${HYPERHEN_THIRD_PARTY_DIR}/cheat_support/sha256.c"
)
target_compile_definitions(${PROJECT_NAME} PRIVATE HDE_DECODE_ONLY
  HYPERHEN_PLUGIN_WORKER_BIN="${_worker_dir}/worker.bin")
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${PROJECT_NAME}")

target_compile_options(${PROJECT_NAME} PUBLIC
  $<$<COMPILE_LANGUAGE:CXX>:${CXX_DEFS} ${CXX_FLAGS}>
)

set(CMAKE_CXX_FLAGS "${ONION_PS5_TARGET_FLAGS} -gfull -gdwarf-2 -pedantic -pedantic-errors -Wall -Werror -fPIC -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")

target_include_directories(${PROJECT_NAME}
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${_game_runtime}"
          "${PROJECT_ROOT}/libonion_elfldr/include"
          "${HYPERHEN_THIRD_PARTY_DIR}/tachen-plugin-runtime"
          "${HYPERHEN_THIRD_PARTY_DIR}/cheat_support"
          "${HYPERHEN_THIRD_PARTY_DIR}/cjson"
          "${PROJECT_ROOT}/libonion_proc/include"
          "${PROJECT_ROOT}/libonion_platform/include"
          "${PROJECT_ROOT}/include"
)

add_dependencies(${PROJECT_NAME} onion_elfldr onion_proc)
target_link_libraries(${PROJECT_NAME} PUBLIC onion_elfldr onion_proc)
