#ifndef ALBUM_SD_VIDEO_H
#define ALBUM_SD_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * album_sd_video.h — SD-card MJPEG clip source (VFS + FatFS)
 *
 * A clip is a card folder of zero-padded, sequentially numbered JPEG frames
 * (the shape ffmpeg "frame_%06d.jpg" produces).  Frames are streamed one at a
 * time into the photo source's PSRAM buffer.
 *
 * Frame order comes from the number in each file name, never from readdir
 * order: the VFS hands out 8.3 short names.  Only the playing clip's names are
 * cached, so a frame lookup never re-enumerates the folder.
 *
 * Reference: example/storage/vfs_sdcard/example_vfs_sdcard.c
 */

typedef struct
{
    char name[64];    /* folder base name, for display        */
    char path[160];   /* "vol:folder/" path for frame access  */
    int  frame_count; /* accepted frame files                 */
} sd_video_t;

/* List card folders that hold numbered JPEG frames.  Does not mount — the
 * volume must already be up (album_sd_init).  @return clips found (0 if none). */
int album_sd_scan_videos(sd_video_t* list, int max);

/* Cache one clip's frame names in playback order, up to SDV_MAX_FRAMES.
 * @return frames prepared (0 on error / not a clip). */
int album_sd_video_prepare(const sd_video_t* video);

/* Read frame n (0-based, playback order) of the prepared clip into the shared
 * PSRAM buffer.  @return the JPEG stream, valid until the next call, or NULL. */
const uint8_t* album_sd_video_frame(int n, uint32_t* len);

#endif /* ALBUM_SD_VIDEO_H */
