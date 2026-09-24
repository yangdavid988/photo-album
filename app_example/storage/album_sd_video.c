/*
 * album_sd_video.c — SD-card MJPEG clip source (VFS + FatFS)
 *
 * Walks the SD card MJPEG/ directory for video folders (album_sd_scan_videos)
 * and serves one frame at a time (album_sd_video_frame).  A "video" is a
 * sub-folder holding sequentially-numbered baseline JPEG frames; the scan is
 * single-level — every direct child of MJPEG/ is a candidate, a child without
 * numbered JPEG frames is skipped.
 *
 * Playback prepares once per video (album_sd_video_prepare) by holding ONE live
 * directory cursor into the folder; frames are pulled straight off that cursor
 * with FatFS open-by-cursor (f_open_by_dir / f_dir_next), so the cost per frame
 * is O(1) — no per-frame name open, no directory re-scan (see the open-by-cursor
 * note below).  Stream order is the directory's physical readdir order, which
 * for ffmpeg/phone-produced clips equals ascending camera-counter order.
 */
#include "album_sd_video.h"

#include <stdio.h>  /* snprintf */
#include <string.h> /* strchr */

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"
#include "vfs.h"
#include "vfs_fatfs.h"
#include "storage/album_sd.h" /* album_sd_mounted() for the mount gate */

/* Open-by-cursor: old playback opened every frame by name
 * (f_open("vol:/video/00001441.jpg")).  FatFS's dir_find() walks the whole
 * directory from entry 0 on every open, so a multi-thousand-frame folder costs
 * O(N) SD sector reads per frame and read time degrades linearly with the frame
 * index.  Enumerating once and streaming from the live DIR cursor with
 * f_open_by_dir()/f_dir_next() skips path lookup entirely — O(1) per frame.
 * Stream order == directory physical order, which is safe because the frames
 * are written sequentially by ffmpeg/phone cameras.  No name table, no sort,
 * no per-frame path string. */

#ifndef TAG
#define TAG "SD_VIDEO"
#endif

/* ---- Tunables ---- */
#define SDV_MAX_VIDEOS 24    /* cap the MJPEG/ sub-folder listing      */
#define SDV_MAX_FRAMES 5120  /* max frames per video (6-digit counters) */
#define SDV_MJPEG_DIR  "MJPEG" /* video folders live under this root dir */

/* Shared PSRAM stream buffer holding ONE JPEG frame at a time.  Reuses the
 * photo path's 2 MB pool (album_sd_stream_buffer) — photo and video playback
 * are mutually exclusive modes, so the 2 MB is never needed by both at once.
 * This keeps the .psram_heap.start pool at the pre-existing 6.5 MB ceiling. */
static uint8_t* s_frame_buf   = NULL;
static uint32_t s_frame_bufsz = 0;

/* Per-video prepared state: ONE live directory cursor into the playing video's
 * folder.  Frames are streamed off it with f_open_by_dir()/f_dir_next().
 * Only one video is prepared at a time; the cursor stays open until the next
 * prepare() (or unmount). */
static void* s_dir   = NULL; /* opendir() handle of the prepared video */
static int   s_nprep = 0;    /* frame count of the prepared video      */

/* ---- Numeric counter helpers ------------------------------------------ */

/* A "counter" is the run of leading digits of the file stem (before any
 * extension or '-'/'_' separator).  "000123.JPE" → 123. */
static int stem_counter(const char* name)
{
    const char* p = name;
    unsigned    v = 0;
    int         n = 0;
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10u + (unsigned) (*p - '0');
        p++;
        n++;
    }
    return (n >= 1) ? (int) v : -1;
}

/* Is this file name a JPEG the HX170 can consume?  Accepts .jpg/.jpeg/.jpe
 * case-insensitively (8.3 names mangle the extension). */
