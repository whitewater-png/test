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
#include "AEGP_SuiteHandler.h" // AEGP_SuiteHandler(in_data->pica_basicP) -> HandleSuite1() used for AE-suite error reporting
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

// detail_upscaler.h: the classical (non-neural), clean-room edge-preserving
// upscale engine, from plugin/src/core (Adobe-independent, linked into this
// target via the upscale_detail_core library -- see plugin/CMakeLists.txt).
//
// AI ENGINE REMOVED: this plugin previously also offered a Real-ESRGAN/ONNX
// "AI Real-ESRGAN" engine selectable via an "Engine" popup. That engine has
// been removed entirely from this Premiere/AE-facing plugin: on real
// hardware it was far too slow for interactive/render use (multiple seconds
// per 4K frame) and, worse, an accidental engine selection turned into a
// machine freeze (see plugin/README.md "安定運用ガイド" history / git log for
// the concurrency-gate/tile-serialization saga that was needed just to keep
// it from taking down the whole machine). Given that this plugin's own
// render model is same-resolution-only (see the buffer-expansion-retraction
// comment in AIUpscale.cpp), the neural engine's supposed benefit --
// higher-fidelity super-resolution -- was never actually realized here
// anyway, so removing it is a straightforward simplification, not a
// regression. This plugin now ONLY supports the Detail Preserve engine (a
// single "Detail" slider param, no popups at all).
//
// The AI (Real-ESRGAN/ONNX) engine is NOT removed from the rest of the
// repo: plugin/src/cli/upscale_cli.cpp and plugin/scripts/upscale_video.sh
// still support it for pre-conversion batch workflows, where a slow,
// high-quality one-time pass is exactly the right tradeoff (see
// plugin/README.md's "推奨ワークフロー: 素材の事前アップスケール").
#include "detail_upscaler.h"

// ---------------------------------------------------------------------------
// Versioning / string table constants (mirrors the "Skeleton"/"SDK_Noise"
// sample layout: a *_Strings.h-like block kept inline here for brevity).
// ---------------------------------------------------------------------------
#define AI_UPSCALE_NAME            "AI Upscale"
#define AI_UPSCALE_CATEGORY        "AI Enhance"
#define AI_UPSCALE_MATCH_NAME      "ADBE AI Upscale" // must be globally unique; replace vendor prefix before shipping
#define AI_UPSCALE_DESCRIPTION     "Detail-preserving upscaling (fast, classical edge-preserving algorithm) " \
                                    "for Premiere Pro / After Effects."

#define AI_UPSCALE_MAJOR_VERSION   1
// MINOR bumped 1 -> 2 for this revision: the "Engine"/"Scale"/"Mode" popups
// and the AI (Real-ESRGAN/ONNX) engine path are removed entirely from this
// plugin -- it now ONLY has the "Detail" slider param and always runs the
// Detail Preserve engine (see detail_upscaler.h). AIUpscalePiPL.r's
// AE_Effect_Version MUST be recomputed and kept numerically in sync with
// this -- see that file's own comment for the derivation.
#define AI_UPSCALE_MINOR_VERSION   2
#define AI_UPSCALE_BUG_VERSION     0
#define AI_UPSCALE_STAGE_VERSION   PF_Stage_DEVELOP
#define AI_UPSCALE_BUILD_VERSION   1

// ---------------------------------------------------------------------------
// Parameter indices / IDs (order must match PF_Cmd_PARAMS_SETUP additions).
// ---------------------------------------------------------------------------
enum {
    AI_UPSCALE_INPUT = 0,    // implicit layer input, always index 0
    AI_UPSCALE_DETAIL_SLIDER, // "Detail": 0..100, the only user-facing param
    AI_UPSCALE_NUM_PARAMS
};

enum {
    DETAIL_DISK_ID = 1,
};

// "Detail" float slider: 0 (pure Lanczos base resize / identity, no
// sharpening) .. 100 (this implementation's maximum strength), default 50.
#define DETAIL_SLIDER_MIN     0.0
#define DETAIL_SLIDER_MAX     100.0
#define DETAIL_SLIDER_DEFAULT 50.0

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
