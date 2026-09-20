/*
 * album_sd.c — SD-card photo source (VFS + FatFS)
 *
 * Mounts the card (VFS_REGION_4 / VFS_INF_SD), enumerates its JPG/ folder and
 * serves each JPEG through a shared PSRAM buffer, with the same descriptor
 * shape as the flash table so the UI is source-agnostic.  Files are accepted by
 * the 0xFF 0xD8 0xFF header, not by extension (see probe_file).
 *
 * One photo at a time: album_sd_at(i) loads photo i into the shared buffer, so
 * the caller must decode before requesting the next index.
 *
 * Reference: example/storage/vfs_sdcard/example_vfs_sdcard.c
 */
#include "album_sd.h"

#include <stdio.h>  /* snprintf */
#include <string.h> /* strrchr */

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"
#include "vfs.h"
#include "vfs_fatfs.h"

#ifndef TAG
#define TAG "ALBUM_SD"
#endif

/* SD handle, defined in ameba_sd.c; not exported by a public header, so the
 * SDK's own drivers redeclare it too.  Read-only here, for the card-state
 * probe in album_sd_init(). */
extern SD_HdlTypeDef hsd0;

/* ---- Tunables ---- */
#define ALBUM_SD_MAX_PHOTOS 32 /* cap the JPG/ listing            */
#define ALBUM_SD_NAME_LEN   64 /* display name (file stem) length */

/* Photos live in this card folder, never on the bare card root. */
#define ALBUM_SD_PHOTO_DIR "JPG"

/* PSRAM staging buffer for the CURRENT photo's JPEG stream; also the per-file
 * size cap enforced by probe_file().  .psram_heap.start is the section the
 * linker script pools (see PSRAM_KM4TZ_IMG2_SIZE in ameba_layout.ld). */
#define ALBUM_SD_BUF_SIZE (2 * 1024 * 1024)
__attribute__((section(".psram_heap.start"), aligned(64)))
static uint8_t s_jpeg_buf[ALBUM_SD_BUF_SIZE];

/* ---- Internal state ---- */
static bool              s_mounted     = false;
static bool              s_scanned     = false;
static bool              s_cd_present  = false; /* CD pin as last poll saw it   */
static volatile bool     s_cd_edge_pending = false; /* ISR-latched transition  */
static album_sd_result_t s_last_result = SD_RES_NO_CARD; /* latest mount outcome */
static char       s_names[ALBUM_SD_MAX_PHOTOS][ALBUM_SD_NAME_LEN];
static char       s_stem_buf[ALBUM_SD_NAME_LEN]; /* album_sd_name() scratch */
static int        s_count = 0;
static sd_photo_t s_cur   = { 0 }; /* points into s_jpeg_buf */

/* Display name = base name with the extension trimmed. */
static void stem_from_path(const char* path, char* out, size_t out_len)
{
    const char* base = path;
    const char* sep  = strrchr(path, '/');
    if (sep != NULL)
        base = sep + 1;

    snprintf(out, out_len, "%s", base);

    char* dot = strrchr(out, '.');
    if (dot != NULL)
        *dot = '\0';
}

/* The extension is not a usable gate: vfs_fatfs.c copies FILINFO.fname into
 * d_name and never the LFN, so a long "logo.jpeg" can surface as "LOGO~1.JPE".
 * Every regular file is opened and its header checked instead. */
static bool is_jpeg_stream(const uint8_t* head, size_t len)
{
    return len >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
}

typedef enum
{
    PROBE_OK       = 0, /* valid JPEG and fits the shared buffer    */
    PROBE_NOT_JPEG,     /* not a JPEG stream (bad/absent SOI magic) */
    PROBE_TOO_BIG,      /* valid JPEG but larger than s_jpeg_buf    */
} probe_result_t;

/* Look at one JPG/ file: is it a JPEG the HW decoder can take, and does the
 * stream fit s_jpeg_buf?  *size is filled when the file is readable. */
static probe_result_t probe_file(const char* path, uint32_t* size)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return PROBE_NOT_JPEG;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0)
    {
        fclose(f);
        return PROBE_NOT_JPEG;
    }
    *size = (uint32_t) sz;

    uint8_t head[3];
    size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (!is_jpeg_stream(head, got))
        return PROBE_NOT_JPEG;
    if (*size > (uint32_t) ALBUM_SD_BUF_SIZE)
        return PROBE_TOO_BIG;
    return PROBE_OK;
}

