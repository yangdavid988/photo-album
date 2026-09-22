/*
 * Hardware JPEG decoder wrapper — RTL8721F MJPEG + PP pipeline.
 *
 * Flow (one-shot, synchronous):
 *   1. JpegDecGetImageInfo  — parse JPEG header, get src dimensions
 *   2. PPInit / PPDecCombinedModeEnable
 *   3. Compute FIT_COVER / FIT_CONTAIN / FIT_STRETCH crop rect
 *   4. PPSetConfig           — ARGB8888 output, crop + scale in HW
 *   5. JpegDecDecode         — HW decode + PP write into out_buffer
 *   6. PPDecCombinedModeDisable / release
 *
 * NOTE: out_buffer must be in PSRAM (bus address = virtual address for PSRAM
 * region on RTL8721F) and 64-byte aligned for DCache_Clean after decode.
 */

#include "core/jpeg_decode.h"

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"
#include "basetype.h"
#include "jpegdecapi.h"
#include "ppapi.h"

#ifndef TAG
#define TAG "JPEGDEC"
#endif

/* Lazily initialised once; MJPEG clock + hx170 core stay powered for demo lifetime. */
static bool g_jpeg_hw_ready = false;

/* PP RGB32 output byte order.  LVGL XRGB8888 expects 0x00RRGGBB per pixel.
 * If photos show swapped red/blue on the board, flip this to PP_PIX_FMT_BGR32. */
#ifndef PP_OUT_PIX_FMT
#define PP_OUT_PIX_FMT PP_PIX_FMT_RGB32
#endif

/* ========================================================================
 * Persistent decode session (MJPEG playback)
 *
 * Reference: ameba-rtos/example/peripheral/raw/MJPEG/raw_combined_normal.c —
 *   RCC + hx170dec_init once, JpegDecInit/PPInit/PPDecCombinedModeEnable once,
 *   then per frame only PPGetConfig/PPSetConfig + JpegDecDecode, finishing with
 *   PPDecCombinedModeDisable + PPRelease + JpegDecRelease.
 *
 * Each frame is a complete JPEG with its own SOI..EOI.  The PP output has no
 * stride register, so 1:1 full-screen frames are written tightly into the
 * caller's FB.  A frame's chroma format is re-probed per frame because MJPEG
 * encoders may switch 4:2:0/4:2:2 between frames.
 * ======================================================================== */
static void jpeg_hw_power_on(void);
static int  jpeg_pp_in_fmt(uint32_t jpeg_fmt);
struct jpeg_session_s
{
    JpegDecInst  jpegInst;
    PPInst       pp;
    PPConfig     cfg;
};

jpeg_session_t jpeg_session_open(const jpeg_session_cfg_t* cfg)
{
    if (cfg == NULL || cfg->out_w == 0 || cfg->out_h == 0)
        return NULL;

    jpeg_hw_power_on();

    jpeg_session_t s = (jpeg_session_t) rtos_mem_zmalloc(sizeof(*s));
    if (s == NULL)
    {
        RTK_LOGE(TAG, "session alloc failed\n");
        return NULL;
    }

    JpegDecRet jret = JpegDecInit(&s->jpegInst);
    if (jret != JPEGDEC_OK)
    {
        RTK_LOGE(TAG, "session JpegDecInit: %d\n", jret);
        goto fail;
    }

    PPResult pret = PPInit(&s->pp);
    if (pret != PP_OK)
    {
        RTK_LOGE(TAG, "session PPInit: %d\n", pret);
        goto fail;
    }

    pret = PPDecCombinedModeEnable(s->pp, s->jpegInst, PP_PIPELINED_DEC_TYPE_JPEG);
    if (pret != PP_OK)
    {
        RTK_LOGE(TAG, "session PPDecCombinedModeEnable: %d\n", pret);
        goto fail;
    }

    pret = PPGetConfig(s->pp, &s->cfg);
    if (pret != PP_OK)
    {
        RTK_LOGE(TAG, "session PPGetConfig: %d\n", pret);
        goto fail;
    }

    _memset(&s->cfg.ppInCrop, 0, sizeof(s->cfg.ppInCrop));
    s->cfg.ppInCrop.enable = 0;
    s->cfg.ppInRotation.rotation = PP_ROTATION_NONE;
    s->cfg.ppInImg.videoRange = 1;
    s->cfg.ppOutRgb.rgbTransform = PP_YCBCR2RGB_TRANSFORM_BT_709;
    /* Output format is single-sourced by PP_OUT_PIX_FMT (see ppapi.h) — the
     * session API deliberately doesn't expose it: the raw_combined lookups
     * expect RGB32, and exposing ppapi.h enums here would leak HW types into
     * the config struct. */
    s->cfg.ppOutImg.pixFormat = PP_OUT_PIX_FMT;
    s->cfg.ppOutImg.width = cfg->out_w;
    s->cfg.ppOutImg.height = cfg->out_h;
    s->cfg.ppOutFrmBuffer.enable = 0;  /* tight packed write, no PIP */
    s->cfg.ppOutMask1.enable = 0;
    s->cfg.ppOutMask2.enable = 0;

    /* Output alignment (ppinternal.c PPCheckAllWidth/HeightParams):
     * width % 8 == 0, height % 2 == 0, address % 8 bytes. */
    if ((cfg->out_w & 7u) || (cfg->out_h & 1u))
    {
        RTK_LOGE(TAG, "session output %dx%d not PP-aligned (w%%8, h%%2)\n",
                 (int) cfg->out_w, (int) cfg->out_h);
        goto fail;
    }

    return s;

fail:
    if (s->pp != NULL)
        PPRelease(s->pp);
    if (s->jpegInst != NULL)
        JpegDecRelease(s->jpegInst);
    rtos_mem_free(s);
    return NULL;
}

