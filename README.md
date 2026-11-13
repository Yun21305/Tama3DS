# Tama3DS

Send Tamagotchi P's download items from an old 3DS over infrared (IrDA OBEX),
the same way IrDA phones / USB IrDA dongles do.
(maybe iD, iD L can use too)

Made by @Yun21305, 2026.

## Usage

1. Copy `Tama3DS.3dsx` to `sdmc:/3ds/` and start it from the Homebrew Launcher.
2. Put item files (`.jpg`) in `sdmc:/tama/`. Sub folders work as categories.
3. On the P's: Download -> wait screen. Point the IR windows at each other.
4. Select a file and press A.

Controls: Up/Down select, Left/Right page, A open/send, B back,
SELECT about & settings (debug log on/off), START exit (hold START to abort a transfer).

## Build

devkitPro with `3ds-dev` (libctru, citro2d, citro3d):

    make

## Credits

- **PspIrDA** by Mathieu Albinet, IrDA / IrLAP / IrLMP / TinyTP / OBEX stack.
  Based on the IrDA stack by Gerd Rausch, BlauLogic. BSD-style license, see `COPYRIGHT`.
- **stb_image** by Sean Barrett, public domain / MIT (JPEG thumbnail decoding).
- **Doridian/3dsir**, notes on the `ir:u` send behaviour (`WaitSendTransfer` cancels transfers).
- **devkitPro**: libctru, citro3d, citro2d.

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
