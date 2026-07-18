// AIUpscale.cpp - "AI Upscale" After Effects / Premiere Pro effect plugin.
//
// See AIUpscale.h for the "not verified in this environment" caveat: this
// file cannot be compiled without the Adobe AE SDK, which is not present
// in this Linux dev environment. It is written to match the AE SDK
// sample-plugin structure (PF_Cmd dispatch switch in EffectMain calling
// one handler per command) so it should need at most small adjustments
// once built against the real SDK headers.
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
// input size and works as a same-resolution AI detail-regeneration /
// sharpening filter instead: apply it, then scale up afterward (Transform/
// Motion) for a punched-in look -- the AI-regenerated detail holds up
// better under that later scale than plain bilinear/bicubic interpolation
// of the untouched source would. See README.md for the full user-facing
// workflow writeup, including the honest caveat that this does NOT
// increase the layer's actual pixel resolution, and the alternative
// upscale_cli-based pre-processing workflow for users who need that.
#include "AIUpscale.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>

// Platform module-path lookup used by resolve_plugin_directory() below:
// Windows resolves the .aex's own path via GetModuleHandleExA/
// GetModuleFileNameA, everyone else (macOS -- the primary target -- and
// any other POSIX host) uses dladdr(), which is available on both macOS
// and Linux (the latter only matters for this repo's dladdr-based unit
// test, since the AIUpscale target itself is never built on Linux -- see
// plugin/CMakeLists.txt).
#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

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
// name (see ensure_model_loaded() below). PF_SPRINTF is also effectively an
// unbounded sprintf into out_data->return_msg (char[PF_MAX_EFFECT_MSG_LEN +
// 1]). To avoid depending on a spelling-sensitive macro and to get a
// length-bounded, NUL-terminated write, every return_msg assignment in this
// file goes through this helper instead.
#if defined(__GNUC__) || defined(__clang__)
void set_return_msg(PF_OutData* out_data, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
void set_return_msg(PF_OutData* out_data, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out_data->return_msg, sizeof(out_data->return_msg), fmt, args);
    va_end(args);
}

int scale_choice_to_factor(PF_ParamDef* params[]) {
    const A_long choice = params[AI_UPSCALE_SCALE_POPUP]->u.pd.value;
    return (choice == SCALE_CHOICE_4X) ? 4 : 2;
}

const char* mode_choice_to_model_filename(A_long choice) {
    return (choice == MODE_CHOICE_ANIME) ? MODEL_FILENAME_ANIME : MODEL_FILENAME_PHOTO;
}