/* "sd:JPG/file.ext" — the folder must be in the path, a bare "sd:<file>"
 * resolves against the card root where the photo does not live. */
static int sd_path_for(const char* name, char* out, size_t out_len)
{
    char* prefix = find_vfs_tag(VFS_REGION_4);
    if (prefix == NULL)
        return -1;
    snprintf(out, out_len, "%s:%s/%s", prefix, ALBUM_SD_PHOTO_DIR, name);
    return 0;
}

/* Runs in the SDIO CD interrupt context: latch the transition only, no VFS/FATFS
 * work.  s_cd_present stays owned by album_sd_cd_poll() — if the ISR latched the
 * level too, the poll would see present == was and the edge would vanish. */
static void album_sd_hotplug_cb(int status)
{
    s_cd_edge_pending = true;
    RTK_LOGI(TAG, "CD callback: %s (edge latched)\n",
             status == HOTPULG_IN ? "inserted" : "removed");
}

/* Live CD pin level (low = card present); true when no CD pin is configured, so
 * the poll still converges when the edge interrupt is missed. */
static bool sd_card_present_now(void)
{
    if (sdioh_config.sdioh_cd_pin == _PNC)
        return true;

    return (GPIO_ReadDataBit(sdioh_config.sdioh_cd_pin) == 0);
}

/* One mount attempt, no retry: album_sd_cd_poll() is the only actor that
 * (re)mounts, and the UI owns recovery from a failed attempt.
 *
 * No SD_DeInit()/SD_Init() here: with CONFIG_FATFS_SD_HOTPLUG_MENU=y the SDK
 * hotplug thread already re-inits the controller on a CD edge, and a second
 * SD_Init() mid-walk drops the card out of transfer state.  SD_disk_initialize()
 * under vfs_user_register() is the only (re)init path. */
album_sd_result_t album_sd_init(void)
{
    if (s_mounted)
        return SD_RES_OK;

    fatfs_set_hotplug_usr_cb(album_sd_hotplug_cb);
    s_cd_present = sd_card_present_now();

    if (!s_cd_present)
    {
        RTK_LOGI(TAG, "SD: no card present at boot\n");
        s_last_result = SD_RES_NO_CARD;
        return SD_RES_NO_CARD;
    }

    int res = vfs_user_register("sdcard", VFS_FATFS, VFS_INF_SD, VFS_REGION_4, VFS_RW);
    if (res != 0)
    {
        RTK_LOGE(TAG, "SD mount failed (res=%d) — card present but unusable\n", res);
        vfs_user_unregister("sdcard", VFS_FATFS, VFS_INF_SD);
        s_last_result = SD_RES_UNREADABLE;
        return SD_RES_UNREADABLE;
    }
    RTK_LOGI(TAG, "SD mount OK\n");

    /* vfs_user_register() short-circuits on an existing tag, so a mount can
     * "succeed" over a dead controller — verify with direct reads. */
    SD_CardStateTypeDef cardstate = SD_GetCardState(&hsd0);
    if (cardstate != SD_CARD_TRANSFER)
    {
        RTK_LOGE(TAG, "card not in transfer state (state=%d) — mount invalid\n",
                 (int) cardstate);
        vfs_user_unregister("sdcard", VFS_FATFS, VFS_INF_SD);
        s_last_result = SD_RES_UNREADABLE;
        return SD_RES_UNREADABLE;
    }

    SD_RESULT sdret = SD_ReadBlocks(0, s_jpeg_buf, 2);

    /* Superfloppy FAT: boot sector at LBA 0 (0xEB/0xE9 jump + 0x55AA at 510).
     * An MBR-partitioned card keeps its VBR behind the LBA-0 partition record
     * and is rejected here. */
    bool fat_ok = (sdret == SD_OK) &&
                  (s_jpeg_buf[0] == 0xEB || s_jpeg_buf[0] == 0xE9) &&
                  s_jpeg_buf[510] == 0x55 && s_jpeg_buf[511] == 0xAA;
    if (!fat_ok)
    {
        RTK_LOGE(TAG, "sector0 not a FAT boot sector (ret=0x%x, jump=%02x, sig=%02x%02x)\n",
                 (int) sdret, s_jpeg_buf[0],
                 s_jpeg_buf[511], s_jpeg_buf[510]);
        vfs_user_unregister("sdcard", VFS_FATFS, VFS_INF_SD);
        s_last_result = SD_RES_UNREADABLE;
        return SD_RES_UNREADABLE;
    }

    s_mounted = true;

    char* tag = find_vfs_tag(VFS_REGION_4);
    RTK_LOGI(TAG, "SD mounted, tag=\"%s\"%s\n",
             tag != NULL ? tag : "(null)",
             (tag == NULL || tag[0] == '\0') ? " [EMPTY TAG]" : "");
    s_last_result = SD_RES_OK;
    return SD_RES_OK;
}

