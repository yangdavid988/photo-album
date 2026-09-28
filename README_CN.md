* [English Version](./README.md)

---

<div align="center">

# 📷 Ameba RTL8721F 电子相册（FreeRTOS）

</div>

[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)](https://freertos.org)
[![SDK-Ver](https://badgen.net/badge/SDK/Ameba%20RTOS/blue)](https://aiot.realmcu.com/zh/latest/rtos/index.html)
[![Platform](https://img.shields.io/badge/platform-RTL8721F-blue)]()
[![License](https://badgen.net/badge/License/Apache-2.0/lightgrey)](LICENSE)
[![Language](https://badgen.net/badge/language/C/blue)](https://github.com/yangdavid988/photo-album/search?l=c)
[![UI](https://img.shields.io/badge/LVGL-9.3-brightgreen)](https://lvgl.io)
[![Status](https://img.shields.io/badge/status-updating-yellow)]()
[![Gitee Mirror](https://badgen.net/badge/mirror/Gitee/c71d23?icon=git)](https://gitee.com/yangdavid988/photo-album)

---
<div align="center">

<img src="launcher_demo.gif" width="100%" style="max-width:1365px"
     alt="Photo Album launcher on the T1720A 800x480 panel">

</div>

---

🏞️ 基于 **Ameba RTL8721F**（AmebaGreen2）的电子相册：从 **microSD 卡**读取 JPEG 照片与 MJPEG 视频片段，并通过 **LVGL 9.3** 显示在 **800×480 TFT** 面板上。每个像素都由 **MJPEG 硬件解码器**（Vivante GC/HX170）+ **PP 后处理器** 生成——无 CPU 像素运算。照片解码到 PSRAM 帧缓冲后由 **LCDC DMA** 在安全帧边界翻转；视频则直接解码到 LCDC *尚未扫描* 的帧缓冲，播放永不撕裂。

启动器（Launcher）在两种模式之间路由：
- **JPG 相册** — 浏览照片（优先 SD 卡，flash C 数组表作为回退），支持滑动手势、亮度调节与幻灯片播放。
- **MJPEG 视频** — 播放视频片段（按序号命名的 JPEG 帧文件夹），支持点击暂停与亮度拖动。

- 📄 [芯片与模块信息](https://aiot.realmcu.com/zh/home.html) | 🌿 [Gitee 镜像](https://gitee.com/yangdavid988/photo-album) | 🛒 [淘宝购买](https://item.taobao.com/item.htm?id=1064116880996)


---

### 🛠️ 技术栈

<p>
  <img src="https://img.shields.io/badge/C-A8B9CC?style=flat-square&logo=c&logoColor=black" alt="C" />
  <img src="https://img.shields.io/badge/FreeRTOS-1D7EA8?style=flat-square&logo=freertos" alt="FreeRTOS" />
  <img src="https://img.shields.io/badge/LVGL-5A9E3E?style=flat-square&logo=lvgl" alt="LVGL" />
  <img src="https://img.shields.io/badge/Arm_Cortex-0091BD?style=flat-square&logo=arm&logoColor=white" alt="Arm" />
  <img src="https://img.shields.io/badge/MJPEG_HW-0091BD?style=flat-square&logo=videoplayer&logoColor=white" alt="MJPEG HW" />
  <img src="https://img.shields.io/badge/Ameba_IoT-00A1DE?style=flat-square&logo=wifi" alt="Ameba" />
  <img src="https://img.shields.io/badge/CMake-064F8C?style=flat-square&logo=cmake&logoColor=white" alt="CMake" />
</p>

### 🔍 Topics & Keywords

<p>
  <a href="https://github.com/topics/embedded"><img src="https://img.shields.io/badge/Embedded-555555?style=flat-square&logo=github" alt="Embedded" /></a>
  <a href="https://github.com/topics/iot"><img src="https://img.shields.io/badge/IoT-555555?style=flat-square&logo=github" alt="IoT" /></a>
  <a href="https://github.com/topics/freertos"><img src="https://img.shields.io/badge/FreeRTOS-555555?style=flat-square&logo=github" alt="FreeRTOS" /></a>
  <a href="https://github.com/topics/lvgl"><img src="https://img.shields.io/badge/LVGL-555555?style=flat-square&logo=github" alt="LVGL" /></a>
  <a href="https://github.com/topics/realtek"><img src="https://img.shields.io/badge/Realtek-555555?style=flat-square&logo=github" alt="Realtek" /></a>
  <a href="https://github.com/topics/photo-album"><img src="https://img.shields.io/badge/Photo_Album-555555?style=flat-square&logo=github" alt="Photo Album" /></a>
  <a href="https://github.com/topics/tft-display"><img src="https://img.shields.io/badge/TFT--Display-555555?style=flat-square&logo=github" alt="TFT Display" /></a>
  <a href="https://github.com/topics/embedded-video"><img src="https://img.shields.io/badge/Embedded_Video-555555?style=flat-square&logo=github" alt="Embedded Video" /></a>
</p>

---

### 🏗️ 项目结构

```
photo_album_demo/
├── app_example/
│   ├── main/app_main.c             # 启动 + LVGL 渲染循环（源自 mcu-pc-dashboard）
│   ├── core/jpeg_decode.c/.h       # MJPEG + PP 硬件解码封装（含持久会话）
│   ├── core/mjpeg_player.c/.h      # 片段播放任务：FB 乒乓、节奏控制、手势
│   ├── core/brightness_osd.c/.h    # 直接绘制到 FB 的亮度浮层
│   ├── ui/launcher_ui.c/.h         # 启动屏 + 照片/视频模式路由
│   ├── ui/album_ui.c/.h            # 画布 + 信息栏 + 手势 + 幻灯片
│   ├── ui/mjpeg_picker_ui.c/.h     # 多片段选择器（分页卡片网格）
│   ├── hal/lcd/                    # T1720A LCDC 驱动（源自 mcu-pc-dashboard）
│   ├── hal/touch/                  # GT911（源自 mcu-pc-dashboard，+ 多指和弦检测）
│   ├── hal/backlight_ctrl.c        # TIM4 PWM 背光（源自 mcu-pc-dashboard）
│   ├── storage/album_sd.c/.h       # SD 卡照片源（VFS + FatFS，热插拔）
│   ├── storage/album_sd_video.c/.h # SD 卡片段源（帧文件夹）
│   ├── assets/photos/              # ← 生成的图片表（jpg2album.py）
│   ├── assets/icons/               # ← 生成的启动器图标（gen_launcher_icons.py）
│   └── config/                     # 相册调参、LVGL 覆盖、阈值配置
├── tools/jpg2album.py              # JPEG → C 数组生成器（含 --max-kb 重新编码）
├── tools/gen_launcher_icons.py     # 启动器图标生成器（A8 alpha 掩码）
├── photos_in/                      # 放入你的 .jpg（已含 13 张示例照片）
└── Kconfig / prj.conf              # T1720A + LVGL 9.3 配置
```

---

### ✨ 功能特点

- ✅ **全链路硬件 JPEG 解码** — `JpegDecDecode` + PP 输出 ARGB8888 帧缓冲；无 CPU 像素运算、无软件解码器。设计上仅支持 4:2:0 基线 JPEG。
- ✅ **两种照片显示模式**（双击切换）— **cover** = 居中裁切，图片始终填满屏幕且不变形；**fit** = 保持宽高比完整显示，带黑色信箱条。
- ✅ **硬件 YCbCr→RGB + 缩放** 直接写入 *两个* LCDC 帧缓冲（PSRAM），帧边界翻转——无撕裂。
- ✅ **LVGL 9.3 DIRECT 渲染** + 双帧缓冲页翻转；LVGL 只负责悬浮信息栏，3 秒后自动隐藏并把对应行交还给 PP。
- ✅ **纯触摸交互**（T1720A GT911 电容屏）— 滑动、点击、双击、长按、三指和弦（无物理按键）。
- ✅ **MJPEG 片段播放** — 帧按同一硬件路径解码到*未被扫描*的帧缓冲，在帧边缘交换 DMA 指针；点击 = 暂停/继续、纵向拖动 = 亮度、三指点击 = 返回。
- ✅ **多片段选择器** — 卡上的多个视频文件夹以分页卡片网格显示；demo内置一个 1021 帧示例片段（`MJPEG/NAV`）。
- ✅ **共享亮度 OSD** — 直接绘制到帧缓冲，同一悬浮条可叠加在照片与视频帧之上。
- ✅ **照片来源** — 优先 microSD 卡（FAT32，热插拔，无需复位）；无卡时回退到 flash C 数组表（XIP）。全程无 WiFi / 网络协议栈。

---

### 🧠 工作原理

1. **照片来源（SD）** — `storage/album_sd.c` 通过 VFS + FatFS（只读）挂载卡片，枚举 `JPG/` 子文件夹（按魔数头 `0xFF 0xD8 0xFF` 判定，非扩展名），把当前照片流入共享的 2 MB PSRAM 缓冲。**回退** — 无卡时，`assets/photos/album_photos.c`（由 `tools/jpg2album.py` 生成）以 `const` C 数组形式在 flash（XIP `.rodata`）中提供 13 张照片 JPEG。
2. **启动** — `app_main.c` 依次初始化 LCD（T1720A）→ LVGL 9.3（双缓冲、DIRECT 模式）→ GT911 触摸 → `launcher_ui_init()`（相册 + 视频路由）。
3. **照片解码** — 每次切换在 LVGL 线程上执行一次同步解码：`JpegDecGetImageInfo` → 居中裁切矩形（cover）或等比缩放（letterbox）→ `PPSetConfig`（RGB32 输出，硬件裁剪+缩放）→ `JpegDecDecode` → PP 将完成的 RGB32 帧 **直接 DMA 到两个 LCDC 帧缓冲**（XRGB8888，PSRAM `.psram_heap`），全屏 800×480。
4. **信息栏** — LVGL（DIRECT）只负责 36 px 状态栏条带并仅重绘该区域；任何覆盖到照片行的绘制都触发 `LV_EVENT_REFR_READY` 钩子中的 PP 重新解码，先于帧翻转发生。
5. **视频播放** — 播放器任务为整个片段持有一个持久 `jpeg_session_open()`（HX170 + PP，无逐帧初始化），将每帧解码到*未被扫描*的帧缓冲（`lcdc_core_get_active_fb()`），随后 `lcdc_core_flush_now()` 在 FRD 边界交换 DMA 指针。`mjpeg_player_is_active()` 为真时 LVGL 循环让位，避免其刷新路径与播放器争抢帧缓冲。片段顺序取自文件名中的数字序号，而非 `readdir` 顺序（FatFS 返回 8.3 短名）。

**内存布局（播放照片或视频时的全部 PSRAM 帧缓冲）：**

| 区域 | 位置 | 大小 |
|---|---|---|
| LCDC FB ×2（LVGL 绘制缓冲） | PSRAM `.psram_heap.start` | 2 × 1.5 MB |
| 信箱条临时缓冲（fit 模式 PP 输出） | PSRAM `.psram_heap.start` | 1.5 MB |
| SD 流缓冲（当前照片 / 当前视频帧） | PSRAM `.psram_heap.start` | 2 MB |
| JPEG 数据（flash 回退相册） | Flash `.rodata`（XIP） | ~1.02 MB（13 张） |

---

### 🚀 快速开始

#### 1️⃣ 前置条件

- **RTL8721F EVB** + **T1720A** RGB LCD 模组（GT911 电容触摸）— 唯一支持的屏幕。
- **microSD 卡**，格式化为 FAT32 *不带分区表*（MBR 分区的卡会被识别为不可读），需要 `JPG/` 文件夹放照片，和/或 `MJPEG/` 文件夹放片段。
- 原生 SDK 缺少本项目需要的两块内容：**T1720A 16 MB MCM 型号的 `psram.ld` 布局**（PSRAM_END 4 MB→16 MB、KM4TZ img2 池、显式 `.psram_heap.start` 收集段）以及 **FatFS open-by-cursor 修复**（性能问题 #14 —— `f_open_by_dir()` / `f_dir_next()`，用于 O(1) 的 MJPEG 帧流式读取）。两者都包含在单条 SDK 提交中：[`c4f08b0`](https://github.com/yangdavid988/ameba-rtos/commit/c4f08b0c5e315a7f3f6827e9612cc2d8d9ab84ea)（[`feat/sdk-photo-album`](https://github.com/yangdavid988/ameba-rtos/tree/feat/sdk-photo-album) 分支顶端）。最简单的方式：将该 SDK 分支 clone 到本仓库*同级目录*。若想保留原生 SDK，可手动合入 [`c4f08b0`](https://github.com/yangdavid988/ameba-rtos/commit/c4f08b0c5e315a7f3f6827e9612cc2d8d9ab84ea) 补丁即可。

#### 2️⃣ 编译与烧录

```powershell
# 1. （可选：Linux）在 env.sh 中设置 SDK 根目录 → source env.sh
#    （Windows）env.bat 默认自动检测 SDK
.\env.bat

# 2. 编译（可加并行）— 辅助别名：bb / bp / bm
python ameba.py build

# 3. 烧录与串口监视（交互式菜单；定义 bb / bm 辅助命令）
python ameba.py flash --p COMx --image boot.bin 0x08000000 0x08040000 --image app.bin 0x08040000 0x083C0000
python ameba.py monitor --port COMx --b 1500000
```

开箱即用——`photos_in/` 中的 13 张示例照片已内嵌，无 SD 卡也能构建并运行幻灯片。

#### 3️⃣ 准备媒体（SD 卡）

> **现成素材** — 仓库自带一份可直接拷贝的卡内容到 `SDcard/`：`SDcard/JPG/` 中有 28 张照片，`SDcard/MJPEG/NAV/` 中有一个 1021 帧的示例片段（车载仪表盘视频）。将 `JPG/` 与 `MJPEG/` 的内容直接复制到 FAT32 卡（无分区表）即可。启动后 launcher 会同时显示相册与 `NAV` 片段。
>
> ⚠️ **素材声明** — `SDcard/`（以及 `photos_in/`）下的照片与视频帧均为从互联网收集、仅用于演示的示例/测试素材，版权归原作者所有；如有版权方提出异议，将按要求移除（侵删）。

- **照片** → 放入卡的 `JPG/` 文件夹（按头字节校验，任意 `0xFF D8 FF` JPEG 均可；超过 2 MB 暂存缓冲的文件会被跳过）。运行时即可换卡——热插拔基于卡检测引脚，无需复位或重新烧录。
- **视频片段** → `MJPEG/<clip>/frame_%06d.jpg`（零填充数字命名，如 ffmpeg 直接产出：
  `ffmpeg -i in.mp4 -q:v 3 MJPEG/clip/frame_%06d.jpg`）。每个子文件夹是一个片段；多个文件夹会出现在选择器中。
- **Flash 相册** → 重新生成内嵌表：`python tools/jpg2album.py photos_in/`（`--max-kb N` 通过 Pillow 重新编码超大图片）。`photos_in/` 与表需保持同步。

#### 串口日志

```c
[ALBUM_SD-I] SD mount OK
[ALBUM_SD-I] SD mounted, prefix="sdcard" (find_vfs_tag=sdcard)
[ALBUM_SD-I] scan root="sdcard:JPG", SD drv_num=0
[ALBUM_SD-I] SD photo scan: 28 usable jpg (listed 28, skipped 0)
[ALBUM_UI-I] photo source: SD card (28 photos)
[ALBUM_UI-I] album UI ready: 28 photos, full screen 800x480, bar 36px
[SD_VIDEO-I]   pass 1: 6 subfolder(s) under sdcard:MJPEG/
[SD_VIDEO-I]   [video] animation_800x480_jpg    4320 frames
[SD_VIDEO-I]   [video] movie_trailer_800x480_jpg 4800 frames
[SD_VIDEO-I]   [video] NAV                      1021 frames
[SD_VIDEO-I]   [video] nav_tiles_800x480_jpg    4320 frames
[SD_VIDEO-I]   [video] sintel                   4782 frames
[SD_VIDEO-I]   [video] tears_of_steel           4440 frames
[SD_VIDEO-I] SD video scan: 6 videos
[LAUNCHER-I] launcher ready: 6 video(s), album below
```

---

### 👆 交互方式

> **纯触摸**（T1720A GT911 电容屏；无物理按键）。

**照片模式（JPG 相册）**

| 手势 | 操作 |
|---|---|
| ← / → 滑动 | 上一张 / 下一张照片 |
| ▲ / ▼ 滑动 | 亮度 +10% / −10%（带 OSD） |
| 双击 | 切换显示模式（cover ⇄ fit） |
| 单击 | 显示 / 隐藏信息栏（3 秒后自动隐藏） |
| 长按（按住） | 切换幻灯片（5 秒间隔） |
| 三指点击 | 返回 launcher |

**视频模式（MJPEG 视频）**

| 手势 | 操作 |
|---|---|
| 单击 | 暂停 / 继续 |
| ▲ / ▼ 拖动 | 亮度 +10% / −10%（同一 OSD） |
| 三指点击 | 停止 MJPEG 播放，返回 launcher |

任意位置**三指点击**都会返回 launcher。手势在驱动层检测（GT911 保持多指计数），因此播放期间 LVGL 暂停时依然生效。

---

### ⚙️ 配置

| 项目 | 位置 | 默认值 |
|---|---|---|
| 幻灯片间隔 | `config/album_config.h` → `ALBUM_SLIDESHOW_INTERVAL_MS` | 5000 ms |
| 信息栏高度 / 自动隐藏 | `config/album_config.h` → `ALBUM_STATUSBAR_H` / `ALBUM_BAR_AUTOHIDE_MS` | 36 px / 3000 ms |
| 亮度步进 / 下限 / 开关 | `config/threshold_config.h` → `BL_STEP_PCT` / `BL_MIN_PCT` / `BRIGHTNESS_ENABLED` | 10 % / 2 % / 开 |
| 播放帧率 | `core/mjpeg_player.h` → `MJPEG_PLAY_FPS` | 24 fps |
| 片段数 / 帧数上限 | `storage/album_sd_video.c` → `SDV_MAX_VIDEOS` / `SDV_MAX_FRAMES` | 24 / 5120 |
| 信息栏颜色 / 字体 | `ui/album_ui.c` 内联（`LV_COLOR_HEX`、Montserrat） | 固定 |
| 启用的 LVGL 字体 | `config/lv_conf_project.h` | Montserrat 8–48 |
| 显示模式（cover/letterbox） | `ui/album_ui.c` → `s_fit_mode` | `FIT_COVER` |
| SD 照片文件夹 / 最大文件 | `storage/album_sd.c` → `ALBUM_SD_PHOTO_DIR`, `ALBUM_SD_BUF_SIZE` | `JPG`, 2 MB |

**Flash 预算**：图片位于 image2 XIP flash 区域——请将生成总量控制在其中。`jpg2album.py` 会打印总大小，并可借助 `--max-kb N` 重新编码超大文件（需要 Pillow）。

**SD 卡**：卡片以只读方式挂载；热插拔基于卡检测引脚，因此更换相册无需复位或重新烧录。照片按 `0xFF D8 FF` 头字节判定而非扩展名，超过 2 MB 暂存缓冲的文件会被跳过。

> `JPG/` 文件夹中若照片名也是零填充数字，会同时通过片段扫描，因此可能显示为一个可播放的静帧文件夹。

---

### 🙏 致谢 / 参考

- LCD / 触摸 / LVGL / 主题脚手架：[`mcu-pc-dashboard`](https://github.com/yangdavid988/mcu-pc-dashboard)
- 硬件 JPEG 解码流程：`ameba-rtos/example/peripheral/raw/MJPEG/raw_combined_multi_function/`
