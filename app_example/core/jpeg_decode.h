#ifndef JPEG_DECODE_H
#define JPEG_DECODE_H

#include <stdint.h>

/* ========================================================================
 * Hardware JPEG decoder wrapper (RTL8721F MJPEG + PP post-processor)
 *
 * Decodes a JPEG byte stream (flash C array, or SD via a PSRAM staging
 * buffer) and writes the result as ARGB8888 straight into a caller-provided
 * PSRAM buffer that is also the LVGL canvas backing store.  The PP block
 * crops-to-fill + scales in hardware, so the output always fills the target
 * rect without letterbox bars and without distorting the aspect ratio.
 * jpeg_data stays const for both sources.
 *
 * Reference: ameba-rtos/example/peripheral/raw/MJPEG/raw_combined_multi_function
 * ======================================================================== */

typedef struct
{
    const uint8_t* jpeg_data; /* JPEG stream, flash- or PSRAM-resident */
    uint32_t       jpeg_len;  /* stream length in bytes               */

    void*    out_buffer; /* destination FB base (PSRAM)                    */
    uint32_t out_w;      /* output width  (pixels)                       */
    uint32_t out_h;      /* output height (pixels)                       */

    /* Windowed output into a larger framebuffer (PP PPOutFrameBuffer mode).
     * Set stride = FB line width in px (0 = tight out_w×out_h buffer).
     * writeOrigin = (dst_x, dst_y) inside the FB; fb_h = FB total height. */
    uint32_t stride;
    uint32_t fb_h;
    uint32_t dst_x;
    uint32_t dst_y;

    uint8_t fit_mode; /* FIT_STRETCH / FIT_CONTAIN / FIT_COVER (see below) */
} jpeg_dec_req_t;

/* Fit modes for scaling the source image into the output rect. */
#define FIT_STRETCH    0 /* scale W and H independently (may distort)  */
#define FIT_CONTAIN    1 /* whole image fits, PP leaves padding (best-effort) */
#define FIT_COVER      2 /* crop-to-fill center, no bars, no distortion (default) */
#define FIT_LETTERBOX  3 /* aspect-true scale, caller composites black bars
                          * (decode into a tight scratch buffer, no FB window) */

/* @return 0 on success, negative on error. */
int jpeg_decode_to_argb8888(const jpeg_dec_req_t* req);

/* Probe image dimensions without a full decode.
 * @return 0 on success; *w* *h filled when non-NULL. */
int jpeg_peek_size(const uint8_t* jpeg_data, uint32_t jpeg_len, uint32_t* w, uint32_t* h);

/* ========================================================================
 * Persistent decode session — for MJPEG video playback.
 *
 * A single JpegDecInit + PPInit + PPDecCombinedModeEnable lives for the whole
 * playback (the SDK's raw_combined_normal pattern), instead of the per-photo
 * init/decode/release of jpeg_decode_to_argb8888().  Frames cost only the
 * PPSetConfig + JpegDecDecode calls.
 *
 * Frame → screen is a pure ping-pong through the caller's output FBs:
 *   session_open(current)  → decode into fb[cur]
 *   while playing:         → decode into fb[!cur], then fb[cur] flips to screen
 *
 * The caller owns the two output framebuffers (full-screen tight ARGB8888):
 *  - output 0 is decoded first, then presented (the LCDC already scans the
 *    OTHER buffer), so no torn frame is ever shown.
 * ======================================================================== */

typedef struct
{
    uint32_t out_w;      /* output width  (pixels) — tight write        */
    uint32_t out_h;      /* output height (pixels)                      */
} jpeg_session_cfg_t;

/* Opaque session handle. */
typedef struct jpeg_session_s* jpeg_session_t;

/* Create / tear down a persistent decode+PP session bound to a fixed output
 * size.
 * @param cfg session config (output size / pixel format)
 * @return opaque handle, or NULL on failure (device busy / init error). */
jpeg_session_t jpeg_session_open(const jpeg_session_cfg_t* cfg);

/* Decode one complete JPEG frame into the caller-provided output FB.
 *
 * The caller picks out_fb — the MJPEG player always points it at the LCDC
 * buffer that is NOT being scanned, so the hardware never half-paints a
 * visible frame (the session itself owns no output routing).
 *
 * @param session     session from jpeg_session_open
 * @param jpeg_data   frame stream (PSRAM/CPU buffer, bus == virtual)
 * @param jpeg_len    frame length
 * @param out_fb      destination framebuffer bus address (tight full-screen,
 *                    PP-aligned — same geometry as jpeg_session_open)
 * @return 0 on success; -1 on decode/config error. */
int jpeg_session_decode_frame(jpeg_session_t session,
                              const uint8_t* jpeg_data, uint32_t jpeg_len,
                              uint32_t out_fb);

/* Close a session, releasing the HX170 + PP. */
void jpeg_session_close(jpeg_session_t session);

#endif /* JPEG_DECODE_H */
