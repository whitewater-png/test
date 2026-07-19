// detail_upscaler.h - Classical (non-neural) edge-preserving upscaling.
//
// This is a clean-room, independent implementation of the same GENERAL
// CLASS of algorithm as Adobe After Effects' "Detail-preserving Upscale"
// effect: a fast, classical (non-machine-learning) resize that tries to
// keep edges crisp instead of just blurring them the way a plain bilinear/
// bicubic resize does. It does NOT reference, port, decompile, or reuse any
// Adobe source code, binary, or internal documentation -- it is built
// purely from well-known, decades-old, publicly documented image
// processing techniques:
//   - Lanczos windowed-sinc resampling for the base resize (Duchon, 1979;
//     a standard high-quality resampling filter described in any image
//     processing textbook and used by countless independent
//     implementations, e.g. ImageMagick, Pillow, ffmpeg's swscale).
//   - Unsharp masking (a print-darkroom technique dating to the 1930s,
//     later a standard digital image processing operation) to resynthesize
//     high-frequency detail.
//   - Edge-adaptive gain (weighting the unsharp mask's strength by local
//     gradient magnitude, via a Sobel operator) so flat/noisy regions
//     aren't over-amplified -- a standard noise-aware sharpening technique.
//   - Local min/max clamping ("halo suppression") to prevent the
//     overshoot/ringing artifacts a naive unsharp mask produces near hard
//     edges -- also a standard, widely documented technique (sometimes
//     called "clamped unsharp masking" or described as part of edge-
//     preserving smoothing literature, e.g. Gonzalez & Woods, "Digital
//     Image Processing").
//
// This exists to replace Real-ESRGAN/ONNX (see onnx_upscaler.h) as the
// DEFAULT engine for the Premiere/AE "AI Upscale" effect: the neural engine
// is far too slow for realtime/render use in Premiere (several seconds per
// 4K frame on real hardware), whereas this classical approach targets tens
// to a few hundred milliseconds per 4K frame -- see
// tests/test_detail_upscaler.cpp for an actual measured figure on this
// repo's dev CPU. The AI engine remains available as an opt-in "Engine"
// choice for users who want it (see AIUpscale.h/.cpp).
#pragma once

#include "tile.h" // ImageRGBA8

namespace upscale {

// Runs the classical detail-preserving upscale pipeline:
//   1. Base resize: separable Lanczos-3 resample of `in` to (out_w, out_h).
//      Skipped entirely (falls straight through to step 2 on an unscaled
//      copy of `in`) when out_w == in.width && out_h == in.height -- this
//      is the "detail regeneration" mode the Premiere plugin always uses
//      today (see AIUpscale.cpp HandleRender), since the plugin's output
//      world is always kept at the same size as its input world.
//   2. Detail restoration: an edge-adaptive, halo-clamped unsharp mask
//      applied to the (possibly just-copied) base image -- see
//      detail_upscaler.cpp for the full per-stage explanation.
//
// `detail_amount` (clamped to [0, 100]) linearly controls the unsharp
// mask's strength: 0 disables it entirely (output is pure Lanczos resample,
// or an exact copy of `in` in the same-size case), 50 is a "standard"
// amount, 100 is the maximum this implementation applies.
//
// Throws upscale::SizeLimitError (see size_limits.h) if `in`'s dimensions
// or the requested (out_w, out_h) violate this codebase's size safety
// limits -- validated up front, before any allocation, same convention as
// OnnxUpscaler::upscale()/upscale_tiled().
void detail_preserving_upscale(const ImageRGBA8& in,
                                ImageRGBA8& out,
                                int out_w,
                                int out_h,
                                float detail_amount);

} // namespace upscale