// Resolves the directory the plugin binary lives in, so we can find
// <plugin_dir>/models/*.onnx.
//
// THIS WAS THE ROOT CAUSE of the real-hardware PF_Err_INTERNAL_STRUCT_DAMAGED
// (512) reported at render time: this function used to unconditionally
// return the literal string "models", resolved by every later
// std::filesystem call relative to the host process's current working
// directory. Premiere Pro/After Effects do NOT run with CWD set to the
// plugin's install directory (MediaCore or Plug-ins), so
// model_path_for()'s fs::canonical(models_dir) lookup failed, model_path_for
// returned "", and ensure_model_loaded() surfaced that as
// PF_Err_INTERNAL_STRUCT_DAMAGED -- exactly the error code seen in the
// field. Premiere reports render-time errors in aggregate at
// PF_Cmd_FRAME_SETDOWN (selector 11), which is why the error dialog
// pointed at teardown rather than the actual failing command.
//
// Fixed by resolving the plugin's own module/bundle path via the
// platform loader APIs instead of trusting CWD:
//   - macOS: dladdr() on this very function's address gives the absolute
//     path of the Mach-O binary inside the bundle,
//     .../MediaCore/AIUpscale.plugin/Contents/MacOS/AIUpscale. Four
//     parent_path() calls walk back up to MediaCore, the directory
//     setup_mac.sh's step6_install() installs models/ into as a sibling
//     of AIUpscale.plugin (see MEDIACORE_DIR/dest_bundle/dest_models
//     there) -- i.e. exactly the plugin_dir model_path_for() expects.
//   - Windows: the plugin ships as a bare .aex (a renamed DLL) placed
//     directly in the shared Plug-ins/MediaCore folder, with models/ as
//     an immediate sibling (no bundle nesting like macOS), so the
//     directory containing the .aex IS the plugin directory --
//     GetModuleFileNameA's result needs only one parent_path().
// Both branches fall back to the old CWD-relative "models" (with a WARN
// log) if the platform lookup fails, so behavior degrades gracefully
// rather than throwing/crashing.
std::string resolve_plugin_directory() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&resolve_plugin_directory),
            &module)) {
        char path_buf[MAX_PATH];
        const DWORD len = GetModuleFileNameA(module, path_buf, sizeof(path_buf));
        // GetModuleFileNameA returns 0 on failure, or a length == the
        // buffer size (with no reliable way to distinguish "exact fit"
        // from "truncated") on overflow -- treat both as failure rather
        // than risk silently using a truncated path.
        if (len > 0 && len < sizeof(path_buf)) {
            const std::filesystem::path aex_path(std::string(path_buf, len));
            // .../Plug-ins/Common/AIUpscale.aex -> .../Plug-ins/Common (1)
            const std::filesystem::path plugin_dir = aex_path.parent_path();
            if (!plugin_dir.empty()) {
                return plugin_dir.string();
            }
        }
    }
    upscale::log_warn(
        "resolve_plugin_directory: GetModuleHandleExA/GetModuleFileNameA failed to resolve the "
        "plugin's own module path; falling back to CWD-relative \"models\" (this will very likely "
        "fail to find models/ under a real Premiere/AE host -- see AIUpscale.cpp comment).");
    return "models";
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&resolve_plugin_directory), &info) && info.dli_fname &&
        info.dli_fname[0] != '\0') {
        const std::filesystem::path bin(info.dli_fname);
        // .../MediaCore/AIUpscale.plugin/Contents/MacOS/AIUpscale
        //   -> .../MediaCore/AIUpscale.plugin/Contents/MacOS   (1)
        //   -> .../MediaCore/AIUpscale.plugin/Contents         (2)
        //   -> .../MediaCore/AIUpscale.plugin                  (3)
        //   -> .../MediaCore                                   (4)
        const std::filesystem::path plugin_dir =
            bin.parent_path().parent_path().parent_path().parent_path();
        if (!plugin_dir.empty()) {
            return plugin_dir.string();
        }
    }
    upscale::log_warn(
        "resolve_plugin_directory: dladdr() failed to resolve the plugin's own module path; "
        "falling back to CWD-relative \"models\" (this will very likely fail to find models/ "
        "under a real Premiere/AE host -- see AIUpscale.cpp comment).");
    return "models";
#endif
}

