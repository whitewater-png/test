// AIUpscale.cpp - "AI Upscale" After Effects / Premiere Pro effect plugin.
//
// See AIUpscale.h for the "not verified in this environment" caveat: this
// file cannot be compiled without the Adobe AE SDK, which is not present
// in this Linux dev environment. It is written to match the AE SDK
// sample-plugin structure (PF_Cmd dispatch switch in EffectMain calling
// one handler per command) so it should need at most small adjustments
// once built against the real SDK headers.
//
// AI (Real-ESRGAN/ONNX) ENGINE REMOVED from this file entirely (see
// AIUpscale.h's file-header comment for the rationale): this plugin now
// only ever runs the classical Detail Preserve engine
// (upscale::detail_preserving_upscale(), src/core/detail_upscaler.h/.cpp).
// There is no "Engine"/"Scale"/"Mode" param anymore, no OnnxUpscaler, no
// ConcurrencyGate, no model file resolution/loading, and (since none of
// that needs any per-sequence cached state) no per-sequence data at all --
// PF_Cmd_SEQUENCE_SETUP/_RESETUP/_SETDOWN are simply left unhandled
// (default: break in EffectMain's switch), matching what AE/Premiere SDK
// samples do for effects with no cached state to carry between frames.
//
// Buffer-expansion RETRACTED (third real-Premiere-hardware render-failure
// fix; see README.md "既知の制約" for the user-facing writeup): earlier
// revisions of this file had PF_Cmd_FRAME_SETUP grow out_data->width/height
// by the requested scale via PF_OutFlag_I_EXPAND_BUFFER, the same mechanism
// AE blur-type effects use to grow their output rect. Real Premiere Pro
// hardware logs proved this actively harmful rather than merely
// unsupported: Premiere handed HandleRender an input world (params[...]->
// u.ld) that was ALREADY larger than the layer's nominal full-res size
// (e.g. input_world=4892x8192 against in_data(full-res)=3840x2160) --
// i.e. Premiere had already grown the buffer once in response to the
// expansion request from a previous pass -- and then this plugin's model
// applied its OWN native 4x on top of that already-expanded input,
// producing a 19568x32768 (641,204,224px) intermediate that blew through
// every size-safety margin (see size_limits.h) and crashed the render
// with PF_Err_INTERNAL_STRUCT_DAMAGED (512). Since a standard AE/Premiere
// filter effect cannot legitimately hand a higher-resolution buffer
// downstream to a later Transform/Motion scale in the effects chain (host
// constraint, not a bug in this plugin), and I_EXPAND_BUFFER does not
// behave reliably here, this effect now ALWAYS keeps the output world at
// input size and works as a same-resolution detail-regeneration /
// sharpening filter instead: apply it, then scale up afterward (Transform/
// Motion) for a punched-in look. See README.md for the full user-facing
// workflow writeup, including the honest caveat that this does NOT
// increase the layer's actual pixel resolution, and the alternative
// upscale_cli/upscale_video.sh-based pre-processing workflow (which still
// supports the AI engine) for users who need that.
#include "AIUpscale.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "logger.h"
#include "size_limits.h"