static int is_jpeg_name(const char* fn)
{
    char* dot = strrchr(fn, '.');
    if (dot == NULL)
        return 0;
    const char* ext = dot + 1;
    if (ext[0] != 'J' && ext[0] != 'j')
        return 0;
    if (ext[1] != 'P' && ext[1] != 'p')
        return 0;
    if (ext[2] == 'G' || ext[2] == 'g')
        return 1;
    if ((ext[2] == 'E' || ext[2] == 'e') && (ext[3] == 'G' || ext[3] == 'g'))
        return 1;
    return 0;
}

/* ---- Video scan ------------------------------------------------------- */

int album_sd_scan_videos(sd_video_t* list, int max)
{
    int nv = 0;
    if (list == NULL || max <= 0)
        return 0;

    if (!album_sd_mounted())
    {
        RTK_LOGI(TAG, "video scan: no mounted SD\n");
        return 0;
    }

    char* prefix = find_vfs_tag(VFS_REGION_4);
    if (prefix == NULL || prefix[0] == '\0')
    {
        RTK_LOGE(TAG, "video scan: no VFS tag\n");
        return 0;
    }

    /* Two-pass scan.  A single loop that peeks each sub-folder (a nested
     * opendir/readdir) from inside the root readdir loop stops after the first
     * folder on long LFN names.  Collect the MJPEG/ sub-folder names first
     * (pass 1), then peek each one after the root iterator is closed (pass 2). */
    char root[VFS_PATH_MAX];
    snprintf(root, sizeof(root), "%s:%s/", prefix, SDV_MJPEG_DIR);

    /* Long names (LFN): a sub-folder name can exceed 8.3. */
    char sub[SDV_MAX_VIDEOS][64];
    int  nsub = 0;

    void* dir = opendir(root);
    if (dir == NULL)
    {
        RTK_LOGI(TAG, "video scan: opendir(\"%s\") failed (no MJPEG dir?)\n", root);
        return 0;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && nsub < SDV_MAX_VIDEOS)
    {
        if (ent->d_type != DT_DIR)
            continue; /* a video is a folder only */
        const char* nm = ent->d_name;
        if (nm[0] == '.' || strcmp(nm, "System Volume Information") == 0)
            continue;
        snprintf(sub[nsub], sizeof(sub[0]), "%s", nm);
        nsub++;
    }
    closedir(dir);

    RTK_LOGI(TAG, "  pass 1: %d subfolder(s) under %s\n", nsub, root);

    /* Pass 2: peek each collected sub-folder for numbered JPEG frames. */
    for (int i = 0; i < nsub && nv < max; i++)
    {
        const char* nm = sub[i];
        sd_video_t* v  = &list[nv];
        memset(v, 0, sizeof(*v));

        if (snprintf(v->path, sizeof(v->path), "%s%s/", root, nm) >=
            (int) sizeof(v->path))
        {
            RTK_LOGI(TAG, "  [skip] %-24s (path too long)\n", nm);
            continue;
        }
        snprintf(v->name, sizeof(v->name), "%s", nm);

        void* sdir = opendir(v->path);
        if (sdir == NULL)
        {
            RTK_LOGI(TAG, "  [skip] %-24s (not a readable folder)\n", nm);
            continue;
        }

        int frames = 0;
        struct dirent* se;
        while ((se = readdir(sdir)) != NULL && frames < SDV_MAX_FRAMES)
        {
            if (se->d_type != DT_REG)
                continue;
            if (se->d_name[0] == '.')
                continue;
            if (!is_jpeg_name(se->d_name))
                continue;
            if (stem_counter(se->d_name) < 0)
                continue;
            frames++;
        }
        closedir(sdir);

        if (frames == 0)
        {
            RTK_LOGI(TAG, "  [skip] %-24s (no numbered JPEG frames)\n", nm);
            continue;
        }

        v->frame_count = frames;
        RTK_LOGI(TAG, "  [video] %-24s %d frames\n", v->name, v->frame_count);
        nv++;
    }

    RTK_LOGI(TAG, "SD video scan: %d videos\n", nv);
    return nv;
}

/* ---- Frame prepare / stream ------------------------------------------- */

