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
    src/core/concurrency.cpp
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
# test_resample (upscale_core unit test, Linux/CI-runnable)
#
# Standalone smoke test for upscale::resize_rgba_bilinear() (src/core/tile.cpp),
# added alongside the fix that makes HandleRender (src/plugin/AIUpscale.cpp)
# resample the model's native-scale output to whatever exact size the AE/
# Premiere host actually allocated for the output world, instead of assuming
# output size == input size * scale. upscale_core has no Adobe SDK
# dependency, so this is buildable/runnable on this Linux dev environment
# without AE_SDK_PATH -- see tests/test_resample.cpp.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_resample
    tests/test_resample.cpp
)
target_link_libraries(test_resample PRIVATE upscale_core)
add_test(NAME test_resample COMMAND test_resample)

# ---------------------------------------------------------------------------
# test_tile_output_scale (upscale_core unit test, Linux/CI-runnable)
#
# Regression test for TileOptions::output_scale (src/core/tile.h/.cpp),
# added while fixing the third real-Premiere-hardware render failure: a
# host-handed input world far larger than the nominal source resolution
# (4892x8192 vs 3840x2160) combined with this plugin's own native 4x model
# upscale produced a 641,204,224px intermediate that blew every size safety
# margin. See tests/test_tile_output_scale.cpp for the full scenario this
# exercises, including the exact field-reported dimensions.
# ---------------------------------------------------------------------------
add_executable(test_tile_output_scale
    tests/test_tile_output_scale.cpp
)
target_link_libraries(test_tile_output_scale PRIVATE upscale_core)
add_test(NAME test_tile_output_scale COMMAND test_tile_output_scale)

# ---------------------------------------------------------------------------
# test_concurrency_gate (upscale_core unit test, Linux/CI-runnable)
#
# Unit test for upscale::ConcurrencyGate (src/core/concurrency.h/.cpp),
# added to fix a real-Premiere-hardware freeze on a MacBook Pro M4 Max
# (36GB): field logs showed dozens of concurrent Premiere render threads
# each spinning up their own internal tile-processing worker pool
# (host_threads * tile_workers reaching ~200 threads), saturating every CPU
# core at once. No onnxruntime/model dependency -- pure threading test, see
# tests/test_concurrency_gate.cpp.
# ---------------------------------------------------------------------------
add_executable(test_concurrency_gate
    tests/test_concurrency_gate.cpp
)
target_link_libraries(test_concurrency_gate PRIVATE upscale_core)
add_test(NAME test_concurrency_gate COMMAND test_concurrency_gate)

