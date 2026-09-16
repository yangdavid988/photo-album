# photo_album_demo

📷 Electronic photo-album demo for the **RTL8721F (AmebaGreen2)** SoC with the **T1720A** 800×480 RGB-888 panel + GT911 capacitive touch.

JPEG images are stored **in flash as C arrays** (XIP) and decoded by the **MJPEG hardware decoder** (Vivante GC/HX170 + PP post-processor), then displayed fullscreen through an LVGL 9.3 canvas.

> ⚠️ **Hardware JPEG decoder only.** No CPU pixel work, no software fallback.
> `CONFIG_LIBJPEG_TURBO_ENABLE=y` appears in `prj.conf` because LVGL 9.3 selects it
> (see the dependency note in `Kconfig`), but this app never calls it — every pixel
> goes through the MJPEG + PP path. Decoding is 4:2:0 only, by design.

- 📄 [Chip information](https://aiot.realmcu.com/zh/product/rtl8721f.html)  
- 📎 Board purchase: [🛒 Taobao](https://item.taobao.com/item.htm?id=1064116880996) 

---

## ✨ Features

- ✅ Hardware JPEG decode (`JpegDecDecode` + PP) — no CPU pixel work, no libjpeg call path
- ✅ Two fit modes (double-tap to toggle): **cover** = center-crop, everything
  fills the screen without distortion; **fit** = aspect-true whole image with
  black letterbox bars (CPU-composited — the PP has no output stride register)
- ✅ Hardware YCbCr→RGB + scaling straight into both LCDC framebuffers (PSRAM)
- ✅ Full-screen photo, floating info bar (LVGL "FPS monitor" pattern: LVGL
  owns only the bar rect) — auto-hides after 3 s, hands its rows back to PP
- ✅ Touch gestures: ◀ / ▶ prev / next, ▲ / ▼ hide / show info bar,
  double-tap = toggle fit mode, tap = wake bar, long-press = slideshow (5 s)
- ✅ Photos embedded in flash — no SD / WiFi / filesystem needed (verification stage)

---

## 🧠 How It Works

1. `tools/jpg2album.py` converts JPEG files into a C table (`assets/photos/album_photos.c`, const → `.rodata`, XIP flash).
2. `app_main.c` boots LCD (T1720A) → LVGL 9.3 (dual-buffer page flip, DIRECT mode) → GT911 touch → `album_ui_init()`.
3. Each photo switch runs one synchronous decode on the LVGL thread:
   `JpegDecGetImageInfo` → compute center-crop rect → `PPSetConfig` (RGB32 out, crop+scale in HW) → `JpegDecDecode` → PP DMAs the finished RGB32 frame **directly into both LCDC framebuffers** (XRGB8888, PSRAM `.psram_heap` section), full screen 800×480.
   In **fit** mode PP instead scales into a scratch buffer and the CPU blits it centered with black bars (PP output has no stride).
4. LVGL (DIRECT mode) owns only the info-bar rect and repaints just that strip; anything that paints over photo rows triggers a PP re-decode in the `LV_EVENT_REFR_READY` hook, before the frame flips.

Buffer layout:

| Region | Location | Size |
|---|---|---|
| LCDC FB ×2 (LVGL draw buffers) | PSRAM `.psram_heap.start` | 2 × 1.5 MB |
| Letterbox scratch (fit-mode PP output) | PSRAM `.psram_heap.start` | 1.5 MB |
| JPEG data | Flash `.rodata` (XIP) | ~1.02 MB (13 photos) |

---

## 📁 Project Structure

```
photo_album_demo/
├── app_example/
│   ├── main/app_main.c          # boot + LVGL render loop (from pc_dashboard_demo)
│   ├── core/jpeg_decode.c/.h    # MJPEG + PP hardware decode wrapper
│   ├── ui/album_ui.c/.h         # canvas + info bar + gestures + slideshow
│   ├── hal/lcd/                 # T1720A LCDC driver (copied from pc_dashboard_demo)
│   ├── hal/touch/               # GT911 (copied)
│   ├── hal/backlight_ctrl.c     # PWM backlight (copied)
│   ├── assets/photos/           # ← generated photo tables (jpg2album.py)
│   └── config/                  # lv_conf override, album tunables
├── tools/jpg2album.py           # JPEG → C array generator
├── photos_in/                   # drop your .jpg here (13 demo photos included)
└── Kconfig / prj.conf           # T1720A + LVGL 9.3 config
```

---

## 🚀 Quick Start

The build needs the LVGL 9.3 + T1720A + MJPEG glue from my SDK fork branch
[`yangdavid988/ameba-rtos@feat/sdk-photo-album`](https://github.com/yangdavid988/ameba-rtos/tree/feat/sdk-photo-album)
(upstream `ameba-rtos` has no matching panel/LVGL target). Clone that branch
beside this repo, then:

```powershell
# 1. (optional) regenerate the photo table from photos_in/
python tools/jpg2album.py photos_in/
#    add --max-kb 64 to re-encode anything over 64 KB (needs Pillow) — this
#    changes the embedded bytes, so re-commit photos_in/ together with the table.

# 2. build & flash (interactive menu; defines the bb / bm helpers)
.\env.bat

# 3. serial log — look for:
#    JPEGDEC: src=800x480  dst=800x480  fit=2
#    JPEGDEC: Decode OK  (209429 bytes JPEG)
```

The 13 demo photos in `photos_in/` are already embedded in
`app_example/assets/photos/album_photos.c`, so a build out of the box shows a
working slideshow. That table is regenerable: `python tools/jpg2album.py
photos_in/` reproduces it byte-for-byte — keep the two in sync when you add or
remove a photo.

---

## ⚙️ Configuration

| What | Where | Default |
|---|---|---|
| Slideshow interval | `config/album_config.h` | 5000 ms |
| Photo flash budget | `tools/jpg2album.py --max-kb` | off |
| Info bar height | `config/album_config.h` → `ALBUM_STATUSBAR_H` | 36 px |
| Info bar auto-hide | `config/album_config.h` → `ALBUM_BAR_AUTOHIDE_MS` | 3000 ms |
| Bar colours / fonts | inline in `ui/album_ui.c` (`LV_COLOR_HEX`, Montserrat) | fixed |
| Enabled LVGL fonts | `config/lv_conf_project.h` | Montserrat 8–48 |
| Fit mode (stretch/contain/cover/letterbox) | `ui/album_ui.c` → `req.fit_mode` | `FIT_COVER` |

**Flash budget**: images live in the image2 XIP flash region — keep the generated total well inside it. `jpg2album.py` prints the sum and can re-encode oversized files with `--max-kb N` (Pillow required).

---

## 🗺️ Roadmap (after flash-resident verification)

- [ ] Move photo storage to **VFS**: pack a photo folder with `tools/image_scripts/vfs.py`,
      burn to an on-chip flash region, then `find_vfs_tag(VFS_REGION_1)` + `fopen/fread`
      into a PSRAM buffer (ref: `example/storage/vfs/example_vfs.c`)
- [ ] Or SD card (FatFS): `fatfs_set_hotplug_usr_cb` hotplug + `vfs_user_register` mount
      (ref: `example/storage/vfs_sdcard/example_vfs_sdcard.c`)
- [ ] Second (external) flash option (ref: `example/storage/vfs_second_flash/`)
- [ ] Thumbnail grid view page (decode thumbnails via MJPEG into a small canvas)
- [ ] EXIF orientation handling
- [ ] Fade / slide transitions between photos
- [ ] PC-side push of new photos (USB CDC channel like pc_dashboard_demo)

---

## 🙏 Credits / References

- LCD / touch / LVGL / theme scaffolding: [`pc_dashboard_demo`](https://github.com/yangdavid988/mcu-pc-dashboard)
- HW JPEG decode flow: `ameba-rtos/example/peripheral/raw/MJPEG/raw_combined_multi_function/`
