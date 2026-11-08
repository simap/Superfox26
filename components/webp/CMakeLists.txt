# libwebp v1.6.0 decoder (https://github.com/webmproject/libwebp, tag v1.6.0, commit
# 4fa21912338357f89e4fd51cf2368325b59e9bd9), the sources of upstream's libwebpdecoder: src/dec,
# the plain C parts of src/dsp and the decoder half of src/utils, unmodified. No encoder, no SIMD
# files (none of the WEBP_USE_* macros is set for Xtensa), no threads (WEBP_USE_THREAD is not set).
# License: COPYING and PATENTS in this directory.
# Users include "webp/decode.h".

set(dec alpha_dec.c buffer_dec.c frame_dec.c idec_dec.c io_dec.c quant_dec.c tree_dec.c vp8_dec.c
        vp8l_dec.c webp_dec.c)
set(dsp alpha_processing.c cpu.c dec.c dec_clip_tables.c filters.c lossless.c rescaler.c
        upsampling.c yuv.c)
set(utils bit_reader_utils.c color_cache_utils.c filters_utils.c huffman_utils.c palette.c
          quant_levels_dec_utils.c random_utils.c rescaler_utils.c thread_utils.c utils.c)
list(TRANSFORM dec PREPEND "src/dec/")
list(TRANSFORM dsp PREPEND "src/dsp/")
list(TRANSFORM utils PREPEND "src/utils/")

idf_component_register(SRCS ${dec} ${dsp} ${utils}
                       INCLUDE_DIRS "src"
                       PRIV_INCLUDE_DIRS ".")

# Upstream code, not ours: keep its warnings visible but not fatal.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
