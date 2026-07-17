// AIUpscale.h - After Effects / Premiere Pro plugin entry points for the
// "AI Upscale" effect.
//
// This file only compiles when the Adobe After Effects SDK is available
// (see plugin/CMakeLists.txt: the AIUpscale target is only added when
// AE_SDK_PATH is set, and only on Windows/macOS). It is written to be
// structured like the AE SDK's own sample plugins (e.g. Skeleton,
// SDK_Noise) so that once the SDK headers are dropped in place this
// should build with at most minor adjustments for the exact SDK version.
//
// NOT VERIFIED IN THIS ENVIRONMENT: this repo's dev environment is Linux
// without the Adobe SDK, so this file has never actually been compiled
// against real AE_Effect.h/PF headers. Signatures below were written from
// the publicly documented AE SDK API and match the conventions used by
// Adobe's own sample plugins, but should be diffed against the actual SDK
// headers (particularly PF_Cmd values, suite versions, and PF_ParamDef
// layout) the first time this is built for real.
#pragma once

// --- Adobe AE SDK headers (from AE_SDK_PATH/Examples/Headers etc.) --------
#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"
#include "PrSDKAESupport.h" // Premiere Pro interop (BGRA_8u world assumptions, PiPL AE_Effect kind)

#ifdef AE_OS_WIN
    #include <Windows.h>
#endif

#include <memory>
#include <string>

// upscale_core is Adobe-independent and lives outside src/plugin; see
// plugin/src/core/onnx_upscaler.h. It is linked into this target by
// plugin/CMakeLists.txt.
#include "onnx_upscaler.h"

// ---------------------------------------------------------------------------
// Versioning / string table constants (mirrors the "Skeleton"/"SDK_Noise"
// sample layout: a *_Strings.h-like block kept inline here for brevity).
// ---------------------------------------------------------------------------
#define AI_UPSCALE_NAME            "AI Upscale"
#define AI_UPSCALE_CATEGORY        "AI Enhance"
#define AI_UPSCALE_MATCH_NAME      "ADBE AI Upscale" // must be globally unique; replace vendor prefix before shipping
#define AI_UPSCALE_DESCRIPTION     "AI super-resolution upscaling (Real-ESRGAN) for Premiere Pro / After Effects."

#define AI_UPSCALE_MAJOR_VERSION   1
#define AI_UPSCALE_MINOR_VERSION   0
#define AI_UPSCALE_BUG_VERSION     0
#define AI_UPSCALE_STAGE_VERSION   PF_Stage_DEVELOP
#define AI_UPSCALE_BUILD_VERSION   1

// ---------------------------------------------------------------------------
// Parameter indices / IDs (order must match PF_Cmd_PARAMS_SETUP additions).
// ---------------------------------------------------------------------------
enum {
    AI_UPSCALE_INPUT = 0,   // implicit layer input, always index 0
    AI_UPSCALE_SCALE_POPUP, // "Scale": 2x | 4x
    AI_UPSCALE_MODE_POPUP,  // "Mode": Photo | Anime
    AI_UPSCALE_NUM_PARAMS
};

enum {
    SCALE_DISK_ID = 1,
    MODE_DISK_ID,
};

// Popup choices. PF_ADD_POPUP wants a single "|"-delimited string.
#define SCALE_POPUP_CHOICES     "2x|4x"
#define SCALE_POPUP_NUM_CHOICES 2
enum { SCALE_CHOICE_2X = 1, SCALE_CHOICE_4X = 2 }; // PF popups are 1-based

#define MODE_POPUP_CHOICES      "Photo|Anime"
#define MODE_POPUP_NUM_CHOICES  2
enum { MODE_CHOICE_PHOTO = 1, MODE_CHOICE_ANIME = 2 };

// Model filenames expected alongside the plugin binary, under models/.
// See plugin/README.md for how these get there (download_models.py).
#define MODEL_FILENAME_PHOTO "realesrgan-x4plus.onnx"
#define MODEL_FILENAME_ANIME "realesrgan-x4plus-anime.onnx"

// ---------------------------------------------------------------------------
// Per-sequence data: caches the loaded model + Ort::Session-backed
// OnnxUpscaler so we don't reload/re-init onnxruntime on every frame.
// Stored via PF_Handle in seq_data (AE) / opaque sequence data (Premiere),
// following the same pattern as AE SDK samples that cache expensive state
// (e.g. SDK_Invert's sequence data, or the AEGP caching samples).
// ---------------------------------------------------------------------------
struct AIUpscaleSequenceData {
    // Which mode's model is currently loaded, so PF_Cmd_RENDER can detect
    // a mode change and reload lazily instead of reloading every frame.
    int loaded_mode_choice = 0; // 0 = none loaded yet

    // The Adobe-independent inference engine. Heap-allocated behind a
    // unique_ptr so this struct stays POD-ish/handle-friendly; AE/Premiere
    // sequence data is a flat handle, so in the real implementation this
    // pointer must be stored via suites->HandleSuite1()->host_new_handle()
    // and the OnnxUpscaler constructed with placement-new (or, more
    // simply, heap-allocate AIUpscaleSequenceData itself via new/delete
    // and store only the raw pointer in the PF_Handle -- the approach
    // taken here). See AIUpscale.cpp SequenceSetup/SequenceSetdown.
    std::unique_ptr<upscale::OnnxUpscaler> upscaler;

    std::string plugin_dir; // resolved once, used to find models/
};

// ---------------------------------------------------------------------------
// Entry point (declared in PiPL and registered via PF_Main / EffectMain,
// matching the AE SDK's standard single-entry-point dispatch convention).
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

DllExport PF_Err EffectMain(
    PF_Cmd          cmd,
    PF_InData*      in_data,
    PF_OutData*     out_data,
    PF_ParamDef*    params[],
    PF_LayerDef*    output,
    void*           extra);

#ifdef __cplusplus
}
#endif
