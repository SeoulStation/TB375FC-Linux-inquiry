# Lenovo TB375FC: vertical brightness bands on second DRM activation with Novatek firmware/reset loops

> 사용자 승인에 따라 Codex가 자동으로 게시했습니다.
> Automatically posted by Codex with the user's authorization.

I am trying to run native Linux on a Lenovo TB375FC tablet. I would appreciate help from developers familiar with MediaTek display resume or Novatek touch firmware. The display corruption remains unresolved; I am asking which state transitions to inspect next.

## Environment and observed failure

- Lenovo TB375FC PRC / P98300DA2, MT6897 / MT8792 family, 8 GB RAM.
- Original Android 14 / ZUI 16.0.20.041. The running kernel is the original BSP `6.1.25-android14-11-g4528d59d4335-ab11203497`, AArch64, with modified vendor `nt36532.ko`.
- Minimal BusyBox PID1, RAM Ubuntu ARM64 userspace. This is not a mainline kernel port or a completed standalone Ubuntu installation.
- MediaTek DRM, DSI-1, 2944×1840. The photo viewer selects an exact panel-advertised 30 Hz mode.
- A first display activation after a fresh Linux boot has worked. A later activation in the same boot can initially show the photo correctly, then develop vertical bright/dark bands. It also occurs without touching the screen.

The latest comparison was on 2026-11-24 KST. FIRST was visually stable and switched off automatically. On SECOND, the photo appeared normally, but the user saw vertical brightness corruption less than one second later. It then switched off automatically. USB control remained available. Successful log recovery and DRM cleanup did not mean that the panel was visually stable.

## Latest comparison and evidence

FIRST and SECOND used the same Linux boot, candidate module and photo payload. They were about 22 hours apart, with charging and USB reconnection between them. The viewer binaries differ: SECOND adds phase logging. This is an observed comparison, not a fully controlled causal A/B test. FIRST does not have equivalent phase/kernel-stream instrumentation.

| Captured observation | FIRST | SECOND |
|---|---:|---:|
| Visual result | Stable | Bands in less than 1 second |
| SPI trace duration | 6,000 ms | 6,239 ms |
| Observed read/write pairs | 0 / 1 | 311 / 564 |
| Trace events | 2 | 1,750 |
| Reported trace drop/overrun/missed | 0 | 0 |

All 875 observed SECOND SPI post events returned ret/status 0 with matching requested/frame/actual lengths. Nevertheless the continuous kernel stream contains **80 literal `checksum not match` messages**, 8 firmware-reset recoveries and 9 `Update firmware success` messages. The 960 lines containing the checksum function name include dumps; they are not 960 independent errors. The captured stream has 1,536 records and no internal sequence gaps; completeness of all boot history is not claimed.

Initial worker order in this run:

```text
firmware update success
→ pen 7B 00 success
→ restore entry
→ game 79 00 success
→ restore pen 7B 00 failed
→ restore exit
→ first IRQ checksum mismatch
```

Subsequent IRQ recovery loops include repeated `79` / `7B` failures and grid writes. The first observed point read with an `FD` prefix precedes the first IRQ grid write by about 271.627 ms in the same trace. That later grid write alone cannot explain the earlier FD observation. This does not identify the physical cause of the display corruption.

SECOND phase logging places `ACTIVE` at APPLY_BEGIN +270.242 ms and RESTORE_BEGIN at +5,000.113 ms. The first checksum record's reader start is about ACTIVE +429.917 ms. Reader receipt time, kernel timestamps and visible optical onset are not proven to share an exact clock. The user's less-than-one-second observation is not a synchronized measurement.

## Changes already tried

Earlier B/A/C candidates changed an initial pen conditional branch, added a grid-related lock wrapper, or combined both. They did not establish reliable later display/touch operation.

The latest candidate derives from C and replaces only the restore-grid CALL26 at **`.text+0x56d0`** with NOP, removing its relocation. IRQ `.text+0x428c` and userspace `.text+0xa4f0` grid paths remain. It is not an all-grid-disabled experiment. The candidate still inherits the pen branch NOP at `.text+0x7110`. Restoring that conditional would affect both the pen call and clearing `ts+604` (decimal, `0x25c`); the actual member meaning/value is not established.

Latest module SHA256: `e6df2e95385bbb7a03ab9f7f90f02faf2ff20614860420b36a6b4e190c1e320c`.

The same fault recurred with this candidate. The offsets are specific to this ELF and are not portable file offsets or instructions to patch another device.

## Questions

1. Which exact MediaTek DRM/panel and Novatek resume events would best distinguish an inherited/already-resumed first state from a firmware/restore path on later activation?
2. How should point IRQ processing, firmware recovery and page/command/ACK sequences be serialized above individual SPI transfers? Are there known IRQ/workqueue lock-order constraints to check?
3. How can I test whether touch firmware recovery affects the display through shared reset/power/TDDI paths, rather than assuming that a touch error causes the optical fault?
4. What ready/ACK conditions should precede IRQ enable and repeated pen/game/grid commands? In this run the initial pen command succeeds before the restore pen command fails.
5. Which normal Android baseline events, DSI/DSC/PPS state or reset/power measurements would be most useful before another one-variable experiment?

## Attached material and limits

The accompanying repository/archive contains FIRST/SECOND raw logs, the detailed Korean development diary, static module-scope analysis, selected C/assembly/Python working code, and an offline verifier. Start with `README.md` and `EVIDENCE-INDEX.md`. The full exact vendor driver C source and a complete reproducible system build are not included. The code is a review snapshot with local dependencies and historical boot bindings.

There is no synchronized optical video, full call-stack capture or physical SPI/MIPI measurement in this package. ROMs, firmware binaries, partition/calibration backups, service credentials and flashing utilities are excluded. I am asking for review of the existing evidence and a focused next diagnostic plan. Thank you.

## Historical optical photos

[Gallery, contact sheets and all 30 original photographs](evidence/photos-20261101/README.md) from the 2026-11-01 A candidate **third activation** are available. This is a separate historical trial, not the 2026-11-24 FIRST/SECOND comparison. The separately supplied frames 028 and 029 are included once each, with original bytes preserved.