bool album_sd_mounted(void)
{
    return s_mounted;
}

album_sd_result_t album_sd_init_result(void)
{
    return s_last_result;
}

int album_sd_scan(void)
{
    s_count   = 0;
    s_scanned = true;

    if (!s_mounted)
        return 0;

    /* Scan JPG/ only, one folder deep, no recursion. */
    int skipped = 0; /* rejected entries (non-JPEG / too big / table full) */

    char* prefix = find_vfs_tag(VFS_REGION_4);
    if (prefix == NULL || prefix[0] == '\0')
    {
        RTK_LOGE(TAG, "find_vfs_tag empty/NULL — SD scan impossible\n");
        return 0;
    }

    /* find_vfs_tag() returns the tag without its colon; opendir needs the
     * volume prefix to resolve the path. */
    char root[VFS_PATH_MAX];
    snprintf(root, sizeof(root), "%s:%s", prefix, ALBUM_SD_PHOTO_DIR);
    RTK_LOGI(TAG, "scan root=\"%s\", SD drv_num=%d\n",
             root, FATFS_getDrivernum("SD"));

    void* dir = opendir(root);
    if (dir == NULL)
    {
        RTK_LOGE(TAG, "opendir(\"%s\") failed\n", root);

        /* Retry through FatFS with the numeric drive so the FRESULT separates
         * a driver problem from a path problem. */
        DIR d;
        char fpath[VFS_PATH_MAX];
        int drv = FATFS_getDrivernum("SD");
        if (drv >= 0)
        {
            snprintf(fpath, sizeof(fpath), "%d:/", drv);
            FRESULT fr = f_opendir(&d, fpath);
            RTK_LOGI(TAG, "  f_opendir(\"%s\") = %d\n", fpath, (int) fr);
            if (fr == FR_OK)
                f_closedir(&d);
        }
        return 0;
    }

    /* A rejected file never takes an index, so navigation has no holes. */
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL)
    {
        /* Skip directories ("System Volume Information/" on every Windows
         * formatted card) without logging them. */
        if (ent->d_type != DT_REG)
            continue;

        char path[VFS_PATH_MAX];
        if (sd_path_for(ent->d_name, path, sizeof(path)) != 0)
        {
            RTK_LOGI(TAG, "  [skip] %-28s (no VFS path)\n", ent->d_name);
            skipped++;
            continue;
        }

        uint32_t size = 0;
        probe_result_t pr = probe_file(path, &size);
        switch (pr)
        {
            case PROBE_OK:
                if (s_count >= ALBUM_SD_MAX_PHOTOS)
                {
                    RTK_LOGI(TAG, "  [skip] %-28s (table full, cap %d)\n",
                             ent->d_name, ALBUM_SD_MAX_PHOTOS);
                    skipped++;
                }
                else
                {
                    snprintf(s_names[s_count], ALBUM_SD_NAME_LEN, "%s", ent->d_name);
                    RTK_LOGI(TAG, "  [ok  ] %-28s idx=%d (%d bytes)\n",
                             ent->d_name, s_count, (int) size);
                    s_count++;
                }
                break;

            case PROBE_NOT_JPEG:
                RTK_LOGI(TAG, "  [fail] %-28s not a JPEG stream (missing 0xFF 0xD8 0xFF SOI)\n",
                         ent->d_name);
                skipped++;
                break;

            case PROBE_TOO_BIG:
                RTK_LOGI(TAG, "  [fail] %-28s %d bytes > %d-byte PSRAM buffer\n",
                         ent->d_name, (int) size, (int) ALBUM_SD_BUF_SIZE);
                skipped++;
                break;
        }
    }
    closedir(dir);

    RTK_LOGI(TAG, "SD photo scan: %d usable jpg (listed %d, skipped %d)\n",
             s_count, s_count + skipped, skipped);
    return s_count;
}