int jpeg_session_decode_frame(jpeg_session_t s,
                              const uint8_t* jpeg_data, uint32_t jpeg_len,
                              uint32_t out_fb)
{
    JpegDecInput  jpegIn;
    JpegDecOutput jpegOut;
    JpegDecImageInfo imgInfo;

    if (s == NULL || jpeg_data == NULL || jpeg_len == 0 || out_fb == 0)
        return -1;

    _memset(&jpegIn,  0, sizeof(jpegIn));
    _memset(&jpegOut, 0, sizeof(jpegOut));
    _memset(&imgInfo, 0, sizeof(imgInfo));

    jpegIn.streamBuffer.pVirtualAddress = (u32*) jpeg_data;
    jpegIn.streamBuffer.busAddress      = (u32) jpeg_data;
    jpegIn.streamLength                 = jpeg_len;
    jpegIn.bufferSize                   = 0; /* whole frame supplied at once */

    /* Probe this frame's chroma format + source size. */
    JpegDecRet jret = JpegDecGetImageInfo(s->jpegInst, &jpegIn, &imgInfo);
    if (jret != JPEGDEC_OK)
    {
        RTK_LOGE(TAG, "frame GetImageInfo: %d\n", jret);
        return -1;
    }

    int inFmt = jpeg_pp_in_fmt(imgInfo.outputFormat);
    if (inFmt < 0)
    {
        RTK_LOGE(TAG, "frame unsupported chroma 0x%x\n",
                 (unsigned int) imgInfo.outputFormat);
        return -1;
    }

    /* Frame may differ in dims/chroma per-frame in MJPEG; keep input config in
     * sync (output stays fixed to the session FG size).  Scribbling into the
     * session's cfg is fine — it is only re-sent via PPSetConfig each frame. */
    memset(&s->cfg.ppInCrop, 0, sizeof(s->cfg.ppInCrop));
    s->cfg.ppInCrop.enable = 0;
    s->cfg.ppInImg.width   = imgInfo.outputWidth;
    s->cfg.ppInImg.height  = imgInfo.outputHeight;
    s->cfg.ppInImg.pixFormat = (u32) inFmt;

    s->cfg.ppOutImg.bufferBusAddr = out_fb;

    PPResult pret = PPSetConfig(s->pp, &s->cfg);
    if (pret != PP_OK)
    {
        RTK_LOGE(TAG, "frame PPSetConfig: %d\n", pret);
        return -1;
    }

    jret = JpegDecDecode(s->jpegInst, &jpegIn, &jpegOut);
    if (jret != JPEGDEC_FRAME_READY)
    {
        RTK_LOGE(TAG, "frame JpegDecDecode: %d\n", jret);
        return -1;
    }

    /* PP wrote via DMA; CPU DCache lines for the new buffer are stale. */
    DCache_Invalidate(out_fb, s->cfg.ppOutImg.width * s->cfg.ppOutImg.height * 4u);
    return 0;
}

void jpeg_session_close(jpeg_session_t s)
{
    if (s == NULL)
        return;
    if (s->pp != NULL)
    {
        PPDecCombinedModeDisable(s->pp, s->jpegInst);
        PPRelease(s->pp);
    }
    if (s->jpegInst != NULL)
        JpegDecRelease(s->jpegInst);
    rtos_mem_free(s);
}

