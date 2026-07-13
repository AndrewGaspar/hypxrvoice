cmake_minimum_required(VERSION 3.20)
project(hypxrvoice LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---- system dependencies (found via pkg-config; see README "Requirements") ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(spa      REQUIRED IMPORTED_TARGET libspa-0.2)
pkg_check_modules(sndfile  REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(jansson  REQUIRED IMPORTED_TARGET jansson)

# ---- vendored whisper.cpp (subprojects/, pinned shallow submodule) ----
# whisper.cpp declares cmake_minimum_required(3.5); CMake 4 needs this shim to
# still honor its old-policy compatibility.
set(CMAKE_POLICY_VERSION_MINIMUM "3.10" CACHE STRING "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/subprojects/whisper.cpp/CMakeLists.txt)
    message(FATAL_ERROR "subprojects/whisper.cpp is missing — run: git submodule update --init --depth 1")
endif()
add_subdirectory(subprojects/whisper.cpp EXCLUDE_FROM_ALL)

# ---- vendored llama.cpp (subprojects/, pinned shallow submodule) — OPTIONAL ----
# Provides the local-LLM (GBNF-constrained) intent backend. When absent or disabled,
# the daemon falls back to the deterministic rule backend (HAVE_LLAMA undefined).
option(HYPXRVOICE_LLAMA "Build the local-LLM intent backend (subprojects/llama.cpp)" ON)
set(HAVE_LLAMA FALSE)
if(HYPXRVOICE_LLAMA AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/subprojects/llama.cpp/CMakeLists.txt)
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON   ON  CACHE BOOL "" FORCE) # we link llama-common
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
    add_subdirectory(subprojects/llama.cpp EXCLUDE_FROM_ALL)
    set(HAVE_LLAMA TRUE)
    message(STATUS "hypxrvoice: local-LLM intent backend ENABLED (llama.cpp)")
else()
    message(STATUS "hypxrvoice: local-LLM intent backend disabled (rule backend only)")
endif()

# ---- WP-V5: optional OpenXR HUD overlay subprocess deps ----
# The in-headset HUD is rendered by a separate `hypxrvoice-hud` process (like the
# hypxrpaper sibling): single-threaded OpenXR + EGL/GBM. When these libs are absent the
# subprocess is simply not built and the daemon degrades to notify-send at runtime.
set(HAVE_XR FALSE)
pkg_check_modules(xrdeps IMPORTED_TARGET openxr egl glesv2 gbm libdrm)
if(xrdeps_FOUND)
    set(HAVE_XR TRUE)
    message(STATUS "hypxrvoice: in-headset HUD overlay ENABLED (openxr/egl/gbm found)")
else()
    message(STATUS "hypxrvoice: HUD overlay subprocess disabled (openxr/egl/gbm not found) — notify-send fallback")
endif()

# ---- bundled OFL font, baked into the binary (font_data.c) ----
set(HUD_FONT_TTF ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LiberationMono-Regular.ttf)
set(HUD_FONT_C   ${CMAKE_CURRENT_BINARY_DIR}/font_data.c)
add_custom_command(
    OUTPUT ${HUD_FONT_C}
    COMMAND ${CMAKE_COMMAND} -DFONT_IN=${HUD_FONT_TTF} -DOUT_C=${HUD_FONT_C}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake
    DEPENDS ${HUD_FONT_TTF} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake
    COMMENT "Embedding HUD font (LiberationMono, OFL)"
    VERBATIM)

# ---- hud_core: the light, pure feedback library (NO whisper/pipewire/XR) ----
# Shared by the daemon (via hypxrvoice_core) AND the hypxrvoice-hud subprocess, so the
# HUD renderer never pulls in the heavy ASR/audio stack. Depends only on jansson (wire
# format) + the vendored stb headers + the bundled font.
add_library(hypxrvoice_hud_core STATIC
    src/Log.cpp
    src/HudModel.cpp
    src/HudText.cpp
    src/HudMessage.cpp
    src/PngWrite.cpp
    src/Tts.cpp
    src/stb_impl.cpp
    ${HUD_FONT_C}
)
target_include_directories(hypxrvoice_hud_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(hypxrvoice_hud_core PUBLIC PkgConfig::jansson)
target_compile_options(hypxrvoice_hud_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# The stb single-header impls have their own upstream warning style.
set_source_files_properties(src/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---- core library (shared by the daemon and the tests) ----
add_library(hypxrvoice_core STATIC
    src/Config.cpp
    src/Transcript.cpp
    src/ActivationMachine.cpp
    src/Vad.cpp
    src/WakeWord.cpp
    src/Asr.cpp
    src/Wav.cpp
    src/Compositor.cpp
    src/AudioCapture.cpp
    src/ControlSocket.cpp
    src/Feedback.cpp
    src/HudOverlay.cpp
    src/Pipeline.cpp
    src/Command.cpp
    src/DesktopContext.cpp
    src/GazeResolver.cpp
    src/Grammar.cpp
    src/Intent.cpp
    src/LlamaIntent.cpp
    src/Executor.cpp
    src/IntentPipeline.cpp
    src/Daemon.cpp
)
target_include_directories(hypxrvoice_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hypxrvoice_core PUBLIC
    hypxrvoice_hud_core
    whisper
    PkgConfig::pipewire
    PkgConfig::spa
    PkgConfig::sndfile
    PkgConfig::jansson
)
target_compile_options(hypxrvoice_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

if(HAVE_LLAMA)
    target_compile_definitions(hypxrvoice_core PUBLIC HAVE_LLAMA)
    # llama.cpp's "common" helper lib is target `llama-common` in this pin.
    target_link_libraries(hypxrvoice_core PUBLIC llama llama-common)
    target_include_directories(hypxrvoice_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/subprojects/llama.cpp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/subprojects/llama.cpp/common
        ${CMAKE_CURRENT_SOURCE_DIR}/subprojects/llama.cpp/ggml/include)
endif()

# ---- daemon ----
add_executable(hypxrvoiced src/main.cpp)
target_link_libraries(hypxrvoiced PRIVATE hypxrvoice_core)
target_compile_options(hypxrvoiced PRIVATE -Wall -Wextra)

# ---- control CLI (deliberately light: no whisper/pipewire link) ----
add_executable(hypxrvoicectl src/ctl_main.cpp src/ControlSocket.cpp src/Log.cpp)
target_include_directories(hypxrvoicectl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(hypxrvoicectl PRIVATE -Wall -Wextra)

install(TARGETS hypxrvoiced hypxrvoicectl RUNTIME DESTINATION bin)

# ---- WP-V5: hypxrvoice-hud overlay subprocess (only when XR deps are present) ----
if(HAVE_XR)
    add_executable(hypxrvoice-hud
        src/hud/hud_main.cpp
        src/hud/HudSession.cpp
        src/hud/Egl.cpp
    )
    target_link_libraries(hypxrvoice-hud PRIVATE hypxrvoice_hud_core PkgConfig::xrdeps)
    target_compile_options(hypxrvoice-hud PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    install(TARGETS hypxrvoice-hud RUNTIME DESTINATION bin)
endif()

# ---- tests ----
enable_testing()
add_executable(hypxrvoice_tests
    tests/tests_main.cpp
    tests/test_activation.cpp
    tests/test_config.cpp
    tests/test_vad.cpp
    tests/test_wakeword.cpp
    tests/test_transcript.cpp
    tests/test_compositor.cpp
    tests/test_oneshot.cpp
    tests/test_context.cpp
    tests/test_gaze.cpp
    tests/test_executor.cpp
    tests/test_intent.cpp
    tests/test_pipeline.cpp
    tests/test_llama.cpp
    tests/test_hud.cpp
    tests/test_tts.cpp
)
target_include_directories(hypxrvoice_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(hypxrvoice_tests PRIVATE hypxrvoice_core)
target_compile_options(hypxrvoice_tests PRIVATE -Wall -Wextra)
# Test assets (WAV + optional model) discovered relative to the source tree.
target_compile_definitions(hypxrvoice_tests PRIVATE
    HYPXRVOICE_TEST_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets"
    HYPXRVOICE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_test(NAME hypxrvoice_tests COMMAND hypxrvoice_tests)
