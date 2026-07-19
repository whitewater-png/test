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
#include "AEGP_SuiteHandler.h" // AEGP_SuiteHandler(in_data->pica_basicP) -> HandleSuite1() used for sequence-data handle ops
// NOTE: AEFX_ChannelDepthTpl.h and PrSDKAESupport.h were previously included
// here but are unused -- this plugin is 8bpc-only (see README "known
// limitations") and never invokes the ChannelDepth iteration macros
// (which require PF_TABLE_BITS to be defined by the includer) or the
// Premiere-specific interop helpers. Dropped rather than defining
// PF_TABLE_BITS to placate an include we don't need.

#ifdef AE_OS_WIN
    #include <Windows.h>
#endif

#include <memory>
#include <string>

// upscale_core is Adobe-independent and lives outside src/plugin; see
// plugin/src/core/onnx_upscaler.h. It is linked into this target by
// plugin/CMakeLists.txt.
#include "onnx_upscaler.h"
// detail_upscaler.h: the classical (non-neural), clean-room edge-preserving
// upscale engine that is now the DEFAULT "Engine" choice below (see
// ENGINE_POPUP_CHOICES / ENGINE_CHOICE_DETAIL) -- replacing Real-ESRGAN/
// ONNX as the default because that neural engine proved far too slow for
// realtime/render use in Premiere on real hardware (multiple seconds per 4K
// frame). The AI engine remains selectable via the "Engine" popup for users
// who explicitly want it. See detail_upscaler.h for the algorithm itself
// and its clean-room/no-Adobe-code disclaimer.
#include "detail_upscaler.h"

// ---------------------------------------------------------------------------
// Versioning / string table constants (mirrors the "Skeleton"/"SDK_Noise"
// sample layout: a *_Strings.h-like block kept inline here for brevity).
// ---------------------------------------------------------------------------
#define AI_UPSCALE_NAME            "AI Upscale"
#define AI_UPSCALE_CATEGORY        "AI Enhance"
#define AI_UPSCALE_MATCH_NAME      "ADBE AI Upscale" // must be globally unique; replace vendor prefix before shipping
#define AI_UPSCALE_DESCRIPTION     "Detail-preserving upscaling (fast, classical edge-preserving algorithm, default) " \
                                    "with an optional Real-ESRGAN AI engine, for Premiere Pro / After Effects."

#define AI_UPSCALE_MAJOR_VERSION   1
// MINOR bumped 0 -> 1 for this revision: adds the "Engine" popup and
// "Detail" slider params, and changes the default processing engine from
// AI (Real-ESRGAN/ONNX) to the new classical Detail Preserve engine (see
// detail_upscaler.h). AIUpscalePiPL.r's AE_Effect_Version MUST be
// recomputed and kept numerically in sync with this -- see that file's own
// comment for the derivation.
#define AI_UPSCALE_MINOR_VERSION   1
#define AI_UPSCALE_BUG_VERSION     0
#define AI_UPSCALE_STAGE_VERSION   PF_Stage_DEVELOP
#define AI_UPSCALE_BUILD_VERSION   1

// ---------------------------------------------------------------------------
// Parameter indices / IDs (order must match PF_Cmd_PARAMS_SETUP additions).
// ---------------------------------------------------------------------------
enum {
    AI_UPSCALE_INPUT = 0,    // implicit layer input, always index 0
    AI_UPSCALE_ENGINE_POPUP, // "Engine": Detail Preserve (Fast) | AI Real-ESRGAN
    AI_UPSCALE_SCALE_POPUP,  // "Scale": 2x | 4x
    AI_UPSCALE_MODE_POPUP,   // "Mode": Photo | Anime (AI engine only)
    AI_UPSCALE_DETAIL_SLIDER, // "Detail": 0..100 (Detail Preserve engine only)
    AI_UPSCALE_NUM_PARAMS
};

enum {
    ENGINE_DISK_ID = 1,
    SCALE_DISK_ID,
    MODE_DISK_ID,
    DETAIL_DISK_ID,
};

// Popup choices. PF_ADD_POPUP wants a single "|"-delimited string.
#define ENGINE_POPUP_CHOICES     "Detail Preserve (Fast)|AI Real-ESRGAN"
#define ENGINE_POPUP_NUM_CHOICES 2
enum { ENGINE_CHOICE_DETAIL = 1, ENGINE_CHOICE_AI = 2 }; // PF popups are 1-based; Detail Preserve is the default

#define SCALE_POPUP_CHOICES     "2x|4x"
#define SCALE_POPUP_NUM_CHOICES 2
enum { SCALE_CHOICE_2X = 1, SCALE_CHOICE_4X = 2 }; // PF popups are 1-based

#define MODE_POPUP_CHOICES      "Photo|Anime"
#define MODE_POPUP_NUM_CHOICES  2
enum { MODE_CHOICE_PHOTO = 1, MODE_CHOICE_ANIME = 2 };

// "Detail" float slider: 0 (pure Lanczos base resize / identity, no
// sharpening) .. 100 (this implementation's maximum strength), default 50.
// Only meaningful when Engine == Detail Preserve; ignored (but still shown,
// per the task's params-count/order stability requirement) when Engine ==
// AI Real-ESRGAN -- see plugin/README.md.
#define DETAIL_SLIDER_MIN     0.0
#define DETAIL_SLIDER_MAX     100.0
#define DETAIL_SLIDER_DEFAULT 50.0

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

    // The Adobe-independent inference engine. shared_ptr, NOT unique_ptr:
    // ensure_model_loaded() (AIUpscale.cpp) populates this via
    // upscale::OnnxUpscaler::get_shared(), which hands back a process-wide
    // shared instance keyed on model path rather than a private one owned
    // by this sequence data alone. This matters because the render-time
    // self-healing path (ensure_sequence_data(), below) can end up
    // creating more than one AIUpscaleSequenceData for what is really the
    // same render session (one per render thread that ever observed a
    // null sequence_data) -- without sharing, each would load its own
    // ~64MB CoreML/ANE session, multiplying memory use and ANE/GPU
    // contention. With get_shared(), every AIUpscaleSequenceData loading
    // the same model_path ends up pointing at the SAME Ort::Session; this
    // struct (and this shared_ptr) still gets heap-allocated via plain
    // new/delete and its raw AIUpscaleSequenceData* stored inside a small
    // (sizeof(void*)) PF_Handle obtained from
    // AEGP_SuiteHandler(in_data->pica_basicP).HandleSuite1(), same as
    // before -- see AIUpscale.cpp HandleSequenceSetup/HandleSequenceSetdown.
    std::shared_ptr<upscale::OnnxUpscaler> upscaler;

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