static void jpeg_hw_power_on(void)
{
    if (g_jpeg_hw_ready)
        return;
    RCC_PeriphClockCmd(APBPeriph_MJPEG, APBPeriph_MJPEG_CLOCK, ENABLE);
    hx170dec_init();
    g_jpeg_hw_ready = true;
    RTK_LOGI(TAG, "MJPEG HW powered on\n");
}

/* Map JPEGDEC outputFormat (jpegdecapi.h) → PP input pixFormat (ppapi.h).
 * Same mapping as SDK lv_drivers/amebagreen2/jpeg_decoder.c and ameba-claw
 * jpeg_hw.c.  MUST NOT be hardcoded: a 4:2:0 JPEG fed to PP as 4:2:2 makes
 * PP read the chroma plane with the wrong stride (full-height vs half-height)
 * → evenly-spaced horizontal color stripes on screen. */
static int jpeg_pp_in_fmt(uint32_t jpeg_fmt)
{
    switch (jpeg_fmt)
    {
        case JPEGDEC_YCbCr400:            return PP_PIX_FMT_YCBCR_4_0_0;
        case JPEGDEC_YCbCr420_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
        case JPEGDEC_YCbCr422_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_2_2_SEMIPLANAR;
        case JPEGDEC_YCbCr440:            return PP_PIX_FMT_YCBCR_4_4_0;
        case JPEGDEC_YCbCr411_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_1_1_SEMIPLANAR;
        case JPEGDEC_YCbCr444_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_4_4_SEMIPLANAR;
        default:                          return -1;
    }
}

/* ========================================================================
 * Public: peek image size from JPEG header (no full decode)
 * ======================================================================== */
int jpeg_peek_size(const uint8_t* jpeg_data, uint32_t jpeg_len, uint32_t* w, uint32_t* h)
{
    JpegDecInst inst = NULL;
    JpegDecRet  ret;
    JpegDecImageInfo info;
    JpegDecInput  in;

    jpeg_hw_power_on();

    ret = JpegDecInit(&inst);
    if (ret != JPEGDEC_OK)
    {
        RTK_LOGE(TAG, "JpegDecInit error: %d\n", ret);
        return -1;
    }

    _memset(&in, 0, sizeof(in));
    _memset(&info, 0, sizeof(info));
    in.streamBuffer.pVirtualAddress = (u32*) jpeg_data;
    in.streamBuffer.busAddress      = (u32) jpeg_data;
    in.streamLength                 = jpeg_len;
    in.bufferSize                   = 0;

    ret = JpegDecGetImageInfo(inst, &in, &info);
    JpegDecRelease(inst);

    if (ret != JPEGDEC_OK)
    {
        RTK_LOGE(TAG, "GetImageInfo error: %d\n", ret);
        return -2;
    }

    if (w != NULL)
        *w = info.outputWidth;
    if (h != NULL)
        *h = info.outputHeight;
    return 0;
}

/* ========================================================================
 * Public: decode JPEG → ARGB8888 with HW crop + scale
 * ======================================================================== */
