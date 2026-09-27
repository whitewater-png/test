// AIUpscalePiPL.r - PiPL (Plug-in Property List) resource for the
// "AI Upscale" effect, following the standard AE SDK PiPL layout used by
// sample plugins (Skeleton, SDK_Noise, etc).
//
// NOT VERIFIED IN THIS ENVIRONMENT: requires the Adobe AE SDK's
// PiPL.r / AE_Effect.r / AEConfig.h and a resource compiler (Rez on
// macOS, or the SDK's PiPLtool/CNVTPJ.EXE-based flow on Windows via the
// project's .rc wrapper) neither of which are available here. Field
// names/values below match the documented PiPL kind "eFKT" (effect)
// layout; double-check against AE_Effect.r for the SDK version in use.
//
// This resource is only useful once it is actually compiled into the
// plugin bundle/DLL -- see plugin/CMakeLists.txt's Rez custom command
// (macOS) for how that happens. Without a compiled PiPL resource sitting
// at Contents/Resources/<executable-name>.rsrc inside the .plugin bundle,
// After Effects/Premiere Pro will not show "AI Upscale" in the effects
// list at all, even though the bundle itself loads fine as a Mach-O
// module (this was the actual root cause of the plugin not appearing in
// Premiere's effect list on real hardware).
//
// AE_Effect_Global_OutFlags / _2 below MUST stay numerically identical to
// whatever HandleGlobalSetup() in AIUpscale.cpp sets on
// out_data->out_flags / out_flags2 at PF_Cmd_GLOBAL_SETUP -- AE/Premiere
// cross-checks the two and will refuse to load (or silently misbehave)
// on a mismatch. See the derivation comments below.
#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include "AE_General.r"
#endif

resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            "AI Upscale"
        },
        Category {
            "AI Enhance"
        },

#ifdef AE_OS_WIN
    #ifdef AE_PROC_INTELx64
        CodeWin64X86 {"EffectMain"},
    #endif
#else
        CodeMacIntel64 {"EffectMain"},
        CodeMacARM64   {"EffectMain"},
#endif

        // AE_PiPL_Version matches the running SDK's expectations;
        // mirrors the {1, 0} convention used across recent SDK samples.
        AE_PiPL_Version {
            1, 0
        },

        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },

        AE_Effect_Version {
            // Major.Minor.Bug, stage (develop/alpha/beta/release), build
            // -- keep in sync with AIUpscale.h's AI_UPSCALE_*_VERSION
            // (MAJOR=1, MINOR=2, BUG=0, STAGE=PF_Stage_DEVELOP=0, BUILD=1).
            // MINOR bumped 1 -> 2 when the "Engine"/"Scale"/"Mode" popups
            // and the AI (Real-ESRGAN/ONNX) engine were removed entirely
            // from this plugin, leaving only the "Detail" slider param and
            // the classical Detail Preserve engine (see AIUpscale.h/.cpp
            // and detail_upscaler.h/.cpp). The AI engine remains available
            // via upscale_cli / upscale_video.sh for pre-conversion batch
            // workflows -- see plugin/README.md.
            // Verified against AE_EffectVers.h's packing:
            //   PF_VERSION(MAJOR,MINOR,BUG,STAGE,BUILD) =
            //     ((MAJOR & 0x7) << 19) | ((MINOR & 0xF) << 15) |
            //     ((BUG & 0xF) << 11) | ((STAGE & 0x3) << 9) | (BUILD & 0x1FF)
            //   = (1 << 19) | (2 << 15) | 1 = 524288 + 65536 + 1 = 589825.
            // Recompute this value (and update it here) if
            // AI_UPSCALE_*_VERSION in AIUpscale.h ever changes.
            589825
        },

        AE_Effect_Info_Flags {
            0
        },

        AE_Effect_Global_OutFlags {
            // MUST stay numerically identical to the out_data->out_flags
            // bitmask HandleGlobalSetup() (AIUpscale.cpp) sets at
            // PF_Cmd_GLOBAL_SETUP -- update BOTH sides together whenever
            // either changes, and see the two static_asserts directly
            // above HandleAbout() in AIUpscale.cpp, which fail the build
            // if AE_Effect.h's real bit positions ever disagree with the
            // values assumed here (this repo's dev environment has no
            // real AE_Effect.h to check against directly):
            //   out_data->out_flags = PF_OutFlag_NON_PARAM_VARY
            //                        | PF_OutFlag_DISPLAY_ERROR_MESSAGE;
            // Per AE_Effect.h's PF_OutFlag enum (bit position):
            //   PF_OutFlag_NON_PARAM_VARY      = 1L << 2 = 0x00000004
            //   PF_OutFlag_DISPLAY_ERROR_MESSAGE = 1L << 8 = 0x00000100
            //   combined = 0x004 | 0x100 = 0x00000104 (260)
            // PF_OutFlag_I_EXPAND_BUFFER (0x00000200) was tried here in a
            // previous revision of this fix -- both here and in
            // out_data->out_flags -- to authorize PF_Cmd_FRAME_SETUP
            // growing out_data->width/height for an AI-upscaled (larger)
            // output. Real Premiere Pro hardware logs proved this
            // unreliable rather than merely unsupported: with the flag
            // declared, Premiere handed HandleRender an input world
            // (params[...]->u.ld) that was ALREADY larger than the
            // layer's nominal full-resolution size, and this plugin's own
            // native model upscale on top of THAT produced a
            // 641,204,224px intermediate that blew through every size
            // safety margin and crashed the render with
            // PF_Err_INTERNAL_STRUCT_DAMAGED (512). I_EXPAND_BUFFER is
            // retracted entirely as of this revision -- see the
            // file-header comment in AIUpscale.cpp and plugin/README.md's
            // "既知の制約" for the full writeup, including why a
            // same-resolution AI detail-regeneration filter is this
            // effect's only robust mode against real Premiere hosts.
            // PF_OutFlag_DISPLAY_ERROR_MESSAGE was added so the host
            // actually shows out_data->return_msg in its error UI.
            0x00000104
        },

        AE_Effect_Global_OutFlags_2 {
            // Must equal out_data->out_flags2 from the same HandleGlobalSetup():
            //   out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE * 0 |
            //                           PF_OutFlag2_SUPPORTS_SMART_RENDER * 0;
            // Both terms are multiplied by 0 (neither capability is
            // implemented yet -- see README "known limitations" / roadmap),
            // so out_flags2 is 0 and this already matched; kept as 0.
            0x00000000
        },

        AE_Effect_Match_Name {
            "ADBE AI Upscale"
        },

        AE_Reserved_Info {
            0
        },

        AE_Effect_Support_URL {
            "https://example.invalid/ai-upscale" // replace with a real support URL before shipping
        }
    }
};
