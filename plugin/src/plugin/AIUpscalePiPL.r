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
            // (MAJOR=1, MINOR=0, BUG=0, STAGE=PF_Stage_DEVELOP=0, BUILD=1).
            // Verified against AE_EffectVers.h's packing:
            //   PF_VERSION(MAJOR,MINOR,BUG,STAGE,BUILD) =
            //     ((MAJOR & 0x7) << 19) | ((MINOR & 0xF) << 15) |
            //     ((BUG & 0xF) << 11) | ((STAGE & 0x3) << 9) | (BUILD & 0x1FF)
            //   = (1 << 19) | 1 = 524288 + 1 = 524289. Recompute this value
            // (and update it here) if AI_UPSCALE_*_VERSION in AIUpscale.h
            // ever changes.
            524289
        },

        AE_Effect_Info_Flags {
            0
        },

        AE_Effect_Global_OutFlags {
            // Must equal the out_data->out_flags bitmask HandleGlobalSetup()
            // (AIUpscale.cpp) sets at PF_Cmd_GLOBAL_SETUP:
            //   out_data->out_flags = PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_I_DO_DIALOG * 0;
            // The "* 0" term contributes nothing, so only
            // PF_OutFlag_NON_PARAM_VARY is actually set. Per AE_Effect.h's
            // PF_OutFlag enum (bit position, not the previous placeholder's
            // 0x02000000): PF_OutFlag_NON_PARAM_VARY = 1L << 2 = 0x00000004.
            // Recompute both this value and AIUpscale.cpp's out_flags
            // together if either side's flag combination changes -- a
            // mismatch here causes AE/Premiere to warn or refuse to load
            // the effect at startup.
            0x00000004
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