// ---------------------------------------------------------------------------
// AEGP_SuiteHandler::MissingSuiteError()
//
// AE SDK規約: Examples/Util/AEGP_SuiteHandler.cpp (SDK同梱、plugin/CMakeLists.txt
// が AIUpscale ターゲットのソースに追加する) は AEGP_SuiteHandler の
// コンストラクタ/デストラクタ/各 *Suite*() アクセサを定義するが、
// MissingSuiteError() だけは意図的に未実装のまま提供されており、各プラグイン
// が自前で定義する規約になっている（呼び出し元のエラー通知方法がホストAPIの
// 種類ごとに異なるため）。ここでは例外を投げる実装とし、EffectMain (下記) の
// try/catch 例外境界で捕捉されて PF_Err_INTERNAL_STRUCT_DAMAGED 相当の
// PF_Err に変換される設計とする。クラス外定義のため、名前空間で囲わず
// グローバルスコープに置く。
void AEGP_SuiteHandler::MissingSuiteError() const {
    throw std::runtime_error("AIUpscale: required AE suite is missing");
}

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The AE SDK's PF_STRCPY/PF_SPRINTF macros (AE_EffectCB.h) expand to code
// that implicitly references a variable named `in_data` in the caller's
// scope (e.g. `(*in_data->utils->ansi.strcpy)(...)`), which breaks in any
// function that doesn't happen to have an in_data parameter with that exact
// name. PF_SPRINTF is also effectively an unbounded sprintf into
// out_data->return_msg (char[PF_MAX_EFFECT_MSG_LEN + 1]). To avoid
// depending on a spelling-sensitive macro and to get a length-bounded,
// NUL-terminated write, every return_msg assignment in this file goes
// through this helper instead.
#if defined(__GNUC__) || defined(__clang__)
void set_return_msg(PF_OutData* out_data, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
void set_return_msg(PF_OutData* out_data, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out_data->return_msg, sizeof(out_data->return_msg), fmt, args);
    va_end(args);
}