/* Unmount and drop the photo table; vfs_user_unregister() clears the VFS slot
 * too, so the next register lands in a fresh one. */
static void album_sd_unmount(void)
{
    if (!s_mounted)
    {
        s_scanned = false;
        s_count   = 0;
        return;
    }

    RTK_LOGI(TAG, "SD unmount (card removed)\n");
    vfs_user_unregister("sdcard", VFS_FATFS, VFS_INF_SD);

    s_mounted = false;
    s_scanned = false;
    s_count   = 0;
}

/* Card-detect poll — call from a task/timer context, never the CD ISR.
 *
 * Edge-triggered only: a seated card is never re-mounted and a zero-photo card
 * is never auto-recycled, because software retries never wake this board's
 * constantly-powered card and the retry loops raced the SDK hotplug thread.
 * The UI owns recovery. */
album_sd_poll_result_t album_sd_cd_poll(void)
{
    /* Compare the live pin, which only this function writes s_cd_present from. */
    s_cd_edge_pending = false;
    bool present = sd_card_present_now();
    bool was     = s_cd_present;
    s_cd_present = present;

    if (present == was)
        return SD_POLL_NONE;

    if (!present)
    {
        /* Reported even when nothing was mounted — a user pulling an
         * unmountable card is the edge the recovery dialog waits for. */
        if (s_mounted)
            album_sd_unmount();
        return SD_POLL_REMOVED;
    }

    /* Insert edge: one mount attempt, one scan. */
    s_count   = 0;
    s_scanned = false;
    if (album_sd_init() == SD_RES_OK)
    {
        album_sd_scan();
        if (s_count > 0)
            return SD_POLL_INSERTED;

        /* Mounted but enumerated nothing — readable card, unusable content. */
        RTK_LOGW(TAG, "SD mounted but 0 photos — treating as unreadable\n");
        album_sd_unmount();
        s_last_result = SD_RES_UNREADABLE;
        return SD_POLL_INSERT_FAILED;
    }

    s_last_result = SD_RES_UNREADABLE;
    RTK_LOGW(TAG, "SD insert edge: mount failed (%d), leaving unmounted\n",
             (int) album_sd_init_result());
    return SD_POLL_INSERT_FAILED;
}

int album_sd_count(void)
{
    return s_count;
}

const char* album_sd_name(int index)
{
    if (index < 0 || index >= s_count || !s_scanned)
        return NULL;
    stem_from_path(s_names[index], s_stem_buf, sizeof(s_stem_buf));
    return s_stem_buf;
}

const sd_photo_t* album_sd_at(int index)
{
    if (!s_mounted || !s_scanned || index < 0 || index >= s_count)
        return NULL;

    char path[VFS_PATH_MAX];
    if (sd_path_for(s_names[index], path, sizeof(path)) != 0)
        return NULL;

    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        RTK_LOGE(TAG, "fopen(\"%s\") failed\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (uint32_t) size > (uint32_t) sizeof(s_jpeg_buf))
    {
        RTK_LOGE(TAG, "photo \"%s\" too big (%d > %d)\n", s_names[index],
                 (int) size, (int) sizeof(s_jpeg_buf));
        fclose(f);
        return NULL;
    }

    size_t got = fread(s_jpeg_buf, 1, (size_t) size, f);
    fclose(f);
    if (got != (size_t) size)
    {
        RTK_LOGE(TAG, "short read on \"%s\" (%d/%d)\n", s_names[index], (int) got,
                 (int) size);
        return NULL;
    }

    /* CPU-written PSRAM must be clean before the JPEG/PP DMA reads it. */
    DCache_CleanInvalidate((u32) s_jpeg_buf, (u32) size);

    s_cur.data = s_jpeg_buf;
    s_cur.len  = (uint32_t) size;
    stem_from_path(s_names[index], s_stem_buf, sizeof(s_stem_buf));
    s_cur.name = s_stem_buf;
    return &s_cur;
}