// Resolves and validates the on-disk path of the model for `mode_choice`,
// enforcing that it lives inside <plugin_dir>/models/ -- defense against
// path traversal (e.g. a tampered/malicious plugin_dir value, or a future
// code path that lets mode_choice-derived strings contain "../") even
// though today's mode_choice_to_model_filename() only ever returns one of
// two fixed constants. Symlinks are resolved (std::filesystem::canonical)
// before the containment check, so a symlink planted inside models/ that
// points outside the directory is also rejected.
//
// Returns the empty string if the resolved path is missing or escapes
// <plugin_dir>/models/; callers must treat that as a load failure.
std::string model_path_for(const std::string& plugin_dir, A_long mode_choice) {
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path models_dir = fs::path(plugin_dir) / "models";
    const fs::path candidate = models_dir / mode_choice_to_model_filename(mode_choice);

    const fs::path canonical_models_dir = fs::canonical(models_dir, ec);
    if (ec) {
        upscale::log_error("model_path_for: models/ directory not found or unreadable: " + models_dir.string() +
                            " (" + ec.message() + ")");
        return "";
    }
    const fs::path canonical_candidate = fs::canonical(candidate, ec);
    if (ec) {
        upscale::log_error("model_path_for: model file not found or unreadable: " + candidate.string() +
                            " (" + ec.message() + ")");
        return "";
    }

    // Containment check: canonical_candidate must be canonical_models_dir
    // itself or a descendant of it (string-prefix check on the canonical,
    // symlink-resolved paths -- this is what defeats both "../.." style
    // traversal AND a symlink planted inside models/ pointing elsewhere).
    const std::string dir_str = canonical_models_dir.string();
    const std::string file_str = canonical_candidate.string();
    const bool contained = file_str.size() > dir_str.size() &&
        file_str.compare(0, dir_str.size(), dir_str) == 0 &&
        (file_str[dir_str.size()] == fs::path::preferred_separator);
    if (!contained) {
        upscale::log_error("model_path_for: resolved model path escapes plugin models/ directory, rejecting: " +
                            file_str);
        return "";
    }

    return canonical_candidate.string();
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
// (input size, output size, downsample, scale) combination actually
// changes, so a size-mismatch pattern is visible in AIUpscale.log without
// drowning it in repeats.
// ---------------------------------------------------------------------------
struct RenderDiagCache {
    std::mutex mutex;
    int64_t last_in_w = -1, last_in_h = -1;
    int64_t last_out_w = -1, last_out_h = -1;
    int64_t last_full_w = -1, last_full_h = -1;
    int64_t last_dsx_num = -1, last_dsx_den = -1;
    int64_t last_dsy_num = -1, last_dsy_den = -1;
    int last_scale = -1;
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
const char* classify_expand_status(int64_t in_w, int64_t in_h, int64_t out_w, int64_t out_h, int /*scale*/) {
    if (out_w == in_w && out_h == in_h) {
        return "detail-regen";
    }
    return "custom";
}

void log_render_diag_if_changed(PF_InData* in_data, const PF_EffectWorld* input_world,
                                 const PF_LayerDef* output, int scale) {
    const int64_t in_w = input_world->width, in_h = input_world->height;
    const int64_t out_w = output->width, out_h = output->height;
    const int64_t full_w = in_data->width, full_h = in_data->height;
    const int64_t dsx_num = in_data->downsample_x.num, dsx_den = in_data->downsample_x.den;
    const int64_t dsy_num = in_data->downsample_y.num, dsy_den = in_data->downsample_y.den;

    std::lock_guard<std::mutex> lock(g_render_diag_cache.mutex);
    RenderDiagCache& c = g_render_diag_cache;
    if (in_w == c.last_in_w && in_h == c.last_in_h && out_w == c.last_out_w && out_h == c.last_out_h &&
        full_w == c.last_full_w && full_h == c.last_full_h && dsx_num == c.last_dsx_num &&
        dsx_den == c.last_dsx_den && dsy_num == c.last_dsy_num && dsy_den == c.last_dsy_den &&
        scale == c.last_scale) {
        return; // identical to last-logged combination, skip (avoid per-frame/per-thread spam)
    }
    c.last_in_w = in_w; c.last_in_h = in_h;
    c.last_out_w = out_w; c.last_out_h = out_h;
    c.last_full_w = full_w; c.last_full_h = full_h;
    c.last_dsx_num = dsx_num; c.last_dsx_den = dsx_den;
    c.last_dsy_num = dsy_num; c.last_dsy_den = dsy_den;
    c.last_scale = scale;

    const char* expand_status = classify_expand_status(in_w, in_h, out_w, out_h, scale);

    std::ostringstream oss;
    oss << "HandleRender: size combo changed - input_world=" << in_w << "x" << in_h
        << " output_world=" << out_w << "x" << out_h
        << " in_data(full-res)=" << full_w << "x" << full_h
        << " downsample_x=" << dsx_num << "/" << dsx_den
        << " downsample_y=" << dsy_num << "/" << dsy_den
        << " scale_param=" << scale
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
        "%s v%d.%d\r%s\rAI super-resolution upscaling (Real-ESRGAN via ONNX Runtime).",
        AI_UPSCALE_NAME, AI_UPSCALE_MAJOR_VERSION, AI_UPSCALE_MINOR_VERSION, AI_UPSCALE_DESCRIPTION);
    return PF_Err_NONE;
}

PF_Err HandleGlobalSetup(PF_InData* in_data, PF_OutData* out_data) {
    out_data->my_version = PF_VERSION(
        AI_UPSCALE_MAJOR_VERSION, AI_UPSCALE_MINOR_VERSION, AI_UPSCALE_BUG_VERSION,
        AI_UPSCALE_STAGE_VERSION, AI_UPSCALE_BUILD_VERSION);

    // PF_OutFlag_DEEP_COLOR_AWARE intentionally NOT set: 8bpc only for
    // now (see README "known limitations"). PF_OutFlag_SEQUENCE_DATA_NEEDS_FLATTENING
    // omitted since our sequence data is not persisted to project files.
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
    // resolution AI detail regeneration), so there is no expansion left to
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

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(
        "Scale",
        SCALE_POPUP_NUM_CHOICES,
        SCALE_CHOICE_2X,
        SCALE_POPUP_CHOICES,
        SCALE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(
        "Mode",
        MODE_POPUP_NUM_CHOICES,
        MODE_CHOICE_PHOTO,
        MODE_POPUP_CHOICES,
        MODE_DISK_ID);

    out_data->num_params = AI_UPSCALE_NUM_PARAMS;
    return err;
}

// Allocates and initializes a fresh AIUpscaleSequenceData, wrapped in a
// PF_Handle the way AE/Premiere sequence data must be. Factored out of
// HandleSequenceSetup() so the exact same initialization can also be used
// as a same-call self-healing fallback from HandleRender()/HandleFrameSetup()
// (see ensure_sequence_data() below) when the host hands us a render/frame
// call without ever having delivered PF_Cmd_SEQUENCE_SETUP/_RESETUP first.
//
// PF_InData has no "in_data" member -- in_data IS the PF_InData*, so
// members are accessed directly (in_data->pica_basicP etc.), not via a
// nonexistent in_data->in_data. pica_basicP is an SPBasicSuite*, which is
// only an AcquireSuite/ReleaseSuite bridge -- it has no
// new_handle/lock_handle/etc. members itself. The correct AE SDK way to
// get handle-manipulation functions is to acquire AEGP_HandleSuite1 via
// AEGP_SuiteHandler (declared in AEGP_SuiteHandler.h, included by
// AIUpscale.h), which wraps AcquireSuite/ReleaseSuite for the common
// suites and can throw on a missing suite -- safe here because every
// caller of this function is inside EffectMain's try/catch boundary.
//
// On success, *out_seq_handle receives the new PF_Handle and PF_Err_NONE
// is returned. On failure, *out_seq_handle is left untouched and a
// non-NONE PF_Err is returned.
PF_Err CreateSequenceData(PF_InData* in_data, PF_Handle* out_seq_handle) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    // Allocate our sequence data on the heap and stash the raw pointer in
    // a PF_Handle. This mirrors the pattern used by AE SDK samples that
    // need non-flat (C++ object) sequence data -- e.g. wrapping the
    // pointer in a small fixed-size handle rather than trying to make
    // AIUpscaleSequenceData itself relocatable, since it owns a
    // unique_ptr<OnnxUpscaler>.
    PF_Handle seq_handle = suites.HandleSuite1()->host_new_handle(sizeof(AIUpscaleSequenceData*));
    if (!seq_handle) {
        return PF_Err_OUT_OF_MEMORY;
    }

    auto** stored_ptr = reinterpret_cast<AIUpscaleSequenceData**>(
        suites.HandleSuite1()->host_lock_handle(seq_handle));
    *stored_ptr = new (std::nothrow) AIUpscaleSequenceData();
    if (!*stored_ptr) {
        suites.HandleSuite1()->host_unlock_handle(seq_handle);
        suites.HandleSuite1()->host_dispose_handle(seq_handle);
        return PF_Err_OUT_OF_MEMORY;
    }
    (*stored_ptr)->plugin_dir = resolve_plugin_directory();
    suites.HandleSuite1()->host_unlock_handle(seq_handle);

    *out_seq_handle = seq_handle;
    return PF_Err_NONE;
}

PF_Err HandleSequenceSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_Handle seq_handle = nullptr;
    const PF_Err err = CreateSequenceData(in_data, &seq_handle);
    if (err) {
        return err;
    }

    // AE SDK convention: PF_Cmd_SEQUENCE_SETUP (and _RESETUP) hand the new
    // sequence data back to the host via out_data->sequence_data; the host
    // stores it and passes it back on subsequent calls as
    // in_data->sequence_data (an in-only field on PF_InData -- there is no
    // corresponding settable field on PF_InData itself).
    out_data->sequence_data = seq_handle;
    return PF_Err_NONE;
}

