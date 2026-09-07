# photo-album

> 🚧 **Announcement — repository scaffold only.** Source code is **not published yet**.
> This repo has been created first; the code is being ported over and will land here soon.

An embedded **electronic photo-album** demo for the **Realtek RTL8721F (AmebaGreen2)** SoC,
driving a **T1720A 800×480 RGB-888** display with **GT911** capacitive touch.

JPEG photos are decoded entirely by the SoC's **MJPEG hardware decoder** (Vivante GC/HX170 + PP
post-processor) — zero CPU pixel work, no software JPEG library — and rendered fullscreen through
an **LVGL 9.3** canvas.

## What it does (planned feature set)

- **Hardware JPEG decode** — `JpegDecDecode` + PP, YCbCr→RGB and scaling done in HW, DMA'd straight
  into the LCDC framebuffers (PSRAM).
- **Two fit modes** — *cover* (center-crop, fills the screen) and *fit* (aspect-true, black
  letterbox bars). Double-tap to toggle.
- **Touch gestures** — swipe left/right for prev/next, up/down for the info bar, double-tap for fit
  mode, long-press to start a slideshow.
- **Two interchangeable photo sources** — an **SD card** (VFS + FatFS, `.jpg` files in the card root,
  hot-plug capable) when present, otherwise a built-in **flash XIP** fallback table, so the demo
  boots and runs with or without a card.

## Status & roadmap

| Milestone | Status |
|---|---|
| Create public repo & announce | ✅ done (this commit) |
| Port & publish the application source | ⏳ in progress |
| Build / flash instructions | ⏳ pending source drop |
| Tagged first release (`v0.1.0`) | ⏳ pending |

⚠️ Until the source is published, there is nothing to build from this repo — please don't file
build issues yet. Watch / star to be notified when the code lands.

## License

Planned: **Apache License 2.0** (a `LICENSE` file will accompany the source drop).