int jpeg_decode_to_argb8888(const jpeg_dec_req_t* req)
{
    JpegDecInst   jpegInst = NULL;
    JpegDecRet    jpegRet;
    JpegDecImageInfo imgInfo;
    JpegDecInput  jpegIn;
    JpegDecOutput jpegOut;
    PPInst        pp   = NULL;
    PPResult      ppRet;
    PPConfig      cfg;
    int           rc   = -1;

    if (req == NULL || req->jpeg_data == NULL || req->out_buffer == NULL)
        return -1;
    if (req->out_w == 0 || req->out_h == 0)
        return -1;

    jpeg_hw_power_on();

    _memset(&jpegIn,  0, sizeof(jpegIn));
    _memset(&jpegOut, 0, sizeof(jpegOut));
    _memset(&imgInfo, 0, sizeof(imgInfo));
    _memset(&cfg,     0, sizeof(cfg));

    /* --- Init JPEG decoder --- */
    jpegRet = JpegDecInit(&jpegInst);
    if (jpegRet != JPEGDEC_OK)
    {
        RTK_LOGE(TAG, "JpegDecInit: %d\n", jpegRet);
        goto end;
    }

    jpegIn.streamBuffer.pVirtualAddress = (u32*) req->jpeg_data;
    jpegIn.streamBuffer.busAddress      = (u32)  req->jpeg_data;
    jpegIn.streamLength                 = req->jpeg_len;
    jpegIn.bufferSize                   = 0;

    jpegRet = JpegDecGetImageInfo(jpegInst, &jpegIn, &imgInfo);
    if (jpegRet != JPEGDEC_OK)
    {
        RTK_LOGE(TAG, "GetImageInfo: %d\n", jpegRet);
        goto end;
    }

    uint32_t srcW = imgInfo.outputWidth;
    uint32_t srcH = imgInfo.outputHeight;
    RTK_LOGI(TAG, "src=%dx%d  dst=%dx%d  fit=%d\n",
             (int) srcW, (int) srcH,
             (int) req->out_w, (int) req->out_h,
             (int) req->fit_mode);

    /* --- Init PP --- */
    ppRet = PPInit(&pp);
    if (ppRet != PP_OK)
    {
        RTK_LOGE(TAG, "PPInit: %d\n", ppRet);
        goto end;
    }

    ppRet = PPDecCombinedModeEnable(pp, jpegInst, PP_PIPELINED_DEC_TYPE_JPEG);
    if (ppRet != PP_OK)
    {
        RTK_LOGE(TAG, "PPDecCombinedModeEnable: %d\n", ppRet);
        goto end;
    }

    ppRet = PPGetConfig(pp, &cfg);
    if (ppRet != PP_OK)
    {
        RTK_LOGE(TAG, "PPGetConfig: %d\n", ppRet);
        goto end;
    }

    /* --- Input config (semi-planar YUV from JPEG decoder, format per image) --- */
    int inFmt = jpeg_pp_in_fmt(imgInfo.outputFormat);
    if (inFmt < 0)
    {
        RTK_LOGE(TAG, "unsupported JPEG chroma fmt 0x%x\n",
                 (unsigned int) imgInfo.outputFormat);
        goto end;
    }
    RTK_LOGI(TAG, "jpeg chroma fmt 0x%x -> pp 0x%x\n",
             (unsigned int) imgInfo.outputFormat, (unsigned int) inFmt);

    cfg.ppInImg.width     = srcW;
    cfg.ppInImg.height    = srcH;
    cfg.ppInImg.pixFormat = (u32) inFmt;
    cfg.ppInImg.videoRange = 1;

    /* Windowed = PP writes an out_w×out_h block INSIDE a larger FB.
     * Implemented as a plain tight write at an offset address: valid only
     * when the block spans the full FB row (dst_x==0, out_w==stride), so
     * consecutive PP output rows coincide with FB rows.  (The PP PIP /
     * PPOutFrameBuffer path demands 16px-aligned origins & strides and
     * mis-scales the picture — avoided on purpose.)                          */
    int windowed = (req->stride != 0 && req->fb_h != 0);
    if (windowed && (req->dst_x != 0 || req->out_w != req->stride))
    {
        RTK_LOGE(TAG, "windowed write needs dst_x==0 && out_w==stride\n");
        goto end;
    }

    uint8_t* dst_ptr = (uint8_t*) req->out_buffer;
    if (windowed)
        dst_ptr += (size_t) req->dst_y * req->stride * 4u;

    /* PP alignment (ppinternal.c PPCheckAllWidth/HeightParams):
     * output width % WIDTH_MULTIPLE(8) == 0, height % HEIGHT_MULTIPLE(2) == 0,
     * address % 8 bytes.  Round down so we never write outside the target. */
    uint32_t outW = req->out_w & ~7u;
    uint32_t outH = req->out_h & ~1u;
    if (outW == 0 || outH == 0)
        goto end;

    /* --- Output config: RGB32 (XRGB8888), tight packed write at dst_ptr --- */
    cfg.ppOutImg.pixFormat     = PP_OUT_PIX_FMT;
    cfg.ppOutImg.width         = outW;
    cfg.ppOutImg.height        = outH;
    cfg.ppOutImg.bufferBusAddr = (u32) dst_ptr;

    cfg.ppOutRgb.rgbTransform = PP_YCBCR2RGB_TRANSFORM_BT_709;

    /* --- Crop calculation (FIT_COVER: center-crop to destination aspect) --- */
    cfg.ppInCrop.enable = 0;

    if (req->fit_mode == FIT_COVER && srcW > 0 && srcH > 0)
    {
        /*
         * FIT_COVER: crop the SOURCE so its aspect matches the destination,
         * then PP scales the cropped rect to fill the output exactly.
         *
         *   dst_ratio = out_w / out_h
         *   if src_ratio > dst_ratio:  crop width
         *   else:                      crop height
         */
        /* PP crop constraints (ppinternal.c:871):
         *   crop width & height  → multiple of 8
         *   crop originX/originY → multiple of 16
         * So use the alignment-rounded output (outW×outH) as the target
         * aspect, floor the crop size to 8 and the centre origin to 16. */
        uint32_t cropW = srcW & ~7u;
        uint32_t cropH = srcH & ~7u;

        /* Compare srcW*destH vs srcH*destW to avoid float division */
        uint64_t lhs = (uint64_t) srcW * (uint64_t) outH;
        uint64_t rhs = (uint64_t) srcH * (uint64_t) outW;

        if (lhs > rhs)
        {
            /* Source is wider — trim width to match dest aspect */
            cropW = (uint32_t) (((uint64_t) srcH * (uint64_t) outW) / (uint64_t) outH) & ~7u;
        }
        else if (rhs > lhs)
        {
            /* Source is taller — trim height */
            cropH = (uint32_t) (((uint64_t) srcW * (uint64_t) outH) / (uint64_t) outW) & ~7u;
        }
        /* lhs == rhs: aspect already matches, no crop needed */

        if (cropW < 16u)
            cropW = 16u;
        if (cropH < 16u)
            cropH = 16u;
        if (cropW > srcW)
            cropW = srcW & ~7u;
        if (cropH > srcH)
            cropH = srcH & ~7u;

        if (cropW != srcW || cropH != srcH)
        {
            /* Centre the crop; origin floored to 16.  Because crop size is a
             * multiple of 8 and the origin is floored (never raised), origin +
             * size stays inside the source. */
            uint32_t ox = ((srcW - cropW) / 2u) & ~15u;
            uint32_t oy = ((srcH - cropH) / 2u) & ~15u;

            cfg.ppInCrop.enable  = 1;
            cfg.ppInCrop.width   = cropW;
            cfg.ppInCrop.height  = cropH;
            cfg.ppInCrop.originX = ox;
            cfg.ppInCrop.originY = oy;

            RTK_LOGI(TAG, "FIT_COVER crop: %dx%d @(%d,%d)\n",
                     (int) cropW, (int) cropH,
                     (int) cfg.ppInCrop.originX,
                     (int) cfg.ppInCrop.originY);
        }
    }

    /* 1:1 passthrough: force an identity crop so PP's crop pipeline always
     * runs, at its 8-px alignment.  This was added against a JpegDecDecode -7
     * on some 1:1 sources; testing the bundled photos later showed the -7
     * tracks the stream's Huffman table family (and/or a short source height),
     * not the 1:1 case.  Harmless, so it stays. */
    if (outW == srcW && outH == srcH)
    {
        uint32_t cW = srcW & ~7u;
        uint32_t cH = srcH & ~7u;
        cfg.ppInCrop.enable  = 1;
        cfg.ppInCrop.width   = cW;
        cfg.ppInCrop.height  = cH;
        cfg.ppInCrop.originX = 0;
        cfg.ppInCrop.originY = 0;
        RTK_LOGI(TAG, "1:1 passthrough -> identity crop %dx%d\n",
                 (int) cW, (int) cH);
    }
    /* FIT_STRETCH / FIT_CONTAIN: no crop — PP stretches to fill output rect */

    cfg.ppOutFrmBuffer.enable = 0; /* tight packed write (no PIP compositing) */
    cfg.ppOutMask1.enable = 0;
    cfg.ppOutMask2.enable = 0;

    ppRet = PPSetConfig(pp, &cfg);
    if (ppRet != PP_OK)
    {
        RTK_LOGE(TAG, "PPSetConfig: %d\n", ppRet);
        goto end;
    }

    /* --- Decode --- */
    jpegRet = JpegDecDecode(jpegInst, &jpegIn, &jpegOut);
    if (jpegRet != JPEGDEC_FRAME_READY)
    {
        RTK_LOGE(TAG, "JpegDecDecode: %d\n", jpegRet);
        goto end;
    }

    /* PP writes via DMA bus — CPU DCache still holds stale lines.
     * Invalidate exactly the written rectangle so a later LVGL draw into
     * the same buffer (e.g. the status bar) doesn't write back stale data.
     * dst_ptr is 64B-aligned (FB base aligned; offset = dst_y*stride*4, a
     * multiple of 32 in our configs) and the size is a multiple of 32. */
    DCache_Invalidate((u32) dst_ptr, outW * outH * 4u);

    RTK_LOGI(TAG, "Decode OK  (%d bytes JPEG)\n", (int) req->jpeg_len);
    rc = 0;

end:
    if (pp != NULL)
    {
        PPDecCombinedModeDisable(pp, jpegInst);
        PPRelease(pp);
    }
    if (jpegInst != NULL)
        JpegDecRelease(jpegInst);

    return rc;
}