PF_Err HandleSequenceSetdown(PF_InData* in_data, PF_OutData* out_data) {
    if (in_data->sequence_data) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        auto** stored_ptr = reinterpret_cast<AIUpscaleSequenceData**>(
            suites.HandleSuite1()->host_lock_handle(in_data->sequence_data));
        if (stored_ptr && *stored_ptr) {
            delete *stored_ptr;
            *stored_ptr = nullptr;
        }
        suites.HandleSuite1()->host_unlock_handle(in_data->sequence_data);
        suites.HandleSuite1()->host_dispose_handle(in_data->sequence_data);
    }
    out_data->sequence_data = nullptr;
    return PF_Err_NONE;
}

AIUpscaleSequenceData* get_sequence_data(PF_InData* in_data) {
    if (!in_data->sequence_data) return nullptr;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    auto** stored_ptr = reinterpret_cast<AIUpscaleSequenceData**>(
        suites.HandleSuite1()->host_lock_handle(in_data->sequence_data));
    AIUpscaleSequenceData* seq = stored_ptr ? *stored_ptr : nullptr;
    suites.HandleSuite1()->host_unlock_handle(in_data->sequence_data);
    return seq;
}

// Self-healing sequence-data accessor for the render-time path
// (HandleFrameSetup/HandleRender). AE always issues PF_Cmd_SEQUENCE_SETUP
// (or _RESETUP) before the first PF_Cmd_FRAME_SETUP/PF_Cmd_RENDER of a
// sequence, so get_sequence_data() returning nullptr there should never
// happen in principle -- but this is exactly the failure that was
// observed on real Premiere Pro hardware (in_data->sequence_data == null
// / get_sequence_data() == nullptr at render time), reported as
// PF_Err_INTERNAL_STRUCT_DAMAGED (512) and surfaced by Premiere's
// aggregate error reporting at PF_Cmd_FRAME_SETDOWN (selector 11). Since
// some hosts apparently do not guarantee AE's exact sequence-command
// timing, prefer self-recovery over an immediate hard failure: our
// sequence data is trivial to reconstruct (it's just a lazily-populated
// model cache keyed off resolve_plugin_directory()), so there is nothing
// to actually lose by creating it on the spot here.
//
// Every recovery is logged at WARN so a pattern of "missing sequence data
// at render time" is visible in AIUpscale.log even though the render
// itself now succeeds -- see plugin/README.md "安定運用ガイド".
//
// Returns nullptr only if self-healing itself fails (e.g. out of memory),
// in which case callers must still surface PF_Err_INTERNAL_STRUCT_DAMAGED
// as before.
AIUpscaleSequenceData* ensure_sequence_data(PF_InData* in_data, PF_OutData* out_data) {
    AIUpscaleSequenceData* seq = get_sequence_data(in_data);
    if (seq) {
        return seq;
    }

    upscale::log_warn(
        "ensure_sequence_data: in_data->sequence_data missing at render/frame-setup time "
        "(expected PF_Cmd_SEQUENCE_SETUP/_RESETUP to have run first); self-healing by creating "
        "sequence data now instead of failing the render.");

    PF_Handle seq_handle = nullptr;
    const PF_Err err = CreateSequenceData(in_data, &seq_handle);
    if (err || !seq_handle) {
        upscale::log_error(
            "ensure_sequence_data: self-recovery failed to allocate sequence data (out of memory?); "
            "render must fail.");
        return nullptr;
    }

    // Hand the newly-created sequence data back to the host the same way
    // PF_Cmd_SEQUENCE_SETUP does, so that -- on hosts that do honor
    // out_data->sequence_data when set outside of SEQUENCE_SETUP -- later
    // calls in this sequence no longer need to self-heal.
    out_data->sequence_data = seq_handle;

    // Deliberately NOT re-read via get_sequence_data(in_data) here:
    // in_data is this call's (const, host-owned) input, and the host has
    // no opportunity to echo the out_data->sequence_data we just set back
    // into in_data->sequence_data until its NEXT call into EffectMain --
    // so in_data->sequence_data is still null right now regardless of
    // what we just did to out_data. Read the pointer back out of
    // seq_handle directly instead.
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    auto** stored_ptr = reinterpret_cast<AIUpscaleSequenceData**>(
        suites.HandleSuite1()->host_lock_handle(seq_handle));
    AIUpscaleSequenceData* result = stored_ptr ? *stored_ptr : nullptr;
    suites.HandleSuite1()->host_unlock_handle(seq_handle);
    return result;
}

