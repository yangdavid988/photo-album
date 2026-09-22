/*
 * album_sd_video.c — SD-card MJPEG clip source (VFS + FatFS)
 *
 * Lists the card root for folders of numbered JPEG frames (album_sd_scan_videos)
 * and serves one frame at a time (album_sd_video_frame).  Frame order comes from
 * the number in each 8.3 name, never from readdir order, which would put
 * frame_2 after frame_10.
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

#ifndef TAG
#define TAG "SD_VIDEO"
#endif

/* ---- Tunables ---- */
#define SDV_MAX_VIDEOS     8  /* cap the root video-folder listing       */
#define SDV_MAX_FRAMES     1024 /* cap frames per video */

/* Shared PSRAM stream buffer holding ONE JPEG frame at a time.  Reuses the
 * photo path's 2 MB pool (album_sd_stream_buffer) — photo and video playback
 * are mutually exclusive modes, so the 2 MB is never needed by both at once.
 * This keeps the .psram_heap.start pool at the pre-existing 6.5 MB ceiling. */
static uint8_t* s_frame_buf   = NULL;
static uint32_t s_frame_bufsz = 0;

/* Per-video prepared state: sorted frame file names (8.3 SHORT names as the
 * VFS hands them out).  Only one video is prepared at a time. */
static char s_frame_names[SDV_MAX_FRAMES][16];
static int  s_prepared = 0; /* frame count of the prepared video */
static char s_prep_path[160]; /* full "vol:folder/" of the prepared video */

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

/* Simple selection sort over the prepared frame list, by camera counter. */
static void frame_sort(void)
{
    for (int i = 0; i < s_prepared; i++)
    {
        int best = i;
        for (int j = i + 1; j < s_prepared; j++)
        {
            if (stem_counter(s_frame_names[j]) <
                stem_counter(s_frame_names[best]))
                best = j;
        }
        if (best != i)
        {
            char tmp[16];
            memcpy(tmp, s_frame_names[i], sizeof(tmp));
            memcpy(s_frame_names[i], s_frame_names[best], sizeof(tmp));
            memcpy(s_frame_names[best], tmp, sizeof(tmp));
        }
    }
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

    char root[VFS_PATH_MAX];
    snprintf(root, sizeof(root), "%s:", prefix);

    void* dir = opendir(root);
    if (dir == NULL)
    {
        RTK_LOGE(TAG, "video scan: opendir(\"%s\") failed\n", root);
        return 0;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && nv < max)
    {
        if (ent->d_type != DT_DIR)
            continue; /* a video is a folder only */

        const char* nm = ent->d_name;
        if (nm[0] == '.' || strcmp(nm, "System Volume Information") == 0)
            continue;

        sd_video_t* v = &list[nv];
        memset(v, 0, sizeof(*v));

        if (snprintf(v->path, sizeof(v->path), "%s:%s/", prefix, nm) >=
            (int) sizeof(v->path))
        {
            RTK_LOGI(TAG, "  [skip] %-24s (path too long)\n", nm);
            continue;
        }
        snprintf(v->name, sizeof(v->name), "%s", nm);

        /* Peek inside: does this folder hold numbered JPEG frames? */
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
    closedir(dir);

    RTK_LOGI(TAG, "SD video scan: %d videos\n", nv);
    return nv;
}

/* ---- Frame prepare / stream ------------------------------------------- */

int album_sd_video_prepare(const sd_video_t* video)
{
    s_prepared = 0;
    if (video == NULL)
        return 0;

    /* Lazily bind the shared PSRAM stream pool (photo path's 2 MB buffer). */
    if (s_frame_buf == NULL)
        s_frame_buf = album_sd_stream_buffer(&s_frame_bufsz);

    snprintf(s_prep_path, sizeof(s_prep_path), "%s", video->path);

    void* dir = opendir(video->path);
    if (dir == NULL)
    {
        RTK_LOGE(TAG, "prepare: opendir(\"%s\") failed\n", video->path);
        return 0;
    }

    struct dirent* se;
    while ((se = readdir(dir)) != NULL && s_prepared < SDV_MAX_FRAMES)
    {
        if (se->d_type != DT_REG)
            continue;
        if (se->d_name[0] == '.')
            continue;
        if (!is_jpeg_name(se->d_name))
            continue;
        if (stem_counter(se->d_name) < 0)
            continue;
        snprintf(s_frame_names[s_prepared], sizeof(s_frame_names[0]), "%s",
                 se->d_name);
        s_prepared++;
    }
    closedir(dir);

    frame_sort();

    RTK_LOGI(TAG, "'%s' prepared %d frames\n", video->name, s_prepared);
    return s_prepared;
}

const uint8_t* album_sd_video_frame(int n, uint32_t* len)
{
    if (n < 0 || n >= s_prepared)
    {
        RTK_LOGE(TAG, "frame index %d out of range (%d)\n", n, s_prepared);
        return NULL;
    }

    char path[VFS_PATH_MAX];
    int plen = snprintf(path, sizeof(path), "%s%s", s_prep_path,
                        s_frame_names[n]);
    if (plen <= 0 || plen >= (int) sizeof(path))
        return NULL;

    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        RTK_LOGE(TAG, "frame: fopen(\"%s\") failed\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (unsigned long) size > (unsigned long) s_frame_bufsz)
    {
        RTK_LOGE(TAG, "frame: bad size %d\n", (int) size);
        fclose(f);
        return NULL;
    }

    size_t got = fread(s_frame_buf, 1, (size_t) size, f);
    fclose(f);
    if (got != (size_t) size)
    {
        RTK_LOGE(TAG, "frame: short read %d/%d\n", (int) got, (int) size);
        return NULL;
    }

    /* Frame bytes were read by CPU into PSRAM — make cache coherent for the
     * hardware JPEG DMA that will consume them. */
    DCache_CleanInvalidate((u32) s_frame_buf, (u32) size);

    *len = (uint32_t) size;
    return s_frame_buf;
}
