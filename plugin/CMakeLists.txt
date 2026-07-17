# plugin/CMakeLists.txt - AI Upscale plugin build.
#
# Targets:
#   upscale_core  - Adobe-independent static library (ONNX inference + tiling).
#                   Builds and is testable on any platform, including Linux
#                   with no GPU and no Adobe SDK (this repo's dev environment).
#   upscale_cli   - Command-line test harness for upscale_core (PNG in/out).
#   AIUpscale     - After Effects / Premiere Pro plugin (.aex / bundle).
#                   Only added when AE_SDK_PATH is set AND the target OS is
#                   Windows or macOS; skipped (with a status message) on Linux
#                   or when AE_SDK_PATH is not provided, since the Adobe SDK
#                   headers cannot be redistributed in this repository.
#
# Required variable:
#   ONNXRUNTIME_ROOT - path to an extracted onnxruntime distribution
#                       (containing include/ and lib/). Get one from
#                       https://github.com/microsoft/onnxruntime/releases
#
# Optional variable:
#   AE_SDK_PATH       - path to an extracted Adobe After Effects SDK
#                        (https://developer.adobe.com/after-effects/).
#                        Enables the AIUpscale target on Windows/macOS.
cmake_minimum_required(VERSION 3.16)
project(ai_upscale_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# onnxruntime
# ---------------------------------------------------------------------------
if(NOT DEFINED ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH
        "Path to an extracted onnxruntime release (containing include/ and lib/)")
endif()

if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    message(WARNING
        "onnxruntime headers not found at ${ONNXRUNTIME_ROOT}/include. "
        "Set -DONNXRUNTIME_ROOT=/path/to/onnxruntime (extracted release from "
        "https://github.com/microsoft/onnxruntime/releases). Configure will "
        "likely fail at the upscale_core target.")
endif()

find_library(ONNXRUNTIME_LIB
    NAMES onnxruntime
    PATHS "${ONNXRUNTIME_ROOT}/lib"
    NO_DEFAULT_PATH
)
if(NOT ONNXRUNTIME_LIB)
    message(WARNING "onnxruntime library not found under ${ONNXRUNTIME_ROOT}/lib")
endif()

# ---------------------------------------------------------------------------
# upscale_core (Adobe-independent)
# ---------------------------------------------------------------------------
add_library(upscale_core STATIC
    src/core/onnx_upscaler.cpp
    src/core/tile.cpp
)
target_include_directories(upscale_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    PRIVATE
        ${ONNXRUNTIME_ROOT}/include
)
target_link_libraries(upscale_core PUBLIC ${ONNXRUNTIME_LIB})

# Execution-provider feature flags: define these when the corresponding
# onnxruntime EP libraries are present in ONNXRUNTIME_ROOT/lib, so
# onnx_upscaler.cpp can compile in (and try) that provider. All default
# off; this repo's Linux dev environment only ever exercises CPU.
option(UPSCALE_WITH_CUDA "Enable CUDA execution provider (requires CUDA-enabled onnxruntime build)" OFF)
option(UPSCALE_WITH_DIRECTML "Enable DirectML execution provider (Windows only)" OFF)
option(UPSCALE_WITH_COREML "Enable CoreML execution provider (macOS only)" OFF)

if(UPSCALE_WITH_CUDA)
    target_compile_definitions(upscale_core PRIVATE UPSCALE_WITH_CUDA)
endif()
if(UPSCALE_WITH_DIRECTML AND WIN32)
    target_compile_definitions(upscale_core PRIVATE UPSCALE_WITH_DIRECTML)
endif()
if(UPSCALE_WITH_COREML AND APPLE)
    target_compile_definitions(upscale_core PRIVATE UPSCALE_WITH_COREML)
endif()

# Copy the onnxruntime shared library next to build products so upscale_cli
# (and, on Windows, the .aex) can find it at runtime without needing it on
# a system path.
if(EXISTS "${ONNXRUNTIME_ROOT}/lib")
    file(GLOB ONNXRUNTIME_SHARED_LIBS
        "${ONNXRUNTIME_ROOT}/lib/*.so*"
        "${ONNXRUNTIME_ROOT}/lib/*.dylib*"
        "${ONNXRUNTIME_ROOT}/lib/*.dll"
    )
endif()

# ---------------------------------------------------------------------------
# upscale_cli (test harness, PNG in/out via stb_image)
# ---------------------------------------------------------------------------
add_executable(upscale_cli
    src/cli/upscale_cli.cpp
)
target_include_directories(upscale_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(upscale_cli PRIVATE upscale_core)

if(ONNXRUNTIME_SHARED_LIBS)
    foreach(lib_file ${ONNXRUNTIME_SHARED_LIBS})
        add_custom_command(TARGET upscale_cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lib_file}" "$<TARGET_FILE_DIR:upscale_cli>"
        )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# AIUpscale (Adobe After Effects / Premiere Pro plugin)
# ---------------------------------------------------------------------------
if(DEFINED AE_SDK_PATH)
    if(WIN32 OR APPLE)
        message(STATUS "AE_SDK_PATH set (${AE_SDK_PATH}); configuring AIUpscale plugin target.")

        add_library(AIUpscale MODULE
            src/plugin/AIUpscale.cpp
        )

        # AE SDK sample plugins conventionally pull headers from
        # Examples/Headers, Examples/Util, and Examples/Resources (for the
        # .r PiPL). Adjust these subpaths if a different SDK layout is used.
        target_include_directories(AIUpscale PRIVATE
            ${AE_SDK_PATH}/Examples/Headers
            ${AE_SDK_PATH}/Examples/Headers/SP
            ${AE_SDK_PATH}/Examples/Util
            ${AE_SDK_PATH}/Examples/Resources
            ${ONNXRUNTIME_ROOT}/include
        )
        target_link_libraries(AIUpscale PRIVATE upscale_core)

        if(WIN32)
            # After Effects/Premiere plugins on Windows are DLLs renamed to
            # .aex, built with a matching .def/.r-derived resource. A full
            # build additionally needs the PiPL compiled into a .rc via the
            # SDK's PiPLtool (or Rez-equivalent) -- see plugin/README.md.
            set_target_properties(AIUpscale PROPERTIES
                SUFFIX ".aex"
                OUTPUT_NAME "AIUpscale"
            )
        elseif(APPLE)
            # AE/Premiere plugins on macOS are bundles with a .plugin
            # extension containing the PiPL resource compiled via Rez and
            # embedded per the SDK's bundle template (Info.plist, PkgInfo).
            # This sets up the bundle shape; populating Info.plist/PkgInfo
            # from the SDK's template is left to the real build (see
            # plugin/README.md for the manual steps).
            set_target_properties(AIUpscale PROPERTIES
                BUNDLE TRUE
                BUNDLE_EXTENSION "plugin"
                OUTPUT_NAME "AIUpscale"
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/Info.plist.in"
            )
        endif()
    else()
        message(STATUS "AE_SDK_PATH set but target OS is neither Windows nor macOS; skipping AIUpscale plugin target (Adobe AE/Premiere plugins are not supported on Linux).")
    endif()
else()
    message(STATUS "AE_SDK_PATH not set; skipping AIUpscale plugin target. "
        "Set -DAE_SDK_PATH=/path/to/AfterEffectsSDK to build it (Windows/macOS only). "
        "upscale_core and upscale_cli still build normally.")
endif()