// Ensures the correct model (per current Mode param) is loaded into the
// cached OnnxUpscaler, reloading only when the mode actually changed.
// Prefers CoreML on macOS (see OnnxUpscaler::load()'s append_providers()
// for the actual EP selection/fallback logic); any load failure --
// including a rejected/missing model path -- is surfaced to the host via
// both PF_Err and out_data->return_msg, and logged with the attempted
// path for offline diagnosis (see plugin/README.md "安定運用ガイド").
PF_Err ensure_model_loaded(AIUpscaleSequenceData* seq, A_long mode_choice, PF_OutData* out_data) {
    if (seq->upscaler && seq->loaded_mode_choice == mode_choice) {
        return PF_Err_NONE; // already loaded, nothing to do
    }

    const std::string model_path = model_path_for(seq->plugin_dir, mode_choice);
    if (model_path.empty()) {
        // model_path_for() already logged the specific reason (missing /
        // path-traversal / symlink-escape).
        set_return_msg(out_data,
                  "AI Upscale: model file not found or invalid. Reinstall models/ next to the plugin.");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    try {
        // get_shared(), NOT a private load() on a locally-owned instance:
        // this hands back a process-wide shared OnnxUpscaler/Ort::Session
        // for `model_path`, so multiple AIUpscaleSequenceData instances
        // for the same render session (see the self-healing comment on
        // ensure_sequence_data() above -- the exact scenario that used to
        // multiply model sessions) end up sharing one loaded model instead
        // of each loading their own. See AIUpscaleSequenceData::upscaler's
        // doc comment in AIUpscale.h and OnnxUpscaler::get_shared()'s in
        // onnx_upscaler.h.
        seq->upscaler = upscale::OnnxUpscaler::get_shared(model_path, upscale::ExecutionProvider::kAuto);
        seq->loaded_mode_choice = mode_choice;
        if (seq->upscaler->fell_back_to_cpu()) {
            set_return_msg(out_data,
                      "AI Upscale: accelerated execution provider unavailable; running on CPU (slower). See log.");
        }
    } catch (const upscale::OnnxUpscalerError& ex) {
        upscale::log_error(std::string("ensure_model_loaded: failed to load '") + model_path + "': " + ex.what());
        set_return_msg(out_data, "AI Upscale: failed to load model (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED; // best available generic PF_Err for "bad model file"
    }
    return PF_Err_NONE;
}

PF_Err HandleFrameSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    if (!in_data || !out_data || !params) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Self-healing precondition (see ensure_sequence_data() above): make
    // sure sequence data exists before doing anything else, so that by the
    // time PF_Cmd_RENDER runs for this frame it is already present rather
    // than needing its own recovery. FRAME_SETUP itself doesn't otherwise
    // need the pointer, but this is the earliest render-path opportunity
    // to detect and fix a missing-sequence-data host (the actual root
    // cause behind the field-reported PF_Err_INTERNAL_STRUCT_DAMAGED /
    // 512).
    if (!ensure_sequence_data(in_data, out_data)) {
        set_return_msg(out_data, "AI Upscale: internal error (missing sequence data, self-recovery failed).");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
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
    // render failure this fix targets. The "Scale" param no longer changes
    // the output buffer's size; HandleRender uses it to control how much
    // internal AI detail-regeneration strength is applied while writing
    // back to this same-size buffer.
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

    AIUpscaleSequenceData* seq = ensure_sequence_data(in_data, out_data);
    if (!seq) {
        set_return_msg(out_data, "AI Upscale: internal error (missing sequence data, self-recovery failed).");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    const A_long mode_choice = params[AI_UPSCALE_MODE_POPUP]->u.pd.value;
    err = ensure_model_loaded(seq, mode_choice, out_data);
    if (err) return err;

    // `scale` (2x/4x from the "Scale" UI popup) no longer selects an output
    // buffer size (see file-header comment) -- kept here only for the
    // diagnostic log below and as a TODO hook for a future detail-
    // regeneration-strength knob; it does not change what gets requested
    // from seq->upscaler->upscale() further down (always requested_scale=1,
    // same resolution).
    const int scale = scale_choice_to_factor(params);
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
    log_render_diag_if_changed(in_data, input_world, output, scale);

    upscale::ImageRGBA8 in_img = world_to_rgba8(input_world);

    upscale::TileOptions tile_opts;
    tile_opts.tile_size = 0; // auto-select based on active execution provider (see choose_tile_size())
    tile_opts.overlap = 16;
    // num_workers FORCED TO 1 (serial tiling) here -- this is deliberate,
    // not an oversight, and is one of the fixes for a real-Premiere-
    // hardware freeze (see README.md "安定運用ガイド" and concurrency.h's
    // file header for the full story): Premiere Pro already parallelizes
    // 4K rendering across many of its OWN host threads (one call into
    // HandleRender per in-flight frame). Field logs showed each of those
    // ~14 host threads ALSO spinning up tile.cpp's internal worker pool
    // (the old auto=hardware_concurrency default), producing on the order
    // of host_threads * tile_workers (~14 x 14 = ~200) threads all
    // fighting over the same CPU/ANE cores at once -- enough to saturate
    // and freeze the machine. The host's own frame-level parallelism is
    // the right place for concurrency here; this plugin must not ALSO
    // parallelize within a single frame on top of it. (Overall
    // concurrency across host threads is still bounded separately by
    // ConcurrencyGate inside OnnxUpscaler::upscale() -- see
    // onnx_upscaler.cpp/.h -- which caps how many upscale() calls run
    // inference at once regardless of how many host threads call in.)
    //
    // upscale_cli (see src/cli/upscale_cli.cpp), by contrast, is a single
    // process with no host-level frame parallelism to defer to, so it
    // still passes the user's --jobs value through unchanged (0=auto is
    // the right default there).
    tile_opts.num_workers = 1;

    // Request SAME-RESOLUTION output (requested_scale=1) rather than the
    // model's raw native-scale result: this is the core of the buffer-
    // expansion retraction (see file-header comment). Passing 1 here tells
    // OnnxUpscaler::upscale() to set TileOptions::output_scale=1 (see
    // tile.h/tile.cpp and onnx_upscaler.cpp), which downsizes EACH TILE's
    // native-scale (typically 4x) result back down immediately after
    // inference, before compositing -- so at no point does a buffer sized
    // input_world*4 for the WHOLE frame ever get allocated, regardless of
    // how large input_world is (this is what let a host-handed input world
    // far bigger than the nominal source resolution -- e.g.
    // 4892x8192 -- blow up to a 641,204,224px intermediate and crash the
    // render in the previous, non-tiled-downsize version of this fix).
    // Peak memory per tile job is bounded to (tile_size*native_scale)^2,
    // not (input_world*native_scale)^2.
    //
    // The "Scale" UI popup (2x/4x) no longer controls output buffer size
    // (there's only one output size now: input_world's own size) -- it's
    // reserved for a future internal detail-regeneration-strength knob
    // (TODO, see README.md); both choices currently take the same
    // same-resolution code path here.
    upscale::ImageRGBA8 same_res_out;
    try {
        seq->upscaler->upscale(in_img, same_res_out, /*requested_scale=*/1, tile_opts);
    } catch (const upscale::OnnxUpscalerError& ex) {
        upscale::log_error(std::string("HandleRender: upscale failed: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: render failed (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleRender: upscale rejected by size limit: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: frame too large (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    const int out_target_w = static_cast<int>(output->width);
    const int out_target_h = static_cast<int>(output->height);

    if (same_res_out.width == out_target_w && same_res_out.height == out_target_h) {
        // Expected case: same_res_out is input_world-sized (requested_scale
        // =1), and output world is also input_world-sized now that
        // I_EXPAND_BUFFER is retracted -- sizes already match, nothing to
        // resample.
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
            case PF_Cmd_SEQUENCE_SETUP:
            case PF_Cmd_SEQUENCE_RESETUP:
                err = HandleSequenceSetup(in_data, out_data);
                break;
            case PF_Cmd_SEQUENCE_SETDOWN:
                err = HandleSequenceSetdown(in_data, out_data);
                break;
            case PF_Cmd_FRAME_SETUP:
                err = HandleFrameSetup(in_data, out_data, params);
                break;
            case PF_Cmd_RENDER:
                err = HandleRender(in_data, out_data, params, output);
                break;
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
