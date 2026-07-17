// AIUpscalePiPL.r - PiPL (Plug-in Property List) resource for the
// "AI Upscale" effect, following the standard AE SDK PiPL layout used by
// sample plugins (Skeleton, SDK_Noise, etc).
//
// NOT VERIFIED IN THIS ENVIRONMENT: requires the Adobe AE SDK's
// PiPL.r / AE_Effect.r / AEConfig.h and a resource compiler (Rez on
// macOS, or the SDK's PiPLtool/CNVTPJ.EXE-based flow on Windows via the
// project's .rc wrapper) neither of which are available here. Field
// names/values below match the documented PiPL kind "eFKD" (effect)
// layout; double-check against AE_Effect.r for the SDK version in use.
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
            // -- keep in sync with AIUpscale.h's AI_UPSCALE_*_VERSION.
            524289 // PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 1) packed value; recompute via PF_VERSION macro at build time if it changes
        },

        AE_Effect_Info_Flags {
            0
        },

        AE_Effect_Global_OutFlags {
            0x02000000 // PF_OutFlag_NON_PARAM_VARY -- recompute from AE_Effect.h if flags change
        },

        AE_Effect_Global_OutFlags_2 {
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
