# Tama3DS

Send Tamagotchi P's download items from an old 3DS over infrared (IrDA OBEX),
the same way IrDA phones / USB IrDA dongles do.
(maybe it can be used for iD, iD L too)



![Tama3DS](Tama3DS.gif)
Made by @Yun21305, 2026.




> **Non-commercial**: This project may not be resold or used in any form of commercial activity.




## Installation


**CIA (recommended)**

Download `Tama3DS.cia` from the [Releases](../../releases) page and install it with FBI.

**3DSX (Homebrew Launcher)**

Copy `Tama3DS.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew Launcher.



## Usage

1. Put item files (`.jpg`) in `sdmc:/tama/`. Sub-folders work as categories.
2. On the P's: Download → wait screen. Point the IR windows at each other.
3. Select a file and press A to send.

Controls: Up/Down select, Left/Right page, A open/send, B back,
SELECT about & settings (debug log on/off), START exit (hold START to abort a transfer).



## Build

Requires devkitPro with `3ds-dev` (libctru, citro2d, citro3d) and
`bannertool` / `makerom` in `PATH`.

    make        # builds .3dsx
    make cia    # builds .cia (requires bannertool + makerom)

## Credits

- **PspIrDA** by Mathieu Albinet, IrDA / IrLAP / IrLMP / TinyTP / OBEX stack.
  Based on the IrDA stack by Gerd Rausch, BlauLogic. BSD-style license, see `COPYRIGHT`.
- **stb_image** by Sean Barrett, public domain / MIT (JPEG thumbnail decoding).
- **Doridian/3dsir**, notes on the `ir:u` send behaviour (`WaitSendTransfer` cancels transfers).
- **devkitPro**: libctru, citro3d, citro2d.
- **3dbrew.org**: IR documentation.

## 3DS port notes

- `iruInit` needs a 0x1000-aligned buffer (`memalign`).
- Do not use `iruSendData(..., true)`: it cancels the transfer after ~10 ms.
  Wait for `GetSendFinishedEvent` and poll `GetTransferState` instead.
- The 3DS IR often loses the first byte of a frame, both ways. Tama3DS always
  sends 10 extra BOFs and asks the peer for 48 extra BOFs and a 10 ms turnaround.
- Retries resend only the I-frame (RR + I-frame back to back collided on the
  half-duplex link). Fixed signed/unsigned length checks in the original stack.

## License

The PspIrDA / BlauLogic code keeps its original license (see `COPYRIGHT`).



---



## 3D HOME Menu Banner — Help Wanted

The 3D banner currently rotates continuously on the HOME Menu. If you know how to fix this, a PR is very welcome.

**Findings from development (8+ days of testing):**

**Crashes (`FAR 00000008`, data abort in HOME Menu):**
- An **empty `world` node** (no mesh children) causes an instant crash. The HOME Menu reads the mesh under `world`; if there is none, it dereferences a null pointer.
- A **node that has an animation AND has child nodes** causes a crash. Every animated node must be a leaf (no children). 

**Rotation (using pycgfx):**
- The HOME Menu rotates the entire scene around Y when no `world` node is present.
- Setting `BillboardMode = YAxial (5)` on the root bone does not stop the rotation in practice.

**Attempting NW4C (NintendoWare for CTR):**
- **Models overlapping at center**: All 6 parts stacked on top of each other. Caused by `BillboardMode = YAxial` being applied to intermediate group bones — this overrides their transform and pulls all children back to the origin. Limiting billboard to the root bone only resolves the overlap.
- **Model too small or too large**: Large internal units (Scale × 1500 per bone) combined with the `Magnify = 20` export option required manual calibration of the offset group scale.
- **Still rotating**: Even with correct billboard flags and scale, the HOME Menu continued to rotate the scene in some configurations.
- **No animation playing**: Despite valid skeletal animation data verified by binary inspection, no animation played on hardware. The NW4C animation format differs internally from pycgfx output; the exact reason the HOME Menu ignores it is unknown.

**Reference:**
- Texture alignment and sampler fixes: [Themezer-3DS convert_banner.py](https://github.com/ThemezerNX/Themezer-3DS)
- Mesh–node binding (`mesh_node_name`): [ClouDS Music FA convert_banner_cgfx.py](https://github.com/Epic0522/ClouDS-Music-FA)
