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
# Optional variables:
#   ONNXRUNTIME_INCLUDE_DIR - path to the directory directly containing
#                        onnxruntime_cxx_api.h. Some distributions (notably
#                        Homebrew's onnxruntime formula) nest the headers
#                        under ${ONNXRUNTIME_ROOT}/include/onnxruntime/ (or
#                        deeper) rather than directly under include/. When
#                        set, this takes precedence over
#                        ${ONNXRUNTIME_ROOT}/include for the include path.
#                        Library resolution is unaffected and still uses
#                        ${ONNXRUNTIME_ROOT}/lib.
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
# Apple Silicon (M4 Max) target configuration.
#
# CMAKE_OSX_ARCHITECTURES must be set BEFORE the first project()/enable_language
# call to take effect, but this file is included after `project(... CXX)`
# above only sets the language; on macOS this is still early enough for
# CMake to honor it for compiler invocation as long as it's set before any
# target is created below (which it is). If invoking cmake configure a
# second time on an existing build dir with a different architecture,
# delete the build dir first (CMake does not re-detect toolchain arch
# changes for an existing cache).
# ---------------------------------------------------------------------------
if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING
            "Target architecture(s) for macOS builds; arm64-only by default for native M4 Max builds (no x86_64 slice, no Rosetta translation overhead)." FORCE)
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
            "Minimum macOS version; 13.0+ has the CoreML EP's MLProgram/ANE support this plugin relies on." FORCE)
    endif()
endif()

# Release-mode optimization flags. -O3 plus target-native tuning on
# non-Apple platforms (Apple Silicon already gets -O3 -mcpu=apple-m1-ish
# defaults from AppleClang for -O3; explicit -mcpu flags for Apple chips
# are handled by Xcode's toolchain based on CMAKE_OSX_ARCHITECTURES rather
# than -march, so nothing additional is needed here for macOS).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND APPLE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# ---------------------------------------------------------------------------
# onnxruntime
# ---------------------------------------------------------------------------
if(NOT DEFINED ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH
        "Path to an extracted onnxruntime release (containing include/ and lib/)")
endif()

# ONNXRUNTIME_INCLUDE_DIR, when explicitly provided (e.g. by plugin/setup_mac.sh
# after locating onnxruntime_cxx_api.h via `find`, since some distributions such
# as Homebrew's onnxruntime formula nest headers under include/onnxruntime/ or
# deeper rather than directly under include/), takes precedence over the
# ${ONNXRUNTIME_ROOT}/include default below.
if(DEFINED ONNXRUNTIME_INCLUDE_DIR AND NOT "${ONNXRUNTIME_INCLUDE_DIR}" STREQUAL "")
    set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_INCLUDE_DIR}" CACHE PATH
        "Path to the directory directly containing onnxruntime_cxx_api.h" FORCE)
else()
    set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include" CACHE PATH
        "Path to the directory directly containing onnxruntime_cxx_api.h")
