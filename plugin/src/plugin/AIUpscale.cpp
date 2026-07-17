// AIUpscale.cpp - "AI Upscale" After Effects / Premiere Pro effect plugin.
//
// See AIUpscale.h for the "not verified in this environment" caveat: this
// file cannot be compiled without the Adobe AE SDK, which is not present
// in this Linux dev environment. It is written to match the AE SDK
// sample-plugin structure (PF_Cmd dispatch switch in EffectMain calling
// one handler per command) so it should need at most small adjustments
// once built against the real SDK headers.
//
// Buffer-expansion caveat (also in README.md): PF_Cmd_FRAME_SETUP below
// grows out_data->width/height by the requested scale, the same mechanism
// AE blur-type effects use to grow their output rect. This is the
// standard AE pattern; Premiere Pro's support for resizing effects'
// output this way is more limited and MUST be verified on a real
// Premiere install before shipping (some Premiere effect hosts clip or
// ignore output world resizing that doesn't come from a small connected
// set of "resize"-capable effect types). If Premiere does not honor the
// resize, the practical workaround is to keep the output world at input
// size and letter/pillar-box or require the user to apply an explicit
// "Scale" transform after this effect -- left as a TODO pending real
// hardware/software testing.
#include "AIUpscale.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <system_error>

#include "logger.h"
#include "size_limits.h"

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
// <plugin_dir>/models/*.onnx. AE SDK exposes this via
// PF_AppSuite/PF_UtilitySuite path helpers or, more commonly, via the
// platform APIs (GetModuleFileName on Windows, CFBundle on macOS) using
// the module handle supplied at DllMain/bundle-load time. The exact
// helper varies by SDK version; sketched here as a TODO seam so the
// plugin builds link-complete once the real helper is selected.
std::string resolve_plugin_directory() {
    // TODO(sdk): populate via platform-specific module path lookup.
    // Placeholder: relies on a fixed relative-to-CWD "models/" for now,
    // which is sufficient for local testing inside a host but should be
    // replaced with an absolute path lookup before shipping.
    return "models";
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
    out_data->out_flags = PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_I_DO_DIALOG * 0;
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

PF_Err HandleSequenceSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_Err err = PF_Err_NONE;

    // PF_InData has no "in_data" member -- in_data IS the PF_InData*, so
    // members are accessed directly (in_data->pica_basicP etc.), not via a
    // nonexistent in_data->in_data. pica_basicP is an SPBasicSuite*, which
    // is only an AcquireSuite/ReleaseSuite bridge -- it has no
    // new_handle/lock_handle/etc. members itself. The correct AE SDK way
    // to get handle-manipulation functions is to acquire AEGP_HandleSuite1
    // via AEGP_SuiteHandler (declared in AEGP_SuiteHandler.h, included by
    // AIUpscale.h), which wraps AcquireSuite/ReleaseSuite for the common
    // suites and can throw on a missing suite -- safe here because we're
    // inside EffectMain's try/catch boundary.
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

    // AE SDK convention: PF_Cmd_SEQUENCE_SETUP (and _RESETUP) hand the new
    // sequence data back to the host via out_data->sequence_data; the host
    // stores it and passes it back on subsequent calls as
    // in_data->sequence_data (an in-only field on PF_InData -- there is no
    // corresponding settable field on PF_InData itself).
    out_data->sequence_data = seq_handle;
    return err;
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

    if (!seq->upscaler) {
        seq->upscaler = std::make_unique<upscale::OnnxUpscaler>();
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
        seq->upscaler->load(model_path, upscale::ExecutionProvider::kAuto);
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

    const int scale = scale_choice_to_factor(params);

    // Validate the input size the host reports before computing the
    // expanded output size, and validate the resulting output size too
    // (see size_limits.h) -- catches a corrupt/hostile in_data->width or
    // height before any buffer-sized computation happens.
    try {
        upscale::validate_input_dims(in_data->width, in_data->height, 4);
        upscale::safe_buffer_bytes(static_cast<int64_t>(in_data->width) * scale,
                                    static_cast<int64_t>(in_data->height) * scale, 4, 1);
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleFrameSetup: rejecting frame size: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: unsupported frame size (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    // Buffer-expansion pattern (see file-header caveat re: Premiere
    // support): grow the output world to input-size * scale and keep the
    // origin at (0,0) since this effect doesn't reposition content.
    out_data->width = in_data->width * scale;
    out_data->height = in_data->height * scale;
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

    AIUpscaleSequenceData* seq = get_sequence_data(in_data);
    if (!seq) {
        set_return_msg(out_data, "AI Upscale: internal error (missing sequence data).");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    const A_long mode_choice = params[AI_UPSCALE_MODE_POPUP]->u.pd.value;
    err = ensure_model_loaded(seq, mode_choice, out_data);
    if (err) return err;

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

    try {
        upscale::validate_input_dims(input_world->width, input_world->height, 4);
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleRender: rejecting input world size: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: unsupported frame size (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    upscale::ImageRGBA8 in_img = world_to_rgba8(input_world);

    upscale::TileOptions tile_opts;
    tile_opts.tile_size = 0; // auto-select based on active execution provider (see choose_tile_size())
    tile_opts.overlap = 16;
    // Auto worker count (hardware_concurrency, capped to tile count by
    // tile.cpp); AE/Premiere host processes are typically already busy
    // with UI/other render threads, so we don't try to be cleverer than
    // "use what's available" here.
    tile_opts.num_workers = 0;

    upscale::ImageRGBA8 out_img;
    try {
        seq->upscaler->upscale(in_img, out_img, scale, tile_opts);
    } catch (const upscale::OnnxUpscalerError& ex) {
        upscale::log_error(std::string("HandleRender: upscale failed: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: render failed (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    } catch (const upscale::SizeLimitError& ex) {
        upscale::log_error(std::string("HandleRender: upscale rejected by size limit: ") + ex.what());
        set_return_msg(out_data, "AI Upscale: frame too large (%s).", ex.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    rgba8_to_world(out_img, output);
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