# ---------------------------------------------------------------------------
# test_onnx_upscaler_sharing (upscale_core unit test, Linux/CI-runnable)
#
# Regression test for OnnxUpscaler::get_shared() (src/core/onnx_upscaler.h/
# .cpp), the process-wide shared-session cache added alongside
# ConcurrencyGate to fix the same real-Premiere-hardware freeze: the AE/
# Premiere plugin's render-time sequence-data self-healing could create
# more than one OnnxUpscaler (and thus more than one ~64MB CoreML/ANE
# Ort::Session) for what was really the same render session. See
# tests/test_onnx_upscaler_sharing.cpp.
#
# Needs a real (tiny, non-ML) ONNX model to actually call load()/upscale()
# against -- generated at build time via scripts/make_test_model.py when a
# Python 3 interpreter with the `onnx` package is available. If not
# available, the generated model file won't exist and the test itself
# detects that at runtime and skips (exits 0) rather than failing the
# build or ctest run over a missing optional dev dependency.
# ---------------------------------------------------------------------------
find_program(PYTHON3_EXECUTABLE NAMES python3)
set(TEST_MODEL_4X_PATH "${CMAKE_CURRENT_BINARY_DIR}/test_model_4x.onnx")
if(PYTHON3_EXECUTABLE)
    add_custom_command(
        OUTPUT "${TEST_MODEL_4X_PATH}"
        COMMAND ${PYTHON3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_test_model.py" "${TEST_MODEL_4X_PATH}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_test_model.py"
        COMMENT "Generating tiny test ONNX model (test_model_4x.onnx) for test_onnx_upscaler_sharing"
    )
    add_custom_target(generate_test_model_4x DEPENDS "${TEST_MODEL_4X_PATH}")
else()
    message(STATUS "python3 not found; test_onnx_upscaler_sharing will skip at runtime (no test model generated).")
endif()

add_executable(test_onnx_upscaler_sharing
    tests/test_onnx_upscaler_sharing.cpp
)
target_link_libraries(test_onnx_upscaler_sharing PRIVATE upscale_core)
target_compile_definitions(test_onnx_upscaler_sharing PRIVATE
    TEST_MODEL_4X_PATH="${TEST_MODEL_4X_PATH}"
)
if(PYTHON3_EXECUTABLE)
    add_dependencies(test_onnx_upscaler_sharing generate_test_model_4x)
endif()
add_test(NAME test_onnx_upscaler_sharing COMMAND test_onnx_upscaler_sharing)

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
            # This sets up the bundle shape; Info.plist comes from
            # src/plugin/Info.plist.in below. The PiPL resource itself is
            # compiled and embedded by the custom commands further below.
            set_target_properties(AIUpscale PROPERTIES
                BUNDLE TRUE
                BUNDLE_EXTENSION "plugin"
                OUTPUT_NAME "AIUpscale"
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/Info.plist.in"
            )

            # -----------------------------------------------------------
            # PiPL (Plug-in Property List) resource compilation.
            #
            # This is the step that was previously missing entirely: AE SDK
            # plugins are only recognized by After Effects/Premiere Pro if
            # a compiled PiPL resource is present at
            # Contents/Resources/<executable-name>.rsrc inside the bundle.
            # A bundle without it builds, installs, and even loads as a
            # valid Mach-O module, but the host's plugin scanner silently
            # skips it -- it never appears in the Effects panel. This
            # reproduces the two-step "Compile PiPL" build phase used by
            # the AE SDK's own Xcode sample projects (e.g.
            # Examples/Skeleton):
            #   1. Preprocess AIUpscalePiPL.r with the C preprocessor (it
            #      uses #include/#ifdef against AEConfig.h,
            #      AE_EffectVers.h, AE_General.r) into a plain .rr file
            #      Rez can consume directly.
            #   2. Compile that .rr with Rez into Contents/Resources/
            #      <executable-name>.rsrc -- the AE/Premiere plugin loader
            #      looks for a resource file whose name matches the
            #      executable (OUTPUT_NAME "AIUpscale" above), hence
            #      AIUpscale.rsrc.
            #
            # Rez ships with Xcode (Contents/Developer/Tools) and, on some
            # Xcode/CLT versions, with the standalone Command Line Tools
            # package too. It is intentionally NOT required for the
            # upscale_core/upscale_cli targets or for Linux configure --
            # this whole block only runs once cmake has already decided to
            # build the macOS AIUpscale bundle target above.
            find_program(REZ_EXECUTABLE NAMES Rez
                PATHS
                    /Applications/Xcode.app/Contents/Developer/Tools
                    /Library/Developer/CommandLineTools/usr/bin
            )
            if(NOT REZ_EXECUTABLE)
                # Rez is frequently not on PATH even when installed; xcrun
                # knows how to resolve tools bundled with the active
                # Xcode/Command Line Tools selection (xcode-select -p).
                execute_process(
                    COMMAND xcrun -f Rez
                    OUTPUT_VARIABLE REZ_EXECUTABLE_XCRUN
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE REZ_XCRUN_RESULT
                )
                if(REZ_XCRUN_RESULT EQUAL 0 AND EXISTS "${REZ_EXECUTABLE_XCRUN}")
                    set(REZ_EXECUTABLE "${REZ_EXECUTABLE_XCRUN}" CACHE FILEPATH "Path to Rez" FORCE)
                endif()
            endif()
            if(NOT REZ_EXECUTABLE)
                message(FATAL_ERROR
                    "Rez (the classic Mac OS resource compiler, required to "
                    "compile AIUpscalePiPL.r into the plugin bundle's PiPL "
                    "resource) was not found via PATH, the usual Xcode/CLT "
                    "install locations, or `xcrun -f Rez`. Without it the "
                    "AIUpscale.plugin bundle will build successfully but "
                    "After Effects/Premiere Pro will NOT show the 'AI "
                    "Upscale' effect at all (this is the exact symptom "
                    "reported after a build lacking this step). To fix: "
                    "install Xcode and/or run `xcode-select --install`, "
                    "make sure a full Xcode.app is the active developer "
                    "directory if the standalone Command Line Tools "
                    "package doesn't include Rez on this macOS/Xcode "
                    "version (`xcode-select -s "
                    "/Applications/Xcode.app/Contents/Developer`), then "
                    "re-run cmake configure.")
            endif()
            message(STATUS "Rez found for PiPL compilation: ${REZ_EXECUTABLE}")

            set(AIUPSCALE_PIPL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/AIUpscalePiPL.r")
            set(AIUPSCALE_PIPL_PREPROCESSED "${CMAKE_CURRENT_BINARY_DIR}/AIUpscalePiPL.rr")

            # -E -P: run the preprocessor only and suppress '# <n> "file"'
            # line-marker directives (Rez's resource-script parser doesn't
            # understand them). -x c forces C-preprocessor handling
            # regardless of the .r extension, matching the AE SDK sample
            # projects' build phase. Deliberately CMAKE_CXX_COMPILER, not
            # CMAKE_C_COMPILER: this project only calls project(... CXX) --
            # no C language is enabled -- so CMAKE_C_COMPILER is never
            # detected/set and would silently expand to an empty string
            # here (verified: doing so makes the COMMAND's first token be
            # "-E" itself, which gmake reports as "E: No such file or
            # directory" -- caught by simulating this exact custom command
            # on Linux with APPLE forced on and a stub Rez on PATH).
            # CMAKE_CXX_COMPILER's underlying clang++/g++ driver accepts
            # -x c to switch to C-preprocessor rules for this invocation,
            # so it works as a drop-in preprocessor without requiring
            # enable_language(C).
            add_custom_command(
                OUTPUT "${AIUPSCALE_PIPL_PREPROCESSED}"
                COMMAND ${CMAKE_CXX_COMPILER} -E -P -x c
                        -I "${AE_SDK_EFFECTIVE_ROOT}/Examples/Headers"
                        -I "${AE_SDK_EFFECTIVE_ROOT}/Examples/Resources"
                        "${AIUPSCALE_PIPL_SRC}"
                        -o "${AIUPSCALE_PIPL_PREPROCESSED}"
                DEPENDS "${AIUPSCALE_PIPL_SRC}"
                COMMENT "Preprocessing AIUpscalePiPL.r -> AIUpscalePiPL.rr"
                VERBATIM
            )
            add_custom_target(AIUpscale_PiPL_Preprocess
                DEPENDS "${AIUPSCALE_PIPL_PREPROCESSED}"
            )
            add_dependencies(AIUpscale AIUpscale_PiPL_Preprocess)

            # POST_BUILD so $<TARGET_BUNDLE_CONTENT_DIR:AIUpscale> resolves
            # to an already-created Contents/ directory (CMake lays out the
            # bundle skeleton as part of linking the MODULE BUNDLE target).
            # -useDF tells Rez to write resources into the resource fork's
            # data-fork-based ('.rsrc' as a plain data file) representation,
            # the form modern PiPL-consuming AE/Premiere hosts expect
            # rather than a legacy HFS+ resource fork.
            add_custom_command(TARGET AIUpscale POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_BUNDLE_CONTENT_DIR:AIUpscale>/Resources"
                COMMAND "${REZ_EXECUTABLE}"
                        -o "$<TARGET_BUNDLE_CONTENT_DIR:AIUpscale>/Resources/$<TARGET_FILE_BASE_NAME:AIUpscale>.rsrc"
                        -useDF
                        "${AIUPSCALE_PIPL_PREPROCESSED}"
                        -i "${AE_SDK_EFFECTIVE_ROOT}/Examples/Headers"
                        -i "${AE_SDK_EFFECTIVE_ROOT}/Examples/Resources"
                COMMENT "Compiling AIUpscalePiPL.rr with Rez -> Contents/Resources/AIUpscale.rsrc"
                VERBATIM
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
