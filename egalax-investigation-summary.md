# eGalax stuck-touch investigation — handoff summary

## The problem
On an eGalax `0eef:c000` touchscreen (iron-os / Apollo Lake host, weston
10.0.0, chromium client, custom motion-sensor sleep/wake protocol added to
kiosk-shell), touch occasionally dies completely. Symptom in the weston log:

    touch event received with N points down but no surface focused

Restarting weston/chromium does not help — historically only a power cut did.

## Root cause (two interacting bugs)
1. **Firmware/hardware:** the controller leaks an MT slot — keeps reporting
   fingers down that aren't. Uncertain whether a USB-level reset clears it;
   may need a true power-on reset (POR).
2. **Weston:** no recovery path for `focus == NULL && num_tp > 0`. In
   `process_touch_normal()` (`libweston/input.c:2888`), only the first finger
   (`num_tp == 1`) picks focus; otherwise it logs and drops the event forever.
   The default touch cancel handler is an **empty stub** (`input.c:1067`), so
   device removal / seat release (`input.c:4312`) never resets `num_tp` or
   focus.

## Solution options (in repo)
- `egalax-stuck-touch.md` — **the real fix.** Two weston patches:
  - Patch 1 (`input.c`): re-pick focus on DOWN when `!touch->focus` (self-heal);
    make `default_grab_touch_cancel` actually reset `num_tp=0` + clear focus.
  - Patch 2 (`compositor.c`): cancel in-flight touch in `weston_compositor_sleep()`.
  - ⚠️ Patch 2 has a compile issue as written: it calls `weston_touch_cancel_grab`,
    which is **static** in `input.c` (`input.c:2204`). Must be made non-static +
    declared in a shared header, or implemented inside input.c with an exported wrapper.
- `egalax-usb-reset.md` — companion/prophylactic only. `USBDEVFS_RESET` (ioctl
  `0x5514`) on the device instead of the old `xhci_hcd` unbind hack. Cure is
  uncertain by design.

## Rust deliverable
`egalax_reset.rs` (single self-contained file, needs `libc`):
- `pub fn reset_egalax() -> io::Result<bool>` — scans /sys for 0eef:c000, issues
  USBDEVFS_RESET. Returns true=issued, false=device absent.
- `pub fn read_egalax_mt_slots() -> io::Result<Option<Vec<i32>>>` — reads per-slot
  tracking_ids via EVIOCGMTSLOTS (verification: -1 = free, >=0 = stuck finger).
- `pub fn egalax_has_stuck_slots()`.
- NOTE: ioctl encodings not yet verified on hardware — confirm a normal device
  reads all -1 before trusting a stuck reading.

(Also `test-egalax-reset.py` — same logic in Python for quick SSH testing.)

## Key findings from on-device testing (2026-06-04)
- The reset fired correctly: `usb 1-1: reset full-speed USB device number 2
  using xhci_hcd`. But it was an **in-place port reset** — NO disconnect/reconnect,
  device number stayed 2.
- The `/dev/input/by-id/` symlinks for the eGalax are stamped **23:54 (before the
  23:56 reset) and still point at event1 (if00 = touch) / event2 (mouse).** So the
  reset was **transparent to the input layer** — evdev nodes unchanged, weston still
  holds valid fds. This **rules out a re-grab problem.**
- After the reset, touch did NOT work.

## OPEN QUESTION — next step
Decide between the two remaining causes by reading the touch node directly after a
reset, then touching the screen:

    sudo cat /dev/input/event1 | xxd     # then touch the screen

- **Bytes appear** → device is fine, events reach the kernel, weston drops them →
  it's the focus/num_tp bug → apply the `egalax-stuck-touch.md` weston patches
  (the USB reset is irrelevant to the fix).
- **No bytes** → the in-place USBDEVFS_RESET leaves the controller not reporting →
  the reset is HARMFUL on this controller (only a true POR / GPIO power gate works)
  → drop the USB-reset approach.

Also run `read_egalax_mt_slots()` before/after a reset to settle whether the reset
clears the firmware's leaked slots.

## Environment notes
- iron-os, image 2026.6.1-rc, board intel/Amd64, product gate in app is "explorer".
- usbutils installed but NO `usbreset` binary (it's not part of usbutils upstream).
- Reset is invoked via `service-bootloader egalax-reset` (run with sudo).
- weston launch method (logind seat vs direct) not yet confirmed.