int album_sd_video_prepare(const sd_video_t* video)
{
    s_nprep = 0;
    if (video == NULL)
        return 0;

    /* Lazily bind the shared PSRAM stream pool (photo path's 2 MB buffer). */
    if (s_frame_buf == NULL)
        s_frame_buf = album_sd_stream_buffer(&s_frame_bufsz);

    /* Release a previous cursor before opening the new one. */
    if (s_dir != NULL)
    {
        closedir(s_dir);
        s_dir = NULL;
    }

    void* dir = opendir(video->path);
    if (dir == NULL)
    {
        RTK_LOGE(TAG, "prepare: opendir(\"%s\") failed\n", video->path);
        return 0;
    }

    /* The frame count is already known from album_sd_scan_videos and carried
     * in video->frame_count.  Keep ONE directory cursor open for streaming. */
    s_dir   = dir;
    s_nprep = video->frame_count;

    /* Park the cursor on the FIRST openable file — f_opendir leaves it on the
     * folder's first entry ('.'), so advance once; the first frame() call then
     * opens frame 0, not a directory entry.  FR_NO_FILE means the folder has
     * no openable entry — bail out early. */
    if (f_dir_next((DIR*) ((vfs_file*) dir)->file) != FR_OK)
    {
        RTK_LOGE(TAG, "prepare: '%s' has no openable entry\n", video->name);
        closedir(dir);
        s_dir   = NULL;
        s_nprep = 0;
        return 0;
    }

    RTK_LOGI(TAG, "'%s' prepared %d frames (cursor stream)\n", video->name,
             s_nprep);
    return s_nprep;
}

const uint8_t* album_sd_video_frame(int n, uint32_t* len)
{
    if (s_dir == NULL || n < 0 || n >= s_nprep)
    {
        RTK_LOGE(TAG, "frame index %d out of range (%d)\n", n, s_nprep);
        return NULL;
    }

    /* Open the entry the cursor currently sits on (O(1) — no dir_find path
     * walk), read it, close it, then advance the cursor to the next entry.
     * At the end of a pass the cursor sits on the EOT marker; when playback
     * wraps (n back to 0) rewind the cursor so the pass restarts at frame 0. */
    DIR* dir = (DIR*) ((vfs_file*) s_dir)->file;
    FIL  fil;

    if (n == 0)
    {
        /* f_rewinddir parks the cursor on the folder's first entry ('.') —
         * skip past it to the first frame, mirroring the prepare() park. */
        f_rewinddir(dir);
        f_dir_next(dir);
    }

    memset(&fil, 0, sizeof(fil));
    if (f_open_by_dir(&fil, dir) != FR_OK)
    {
        RTK_LOGW(TAG, "frame %d: open_by_dir skip\n", n);
        f_dir_next(dir);
        return NULL;
    }

    /* Read the whole frame without seeking to EOF first: a seek-to-END on
     * FatFS walks the file's full FAT cluster chain just to learn a size the
     * open already knows.  Read straight to EOF, bounded by the shared buffer. */
    size_t total = 0;
    while (total < (size_t) s_frame_bufsz)
    {
        UINT got = 0;
        if (f_read(&fil, s_frame_buf + total,
                   (UINT) ((size_t) s_frame_bufsz - total), &got) != FR_OK)
            break;
        if (got == 0)
            break;
        total += (size_t) got;
    }
    f_close(&fil);

    f_dir_next(dir); /* advance the cursor for the next call */

    if (total == 0)
    {
        RTK_LOGE(TAG, "frame: empty read\n");
        return NULL;
    }
    if (total >= (size_t) s_frame_bufsz) /* 2 MB cap — a frame never fills it */
    {
        RTK_LOGE(TAG, "frame: buffer full (%d bytes)\n", (int) total);
        return NULL;
    }

    /* Frame bytes were read by CPU into PSRAM — make cache coherent for the
     * hardware JPEG DMA that will consume them. */
    DCache_CleanInvalidate((u32) s_frame_buf, (u32) total);

    *len = (uint32_t) total;
    return s_frame_buf;
}