endif()

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(WARNING
        "onnxruntime headers not found at ${ONNXRUNTIME_INCLUDE_DIR}. "
        "Set -DONNXRUNTIME_ROOT=/path/to/onnxruntime (extracted release from "
        "https://github.com/microsoft/onnxruntime/releases), or "
        "-DONNXRUNTIME_INCLUDE_DIR=/path/to/dir/containing/onnxruntime_cxx_api.h "
        "directly if your onnxruntime distribution nests headers under a "
        "subdirectory (e.g. Homebrew's include/onnxruntime/). Configure will "
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
find_package(Threads REQUIRED)

add_library(upscale_core STATIC
    src/core/onnx_upscaler.cpp
    src/core/tile.cpp
)
target_include_directories(upscale_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    PRIVATE
        ${ONNXRUNTIME_INCLUDE_DIR}
)
# Threads::Threads links -pthread (required for std::thread on
# Linux/glibc); harmless no-op flag-wise on macOS/Windows.
target_link_libraries(upscale_core PUBLIC ${ONNXRUNTIME_LIB} Threads::Threads)

# Execution-provider feature flags: define these when the corresponding
# onnxruntime EP libraries are present in ONNXRUNTIME_ROOT/lib, so
# onnx_upscaler.cpp can compile in (and try) that provider.
# UPSCALE_WITH_COREML defaults ON on macOS (the primary target platform
# for this plugin, M4 Max) since onnxruntime's official macOS release
# packages include the CoreML EP built-in; the try/catch fallback in
# onnx_upscaler.cpp's append_providers() means this is safe to leave on
# even if a particular onnxruntime build lacks it. CUDA/DirectML remain
# opt-in since they require non-default onnxruntime builds.
option(UPSCALE_WITH_CUDA "Enable CUDA execution provider (requires CUDA-enabled onnxruntime build)" OFF)
option(UPSCALE_WITH_DIRECTML "Enable DirectML execution provider (Windows only)" OFF)
if(APPLE)
    option(UPSCALE_WITH_COREML "Enable CoreML execution provider (macOS only)" ON)
else()
    option(UPSCALE_WITH_COREML "Enable CoreML execution provider (macOS only)" OFF)
endif()

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
if(DEFINED AE_SDK_PATH AND NOT "${AE_SDK_PATH}" STREQUAL "")
    # ---------------------------------------------------------------------
    # Resolve the *effective* SDK root under AE_SDK_PATH.
    #
    # AE_SDK_PATH, as detected/supplied by plugin/setup_mac.sh or passed
    # manually by a user, may point either directly at the SDK root
    # (containing Examples/Headers/AEConfig.h etc.) or at a top-level
    # directory that nests the actual SDK one or more levels deeper -- e.g.
    # Adobe's distribution zip for AE 25.6 unpacks to
    #   <AE_SDK_PATH>/AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK/Examples/Headers/...
    # rather than <AE_SDK_PATH>/Examples/Headers/... directly. Blindly using
    # AE_SDK_PATH as the include root in that case fails to find AEConfig.h
    # even though the SDK is present and fully extracted.
    #
    # This resolution runs unconditionally (not gated on WIN32/APPLE) so it
    # is exercised and verifiable on any platform, including Linux CI, even
    # though the AIUpscale target itself is only added on Windows/macOS
    # below. This is a safety net for users invoking cmake directly with
    # -DAE_SDK_PATH=... instead of via plugin/setup_mac.sh (which already
    # derives and passes the effective root itself).
    set(AE_SDK_EFFECTIVE_ROOT "")

    if(EXISTS "${AE_SDK_PATH}/Examples/Headers/AEConfig.h")
        set(AE_SDK_EFFECTIVE_ROOT "${AE_SDK_PATH}")
    else()
        file(GLOB_RECURSE AE_SDK_CONFIG_HEADER_MATCHES "${AE_SDK_PATH}/AEConfig.h")
        list(LENGTH AE_SDK_CONFIG_HEADER_MATCHES AE_SDK_CONFIG_HEADER_MATCH_COUNT)
        if(AE_SDK_CONFIG_HEADER_MATCH_COUNT GREATER 0)
            list(GET AE_SDK_CONFIG_HEADER_MATCHES 0 AE_SDK_CONFIG_HEADER_FOUND)
            message(STATUS "AEConfig.h not found directly under ${AE_SDK_PATH}/Examples/Headers; "
                "located it via recursive search: ${AE_SDK_CONFIG_HEADER_FOUND}")

            # AE_SDK_CONFIG_HEADER_FOUND looks like
            # <root>/Examples/Headers/AEConfig.h in the common layout, or
            # <root>/Examples/Headers/SDK/AEConfig.h in some SDK versions.
            # Walk up from the header's directory: if it's named "Headers",
            # its grandparent is <root>; if it's nested one level deeper
            # (e.g. "SDK"), its great-grandparent is <root>. Any other
            # nesting falls back to a best-effort 3-levels-up guess matching
            # the common <root>/Examples/Headers/AEConfig.h shape.
            get_filename_component(AE_SDK_HEADER_DIR "${AE_SDK_CONFIG_HEADER_FOUND}" DIRECTORY)
            get_filename_component(AE_SDK_HEADER_DIR_NAME "${AE_SDK_HEADER_DIR}" NAME)
            if(AE_SDK_HEADER_DIR_NAME STREQUAL "Headers")
                get_filename_component(AE_SDK_EXAMPLES_DIR "${AE_SDK_HEADER_DIR}" DIRECTORY)
                get_filename_component(AE_SDK_EFFECTIVE_ROOT "${AE_SDK_EXAMPLES_DIR}" DIRECTORY)
            else()
                get_filename_component(AE_SDK_HEADERS_DIR "${AE_SDK_HEADER_DIR}" DIRECTORY)
                get_filename_component(AE_SDK_EXAMPLES_DIR "${AE_SDK_HEADERS_DIR}" DIRECTORY)
                get_filename_component(AE_SDK_EFFECTIVE_ROOT "${AE_SDK_EXAMPLES_DIR}" DIRECTORY)
            endif()
        endif()
    endif()

    if("${AE_SDK_EFFECTIVE_ROOT}" STREQUAL "")
        message(FATAL_ERROR
            "AE_SDK_PATH was set to '${AE_SDK_PATH}' but AEConfig.h could not be "
            "located anywhere under it (checked ${AE_SDK_PATH}/Examples/Headers/AEConfig.h "
            "directly, then searched recursively under ${AE_SDK_PATH}). Verify the "
            "Adobe SDK zip was fully extracted (see plugin/README.md and the SDK's "
            "own README-HowToBuild-*.txt) and that AE_SDK_PATH points at the SDK's "
            "top-level directory, or a directory containing it.")
    endif()

    if(NOT "${AE_SDK_EFFECTIVE_ROOT}" STREQUAL "${AE_SDK_PATH}")
        message(STATUS "AE_SDK_PATH (${AE_SDK_PATH}) is not itself the SDK root; "
            "using derived effective root instead: ${AE_SDK_EFFECTIVE_ROOT}")
    endif()
    message(STATUS "AE SDK effective root: ${AE_SDK_EFFECTIVE_ROOT}")

    # AE SDK sample plugins conventionally pull headers from
    # Examples/Headers (and, in some SDK versions, nested Examples/Headers/SDK
    # or Examples/Headers/SP subdirectories), Examples/Util, and
    # Examples/Resources (for the .r PiPL). Only add subdirectories that
    # actually exist in this SDK layout.
    set(AE_SDK_INCLUDE_DIRS "")
    foreach(_ae_sdk_subdir
        Examples/Headers
        Examples/Headers/SDK
        Examples/Headers/SP
        Examples/Util
        Examples/Resources
    )
        if(EXISTS "${AE_SDK_EFFECTIVE_ROOT}/${_ae_sdk_subdir}")
            list(APPEND AE_SDK_INCLUDE_DIRS "${AE_SDK_EFFECTIVE_ROOT}/${_ae_sdk_subdir}")
        endif()
    endforeach()
    message(STATUS "AE SDK include directories: ${AE_SDK_INCLUDE_DIRS}")

    # AEGP_SuiteHandler is declared (class + inline members) in
    # Examples/Util/AEGP_SuiteHandler.h but its out-of-line members
    # (constructor, destructor, and the suite accessor bodies) are only
    # DEFINED in the SDK-provided Examples/Util/AEGP_SuiteHandler.cpp --
    # AE SDK sample plugins all compile this .cpp directly into the plugin
    # module rather than linking a prebuilt library for it. Without it,
    # the link fails with undefined symbols for
    # AEGP_SuiteHandler::AEGP_SuiteHandler / ~AEGP_SuiteHandler /
    # MissingSuiteError (the latter also needs a plugin-supplied
    # definition -- see AIUpscale.cpp).
    #
    # This existence check runs unconditionally (not gated on WIN32/APPLE),
    # same rationale as the AE_SDK_EFFECTIVE_ROOT resolution above: it's
    # exercisable on any platform, including this repo's Linux dev
    # environment, even though AEGP_SUITE_HANDLER_SRC is only actually
    # added as a source file to the AIUpscale target further below.
    set(AEGP_SUITE_HANDLER_SRC "${AE_SDK_EFFECTIVE_ROOT}/Examples/Util/AEGP_SuiteHandler.cpp")
    if(NOT EXISTS "${AEGP_SUITE_HANDLER_SRC}")
        message(FATAL_ERROR
            "AEGP_SuiteHandler.cpp not found at ${AEGP_SUITE_HANDLER_SRC}. "
            "The AE SDK provides this file's implementation separately from "
            "its header (Examples/Util/AEGP_SuiteHandler.h); AE SDK sample "
            "plugins compile it directly into the plugin module. Verify the "
            "Adobe SDK zip was fully extracted and AE_SDK_PATH/AE_SDK_EFFECTIVE_ROOT "
            "(${AE_SDK_EFFECTIVE_ROOT}) points at a complete SDK checkout.")
    endif()

    if(WIN32 OR APPLE)
        message(STATUS "AE_SDK_PATH set (${AE_SDK_PATH}); configuring AIUpscale plugin target.")

        add_library(AIUpscale MODULE
            src/plugin/AIUpscale.cpp
            ${AEGP_SUITE_HANDLER_SRC}
        )

        target_include_directories(AIUpscale PRIVATE
            ${AE_SDK_INCLUDE_DIRS}
            ${ONNXRUNTIME_INCLUDE_DIR}
        )
        target_link_libraries(AIUpscale PRIVATE upscale_core Threads::Threads)

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