// Converts a PF_EffectWorld (assumed BGRA_8u, see header caveat) into the
// Adobe-independent upscale::ImageRGBA8 the core engine expects (which is
// RGBA-ordered). Channel order is swapped per-pixel.
upscale::ImageRGBA8 world_to_rgba8(const PF_EffectWorld* world) {
    upscale::ImageRGBA8 img;
    const int w = static_cast<int>(world->width);
    const int h = static_cast<int>(world->height);
    img.resize(w, h);

    const PF_Pixel8* row_base = reinterpret_cast<const PF_Pixel8*>(world->data);
    const A_long row_bytes = world->rowbytes;

    for (int y = 0; y < h; ++y) {
        const PF_Pixel8* src = reinterpret_cast<const PF_Pixel8*>(
            reinterpret_cast<const char*>(row_base) + static_cast<size_t>(y) * row_bytes);
        uint8_t* dst = &img.pixels[static_cast<size_t>(y) * w * 4];
        for (int x = 0; x < w; ++x) {
            // PF_Pixel8 is {alpha, red, green, blue} in AE's native ARGB
            // ordering; Premiere hosts typically hand BGRA_8u (see
            // PF_WORLD_IS_DEEP / PF_Pixel_BGRA_8u in PrSDKAESupport.h).
            // This function assumes the BGRA_8u interpretation per the
            // task's stated 8bpc/BGRA_8u precondition -- if compiling
            // against classic AE ARGB worlds instead, swap the indexing
            // below accordingly (flagged as a TODO to verify against the
            // real world's PF_PixelFormat at PF_Cmd_RENDER time).
            const uint8_t* p = reinterpret_cast<const uint8_t*>(src + x);
            dst[x * 4 + 0] = p[2]; // R  (B G R A memory order for BGRA_8u)
            dst[x * 4 + 1] = p[1]; // G
            dst[x * 4 + 2] = p[0]; // B
            dst[x * 4 + 3] = p[3]; // A
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// Render-time diagnostic logging (throttled).
//
// Field hardware showed the render failure "output pixel count 641204224
// exceeds safety limit of 200000000" coming from many concurrent render
// threads for a 4K clip on a 4K sequence -- a ~4.8x-too-large figure versus
// the expected UHD 4x (132,710,400 px), pointing at a host/plugin
// disagreement about frame sizes rather than a genuinely oversized request.
// Logging every size on every frame/tile-thread would be spam (the report
// showed many threads logging per frame); this cache logs only when the
// (input size, output size, downsample) combination actually changes, so a
// size-mismatch pattern is visible in AIUpscale.log without drowning it in
// repeats.
// ---------------------------------------------------------------------------
struct RenderDiagCache {
    std::mutex mutex;
    int64_t last_in_w = -1, last_in_h = -1;
    int64_t last_out_w = -1, last_out_h = -1;
    int64_t last_full_w = -1, last_full_h = -1;
    int64_t last_dsx_num = -1, last_dsx_den = -1;
    int64_t last_dsy_num = -1, last_dsy_den = -1;
};
RenderDiagCache g_render_diag_cache;

// Classifies the input/output world size relationship now that
// PF_OutFlag_I_EXPAND_BUFFER has been retracted (see the file-header
// comment): "detail-regen" is the expected/healthy case (output world ==
// input world -- this effect never asks the host to grow the buffer
// anymore), "custom" is anything else (e.g. a host-imposed tile/band size
// difference some hosts use internally), which HandleRender already
// handles correctly via its own resample-to-match-output-world-size step
// below but which is still worth surfacing in the log as a distinct case.
// Single word by design so it's easy to grep/scan in AIUpscale.log across a
// user's next report. (This used to also classify an "expanded" case for
// the retracted I_EXPAND_BUFFER behavior; that case is no longer possible
// since HandleFrameSetup never requests expansion.)
const char* classify_expand_status(int64_t in_w, int64_t in_h, int64_t out_w, int64_t out_h) {
    if (out_w == in_w && out_h == in_h) {
        return "detail-regen";
    }
    return "custom";
}

void log_render_diag_if_changed(PF_InData* in_data, const PF_EffectWorld* input_world,
                                 const PF_LayerDef* output) {
    const int64_t in_w = input_world->width, in_h = input_world->height;
    const int64_t out_w = output->width, out_h = output->height;
    const int64_t full_w = in_data->width, full_h = in_data->height;
    const int64_t dsx_num = in_data->downsample_x.num, dsx_den = in_data->downsample_x.den;
    const int64_t dsy_num = in_data->downsample_y.num, dsy_den = in_data->downsample_y.den;

    std::lock_guard<std::mutex> lock(g_render_diag_cache.mutex);
    RenderDiagCache& c = g_render_diag_cache;
    if (in_w == c.last_in_w && in_h == c.last_in_h && out_w == c.last_out_w && out_h == c.last_out_h &&
        full_w == c.last_full_w && full_h == c.last_full_h && dsx_num == c.last_dsx_num &&
        dsx_den == c.last_dsx_den && dsy_num == c.last_dsy_num && dsy_den == c.last_dsy_den) {
        return; // identical to last-logged combination, skip (avoid per-frame/per-thread spam)
    }
    c.last_in_w = in_w; c.last_in_h = in_h;
    c.last_out_w = out_w; c.last_out_h = out_h;
    c.last_full_w = full_w; c.last_full_h = full_h;
    c.last_dsx_num = dsx_num; c.last_dsx_den = dsx_den;
    c.last_dsy_num = dsy_num; c.last_dsy_den = dsy_den;

    const char* expand_status = classify_expand_status(in_w, in_h, out_w, out_h);

    std::ostringstream oss;
    oss << "HandleRender: size combo changed - input_world=" << in_w << "x" << in_h
        << " output_world=" << out_w << "x" << out_h
        << " in_data(full-res)=" << full_w << "x" << full_h
        << " downsample_x=" << dsx_num << "/" << dsx_den
        << " downsample_y=" << dsy_num << "/" << dsy_den
        << " expand_status=" << expand_status;
    upscale::log_info(oss.str());
}

void rgba8_to_world(const upscale::ImageRGBA8& img, PF_EffectWorld* world) {
    const int w = std::min(img.width, static_cast<int>(world->width));
    const int h = std::min(img.height, static_cast<int>(world->height));
    const A_long row_bytes = world->rowbytes;

    for (int y = 0; y < h; ++y) {
        uint8_t* dst = reinterpret_cast<uint8_t*>(
            reinterpret_cast<char*>(world->data) + static_cast<size_t>(y) * row_bytes);
        const uint8_t* src = &img.pixels[static_cast<size_t>(y) * img.width * 4];
        for (int x = 0; x < w; ++x) {
            uint8_t* p = dst + x * 4;
            p[0] = src[x * 4 + 2]; // B
            p[1] = src[x * 4 + 1]; // G
            p[2] = src[x * 4 + 0]; // R
            p[3] = src[x * 4 + 3]; // A
        }
    }
}

// ---------------------------------------------------------------------------
// PF_Cmd handlers
// ---------------------------------------------------------------------------

// AE_Effect_Global_OutFlags / _2 in AIUpscalePiPL.r MUST stay numerically
// identical to the bitmask HandleGlobalSetup() below sets on
// out_data->out_flags / out_flags2 -- AE/Premiere cross-checks the PiPL's
// declared flags against what PF_Cmd_GLOBAL_SETUP actually reports and
// will warn or refuse to load the effect on a mismatch. This repo's dev
// environment has no real AE_Effect.h (see AIUpscale.h's "NOT VERIFIED IN
// THIS ENVIRONMENT" caveat), so the exact bit positions below cannot be
// checked against the real header here; these static_asserts are a
// build-time tripwire so the first real SDK build fails loudly (instead
// of silently loading with mismatched flags) if either assumption is
// wrong. If any of these ever fail, or if the out_flags expression below
// changes, recompute AIUpscalePiPL.r's AE_Effect_Global_OutFlags to match.
static_assert(PF_OutFlag_NON_PARAM_VARY == (1L << 2),
              "PF_OutFlag_NON_PARAM_VARY bit position changed -- update AIUpscalePiPL.r's "
              "AE_Effect_Global_OutFlags to match out_data->out_flags");
static_assert(PF_OutFlag_DISPLAY_ERROR_MESSAGE == (1L << 8),
              "PF_OutFlag_DISPLAY_ERROR_MESSAGE bit position changed -- update AIUpscalePiPL.r's "
              "AE_Effect_Global_OutFlags to match out_data->out_flags");
// PF_OutFlag_I_EXPAND_BUFFER is intentionally NOT set on out_data->out_flags
// (see the buffer-expansion-retraction comment at the top of this file) and
// so has no static_assert here -- there is nothing on the PiPL side left to
// keep numerically in sync for it.

PF_Err HandleAbout(PF_InData* in_data, PF_OutData* out_data) {
    set_return_msg(out_data,
        "%s v%d.%d\r%s\rEngine: Detail Preserve (fast, classical edge-preserving upscale, own "
        "implementation, no AI model). For AI (Real-ESRGAN) pre-conversion, see upscale_video.sh.",
        AI_UPSCALE_NAME, AI_UPSCALE_MAJOR_VERSION, AI_UPSCALE_MINOR_VERSION, AI_UPSCALE_DESCRIPTION);
    return PF_Err_NONE;
}

PF_Err HandleGlobalSetup(PF_InData* in_data, PF_OutData* out_data) {
    out_data->my_version = PF_VERSION(
        AI_UPSCALE_MAJOR_VERSION, AI_UPSCALE_MINOR_VERSION, AI_UPSCALE_BUG_VERSION,
        AI_UPSCALE_STAGE_VERSION, AI_UPSCALE_BUILD_VERSION);

    // PF_OutFlag_DEEP_COLOR_AWARE intentionally NOT set: 8bpc only for
    // now (see README "known limitations"). PF_OutFlag_SEQUENCE_DATA_NEEDS_FLATTENING
    // omitted since this plugin has no sequence data at all anymore (no
    // model/engine state to cache -- see file-header comment).
    //
    // PF_OutFlag_I_EXPAND_BUFFER: DELIBERATELY NOT SET (retracted -- see
    // the large comment at the top of this file). It used to be declared
    // here to authorize HandleFrameSetup() growing out_data->width/height
    // beyond the input size, on the theory that a missing declaration was
    // the cause of a real-hardware PF_Err_INTERNAL_STRUCT_DAMAGED (512).
    // That theory was wrong: real Premiere Pro hardware logs from a
    // subsequent attempt (with the flag correctly declared) showed
    // Premiere DID honor/propagate the expansion request across passes,
    // compounding with this plugin's own native upscale into a
    // 641,204,224px intermediate that blew every size safety margin. This
    // effect now always keeps the output world at input size (same-
    // resolution detail regeneration), so there is no expansion left to
    // authorize.
    // PF_OutFlag_DISPLAY_ERROR_MESSAGE: makes the host actually surface
    // out_data->return_msg (set via set_return_msg() throughout this file)
    // in its error dialog instead of silently swallowing it -- added so
    // future failures are diagnosable from the host UI directly rather
    // than only from the log file.
    // NUMERIC MATCH REQUIRED WITH AIUpscalePiPL.r's AE_Effect_Global_OutFlags:
    // see the static_asserts above HandleAbout() and AIUpscalePiPL.r's own
    // comment -- update BOTH sides together if this expression changes.
    out_data->out_flags = PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_DISPLAY_ERROR_MESSAGE;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE * 0 | PF_OutFlag2_SUPPORTS_SMART_RENDER * 0;
    // NOTE: SmartFX/SmartRender support is a documented roadmap item (see
    // README) -- left disabled here since it requires a larger rewrite
    // (PF_Cmd_SMART_PRE_RENDER / PF_Cmd_SMART_RENDER) not implemented in
    // this pass.
    return PF_Err_NONE;
}

PF_Err HandleParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    // "Detail": 0..100 strength for the Detail Preserve engine's edge-
    // adaptive unsharp mask (see detail_upscaler.h). This is now the ONLY
    // param this effect has -- the former "Engine"/"Scale"/"Mode" popups
    // and the AI (Real-ESRGAN/ONNX) engine they selected have been removed
    // entirely (see AIUpscale.h's file-header comment).
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        "Detail",
        DETAIL_SLIDER_MIN,      // MIN_VALUE (slider range)
        DETAIL_SLIDER_MAX,      // MAX_VALUE (slider range)
        DETAIL_SLIDER_MIN,      // VALID_MIN (typed-value range)
        DETAIL_SLIDER_MAX,      // VALID_MAX (typed-value range)
        DETAIL_SLIDER_DEFAULT,  // DEF_VALUE
        1,                      // PRECISION (decimal places shown)
        0,                      // DISPLAY_FLAGS
        0,                      // WANT_PHASE (not an angle-style param)
        DETAIL_DISK_ID);

    out_data->num_params = AI_UPSCALE_NUM_PARAMS;
    return err;
}

PF_Err HandleFrameSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    if (!in_data || !out_data || !params) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Validate the input size the host reports before computing anything
    // downstream of it -- catches a corrupt/hostile in_data->width or
    // height before any buffer-sized computation happens.
    try {
        upscale::validate_input_dims(in_data->width, in_data->height, 4);
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleFrameSetup: rejecting frame size: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: unsupported frame size (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    // Output world stays at input size (see the buffer-expansion-retraction
    // comment at the top of this file): this effect no longer requests
    // PF_OutFlag_I_EXPAND_BUFFER, so out_data->width/height/origin must
    // simply describe "same size, no reposition" -- growing them here would
    // be inconsistent with the flags declared in HandleGlobalSetup() and is
    // exactly the host/plugin disagreement that caused the real-hardware
    // render failure this fix targets.
    out_data->width = in_data->width;
    out_data->height = in_data->height;
    out_data->origin.h = 0;
    out_data->origin.v = 0;

    return PF_Err_NONE;
}

PF_Err HandleRender(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;

    if (!in_data || !out_data || !params || !output) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // 8bpc only (see README known limitations). AE signals higher bit
    // depths via in_data->appl_id / PF_WORLD_IS_DEEP(output)-style checks
    // depending on SDK version; bail out cleanly if not 8bpc.
    if (PF_WORLD_IS_DEEP(output)) {
        set_return_msg(out_data, "AI Upscale currently supports 8bpc footage only.");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    PF_EffectWorld* input_world = &params[AI_UPSCALE_INPUT]->u.ld;

    // NULL / zero-size input world checks -- a well-behaved host should
    // never hand us this, but the plugin API boundary must never trust
    // that assumption (see plugin/README.md "セキュリティ").
    if (!input_world || !input_world->data || input_world->width <= 0 || input_world->height <= 0) {
        upscale::log_error("HandleRender: rejecting null/empty input world");
        set_return_msg(out_data, "AI Upscale: invalid or empty input frame.");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    // Likewise for the output world -- HandleRender below trusts
    // output->width/height as the ground truth for what to write, so a
    // null/zero-size output must be rejected up front too.
    if (output->width <= 0 || output->height <= 0) {
        upscale::log_error("HandleRender: rejecting null/empty output world");
        set_return_msg(out_data, "AI Upscale: invalid output frame.");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    try {
        upscale::validate_input_dims(input_world->width, input_world->height, 4);
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleRender: rejecting input world size: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: unsupported frame size (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    // Output world size is expected to equal input world size now that
    // PF_OutFlag_I_EXPAND_BUFFER is retracted (see file-header comment),
    // but is still checked independently with the output-oriented
    // safe_buffer_bytes() (rather than assuming it matches input) in case
    // a host hands back a different size for its own internal reasons
    // (tiling/banding) -- see the resample-to-match step below, which
    // handles that case gracefully instead of assuming a fixed
    // input/output relationship. This also guards the allocation
    // resize_rgba_bilinear() below would otherwise perform against a
    // corrupt/hostile output->width/height.
    try {
        upscale::safe_buffer_bytes(output->width, output->height, 4, 1);
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleRender: rejecting output world size: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: unsupported output frame size (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    // Diagnostic logging (throttled to size-combination changes -- see
    // log_render_diag_if_changed() above) so a host/plugin size mismatch
    // like the one that caused the field-reported render failure is
    // immediately visible in AIUpscale.log on the next report.
    log_render_diag_if_changed(in_data, input_world, output);

    upscale::ImageRGBA8 in_img = world_to_rgba8(input_world);

    // Request SAME-RESOLUTION output: this is the core of the buffer-
    // expansion retraction (see file-header comment) -- the output world is
    // always kept at input_world's own size, never grown.
    //
    // Detail Preserve (Fast) is now the ONLY engine (see AIUpscale.h's
    // file-header comment for why the AI/Real-ESRGAN engine was removed
    // entirely from this plugin). This is a classical, non-neural, clean-
    // room algorithm (see detail_upscaler.h); no model, no OnnxUpscaler, no
    // ConcurrencyGate, and no worker-thread pool -- it always runs
    // directly, serially, on the calling (Premiere host) render thread,
    // which is exactly the safe behavior needed given that Premiere already
    // parallelizes rendering across its own host threads (see
    // plugin/README.md "安定運用ガイド" for the history of why an
    // additional layer of in-plugin parallelism was actively harmful for
    // the old AI engine). Same-size in/out (out_w/out_h == in_img's own
    // size) means detail_preserving_upscale() skips its internal Lanczos
    // base resize entirely and only runs the detail-restoration stage --
    // see detail_upscaler.h/.cpp.
    upscale::ImageRGBA8 same_res_out;
    const float detail_amount = static_cast<float>(params[AI_UPSCALE_DETAIL_SLIDER]->u.fs_d.value);
    try {
        upscale::detail_preserving_upscale(in_img, same_res_out, in_img.width, in_img.height, detail_amount);
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleRender: detail_preserving_upscale rejected by size limit: ") +
                            ex.what());
        set_return_msg(out_data, "AI Upscale: frame too large (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    const int out_target_w = static_cast<int>(output->width);
    const int out_target_h = static_cast<int>(output->height);

    if (same_res_out.width == out_target_w && same_res_out.height == out_target_h) {
        // Expected case: same_res_out is input_world-sized, and output
        // world is also input_world-sized now that I_EXPAND_BUFFER is
        // retracted -- sizes already match, nothing to resample.
        rgba8_to_world(same_res_out, output);
    } else {
        // Sizes differ -- the host handed a different output world size
        // than input_world for some reason of its own (tiling/banding,
        // etc). Resample to whatever size the host actually gave us
        // instead of assuming a fixed relationship. Bilinear, not a high
        // quality filter -- see resize_rgba_bilinear()'s own doc comment
        // in tile.h. This is now a rare fallback path, not the primary
        // scale-reconciliation mechanism it used to be.
        upscale::log_info(
            "HandleRender: detail-regen output " + std::to_string(same_res_out.width) + "x" +
            std::to_string(same_res_out.height) + " != output world " + std::to_string(out_target_w) + "x" +
            std::to_string(out_target_h) + "; resampling (bilinear) to match.");
        upscale::ImageRGBA8 resized;
        upscale::resize_rgba_bilinear(same_res_out, resized, out_target_w, out_target_h);
        rgba8_to_world(resized, output);
    }

    return err;
}

} // namespace

// ---------------------------------------------------------------------------
// EffectMain: single dispatch entry point, matching AE SDK convention.
// ---------------------------------------------------------------------------
PF_Err EffectMain(
    PF_Cmd          cmd,
    PF_InData*      in_data,
    PF_OutData*     out_data,
    PF_ParamDef*    params[],
    PF_LayerDef*    output,
    void*           extra) {
    PF_Err err = PF_Err_NONE;

    // Safety-critical invariant: EffectMain is the ENTIRE plugin API
    // boundary with the host. Every path below must return a PF_Err, and
    // NO C++ exception may ever cross back into the host -- an uncaught
    // exception unwinding into Adobe's C-linkage dispatcher is undefined
    // behavior and will crash the host application (After Effects /
    // Premiere Pro), taking the user's project down with it. Hence the
    // broad catch(...) below in addition to the narrower catches inside
    // each Handle*() function: those provide a good out_data->return_msg,
    // this one is the last-resort backstop that guarantees the invariant
    // even for exception types/call sites we didn't anticipate.
    try {
        if (!in_data || !out_data) {
            return PF_Err_BAD_CALLBACK_PARAM;
        }

        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = HandleAbout(in_data, out_data);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = HandleGlobalSetup(in_data, out_data);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = HandleParamsSetup(in_data, out_data, params);
                break;
            case PF_Cmd_FRAME_SETUP:
                err = HandleFrameSetup(in_data, out_data, params);
                break;
            case PF_Cmd_RENDER:
                err = HandleRender(in_data, out_data, params, output);
                break;
            // PF_Cmd_SEQUENCE_SETUP / _RESETUP / _SETDOWN: intentionally
            // unhandled (falls to default: below). This effect has no
            // per-sequence state to cache anymore -- no model, no engine
            // handle, nothing -- now that the AI (Real-ESRGAN/ONNX) engine
            // has been removed (see AIUpscale.h's file-header comment), so
            // there is nothing left worth allocating/freeing a sequence
            // data handle for.
            default:
                break;
        }
    } catch (const std::exception& ex) {
        upscale::log_error(std::string("EffectMain: uncaught exception for cmd=") + std::to_string(static_cast<int>(cmd)) +
                            ": " + ex.what());
        if (out_data) {
            set_return_msg(out_data, "AI Upscale: internal error (%s).", ex.what());
        }
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    } catch (...) {
        // Non-std::exception throw (or a type we don't specifically
        // recognize) -- AE SDK convention: never let C++ exceptions cross
        // the plugin boundary; report a generic internal error instead.
        upscale::log_error("EffectMain: uncaught non-std::exception for cmd=" + std::to_string(static_cast<int>(cmd)));
        if (out_data) {
            set_return_msg(out_data, "AI Upscale: unknown internal error.");
        }
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return err;
}
