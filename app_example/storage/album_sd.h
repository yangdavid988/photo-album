/*
 * album_sd.h — SD-card photo source (VFS + FatFS)
 *
 * Builds a photo table from the JPG/ folder of an SD card, with the same
 * descriptor shape as the flash C-array table so the UI can switch between the
 * two sources.  When no card is mounted, or it holds no photos, the caller
 * falls back to assets/photos/album_photos.
 *
 * Reference: example/storage/vfs_sdcard/example_vfs_sdcard.c
 */

#ifndef ALBUM_SD_H
#define ALBUM_SD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Same shape as album_photo_t in album_photos.h. */
typedef struct
{
    const uint8_t* data; /* JPEG stream (PSRAM buffer for SD photos) */
    uint32_t       len;
    const char*    name; /* display name (base name, no extension) */
} sd_photo_t;

/* Mount outcome, so the UI can offer the user an action instead of the driver
 * retrying (software retries do not recover a constantly-powered card). */
typedef enum
{
    SD_RES_OK        = 0, /* mounted and readable (FAT volume OK)     */
    SD_RES_NO_CARD,       /* no card present / CD says absent         */
    SD_RES_UNREADABLE,    /* card present but init/volume read failed */
} album_sd_result_t;

/* One mount attempt: no retry loop, no controller reset. */
album_sd_result_t album_sd_init(void);

/* True when an SD card is mounted and usable. */
bool album_sd_mounted(void);

/* Latest mount outcome, cached by album_sd_init(). */
album_sd_result_t album_sd_init_result(void);

/* Enumerate the JPG/ folder into the internal photo list.  A file is accepted
 * on its 0xFF 0xD8 0xFF header, not its extension (FatFS VFS returns 8.3 short
 * names).  @return photo count (0 when none / no card). */
int  album_sd_scan(void);

/* Card-detect poll transition. */
typedef enum
{
    SD_POLL_NONE        = 0, /* no state transition                */
    SD_POLL_INSERTED,        /* card inserted; mounted+scanned OK  */
    SD_POLL_REMOVED,         /* card removed; SD table torn down   */
    SD_POLL_INSERT_FAILED,   /* card inserted but mount/read failed */
} album_sd_poll_result_t;

/* React to card insert/remove edges: mount + scan, or unmount + drop the
 * table.  Edge-triggered only — a seated card is never re-mounted.  A failed
 * insert reports SD_POLL_INSERT_FAILED and the UI owns recovery.  Call from a
 * task context, never the CD ISR. */
album_sd_poll_result_t album_sd_cd_poll(void);

/* Photo count from the last album_sd_scan(). */
int  album_sd_count(void);

/* Photo i, loaded into the shared PSRAM buffer — decode it before requesting
 * another index.  NULL when out of range or the load failed. */
const sd_photo_t* album_sd_at(int index);

/* Display name of photo i, without file I/O.  NULL when out of range. */
const char* album_sd_name(int index);

#endif /* ALBUM_SD_H */
