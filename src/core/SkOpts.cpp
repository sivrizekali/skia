/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCpu.h"
#include "SkHalf.h"
#include "SkOnce.h"
#include "SkOpts.h"

#if defined(SK_ARM_HAS_NEON)
    #define SK_OPTS_NS neon
#elif SK_CPU_SSE_LEVEL >= SK_CPU_SSE_LEVEL_SSSE3
    #define SK_OPTS_NS ssse3
#elif SK_CPU_SSE_LEVEL >= SK_CPU_SSE_LEVEL_SSE3
    #define SK_OPTS_NS sse3
#elif SK_CPU_SSE_LEVEL >= SK_CPU_SSE_LEVEL_SSE2
    #define SK_OPTS_NS sse2
#else
    #define SK_OPTS_NS portable
#endif

#include "SkBlend_opts.h"
#include "SkBlitMask_opts.h"
#include "SkBlitRow_opts.h"
#include "SkBlurImageFilter_opts.h"
#include "SkColorCubeFilter_opts.h"
#include "SkColorXform_opts.h"
#include "SkMorphologyImageFilter_opts.h"
#include "SkSwizzler_opts.h"
#include "SkTextureCompressor_opts.h"
#include "SkXfermode_opts.h"

namespace SkOpts {
    // Define default function pointer values here...
    // If our global compile options are set high enough, these defaults might even be
    // CPU-specialized, e.g. a typical x86-64 machine might start with SSE2 defaults.
    // They'll still get a chance to be replaced with even better ones, e.g. using SSE4.1.
#define DEFINE_DEFAULT(name) decltype(name) name = SK_OPTS_NS::name
    DEFINE_DEFAULT(create_xfermode);
    DEFINE_DEFAULT(color_cube_filter_span);

    DEFINE_DEFAULT(box_blur_xx);
    DEFINE_DEFAULT(box_blur_xy);
    DEFINE_DEFAULT(box_blur_yx);

    DEFINE_DEFAULT(dilate_x);
    DEFINE_DEFAULT(dilate_y);
    DEFINE_DEFAULT( erode_x);
    DEFINE_DEFAULT( erode_y);

    DEFINE_DEFAULT(texture_compressor);
    DEFINE_DEFAULT(fill_block_dimensions);

    DEFINE_DEFAULT(blit_mask_d32_a8);

    DEFINE_DEFAULT(blit_row_color32);
    DEFINE_DEFAULT(blit_row_s32a_opaque);

    DEFINE_DEFAULT(RGBA_to_BGRA);
    DEFINE_DEFAULT(RGBA_to_rgbA);
    DEFINE_DEFAULT(RGBA_to_bgrA);
    DEFINE_DEFAULT(RGB_to_RGB1);
    DEFINE_DEFAULT(RGB_to_BGR1);
    DEFINE_DEFAULT(gray_to_RGB1);
    DEFINE_DEFAULT(grayA_to_RGBA);
    DEFINE_DEFAULT(grayA_to_rgbA);
    DEFINE_DEFAULT(inverted_CMYK_to_RGB1);
    DEFINE_DEFAULT(inverted_CMYK_to_BGR1);

    DEFINE_DEFAULT(srcover_srgb_srgb);

    DEFINE_DEFAULT(color_xform_RGB1_to_2dot2);
    DEFINE_DEFAULT(color_xform_RGB1_to_srgb);
    DEFINE_DEFAULT(color_xform_RGB1_to_table);
#undef DEFINE_DEFAULT

    // Each Init_foo() is defined in src/opts/SkOpts_foo.cpp.
    void Init_ssse3();
    void Init_sse41();
    void Init_sse42() {}
    void Init_avx();
    void Init_avx2() {}

    static void init() {
    #if defined(SK_CPU_X86) && !defined(SK_BUILD_NO_OPTS)
        if (SkCpu::Supports(SkCpu::SSSE3)) { Init_ssse3(); }
        if (SkCpu::Supports(SkCpu::SSE41)) { Init_sse41(); }
        if (SkCpu::Supports(SkCpu::SSE42)) { Init_sse42(); }
        if (SkCpu::Supports(SkCpu::AVX  )) { Init_avx();   }
        if (SkCpu::Supports(SkCpu::AVX2 )) { Init_avx2();  }
    #endif
    }

    void Init() {
        static SkOnce once;
        once(init);
    }
}  // namespace SkOpts
