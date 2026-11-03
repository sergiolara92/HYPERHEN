cmake_minimum_required(VERSION 3.20)
project(HyperLocalAssistantR35 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(HYPER_AI_UPSTREAM "/workspace/Hyper-P14-R31-20261028/upstream" CACHE PATH "Pinned upstream source")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(option GGML_NATIVE GGML_OPENMP GGML_BACKEND_DL GGML_BLAS GGML_CUDA GGML_VULKAN
               GGML_METAL GGML_RPC GGML_LLAMAFILE GGML_CPU_ALL_VARIANTS GGML_CCACHE
               LLAMA_BUILD_COMMON LLAMA_BUILD_SERVER LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES
               LLAMA_BUILD_TESTS LLAMA_BUILD_APP LLAMA_OPENSSL LLAMA_INSTALL_VERSION
               WHISPER_BUILD_EXAMPLES WHISPER_BUILD_SERVER WHISPER_BUILD_TESTS WHISPER_CURL
               WHISPER_COREML WHISPER_OPENVINO WHISPER_USE_SYSTEM_LLAMA)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(GGML_CPU ON CACHE BOOL "" FORCE)
set(GGML_AVX ON CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_FMA ON CACHE BOOL "" FORCE)
set(GGML_F16C ON CACHE BOOL "" FORCE)
set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
add_subdirectory("${HYPER_AI_UPSTREAM}/llama" llama)
# Both engines use the one parent GGML target, avoiding duplicate runtimes.
add_subdirectory("${HYPER_AI_UPSTREAM}/whisper" whisper)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/worker_main.cpp")
  add_executable(hyper_assistant worker_main.cpp hyper_stt.cpp hyper_dialogue.cpp
                 hyper_tts.cpp hyper_reply_tts.cpp hyper_locale.cpp hyper_runtime_session.cpp native_input_socket.c activation_gate.c)
  set_target_properties(hyper_assistant PROPERTIES
    OUTPUT_NAME "Hyper-Assistant-R35-Catalog-20261028.elf")
  target_include_directories(hyper_assistant PRIVATE "${HYPER_ESPEAK_PREFIX}/include")
  target_link_libraries(hyper_assistant PRIVATE whisper llama
    "${HYPER_ESPEAK_PREFIX}/lib/libespeak-ng.a"
    "${HYPER_ESPEAK_PREFIX}/lib/libucd.a" pthread)
  if(PS5)
    target_compile_definitions(hyper_assistant PRIVATE HYPER_P5_PS5=1)
    target_link_libraries(hyper_assistant PRIVATE SceSystemService)
  else()
    target_link_libraries(hyper_assistant PRIVATE dl)
  endif()
  target_compile_options(hyper_assistant PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
  target_link_options(hyper_assistant PRIVATE -Wl,--gc-sections)
endif()

if(NOT PS5 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hyper_pipeline_smoke.cpp")
  add_executable(hyper_language_inventory hyper_language_inventory.cpp hyper_locale.cpp)
  target_link_libraries(hyper_language_inventory PRIVATE whisper)
  add_executable(hyper_pipeline_smoke hyper_pipeline_smoke.cpp hyper_stt.cpp
                 hyper_dialogue.cpp hyper_tts.cpp hyper_locale.cpp)
  target_compile_definitions(hyper_pipeline_smoke PRIVATE HYPER_STT_HOST_TEST_PATHS=1)
  target_include_directories(hyper_pipeline_smoke PRIVATE "${HYPER_ESPEAK_PREFIX}/include")
  target_link_libraries(hyper_pipeline_smoke PRIVATE whisper llama
    "${HYPER_ESPEAK_PREFIX}/lib/libespeak-ng.a"
    "${HYPER_ESPEAK_PREFIX}/lib/libucd.a" pthread dl)
endif()
