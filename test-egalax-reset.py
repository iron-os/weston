#!/usr/bin/env python3
"""
Faithful, dependency-free test of the egalax-usb-reset.md approach.

Mirrors the Rust routine: scan /sys for the eGalax 0eef:c000, then issue
USBDEVFS_RESET on /dev/bus/usb/BBB/DDD. Around the reset it snapshots the
multitouch slot state (EVIOCGMTSLOTS) so you can SEE whether the stuck
tracking_ids actually cleared -- the open question from the design doc.

Run as root over SSH:  python3 test-egalax-reset.py
"""

import array
import fcntl
import glob
import os
import sys
import time

EGALAX_VID = "0eef"
EGALAX_PID = "c000"

USBDEVFS_RESET = 0x5514          # _IO('U', 20), stable across kernels

ABS_MT_SLOT = 0x2f
ABS_MT_TRACKING_ID = 0x39
EV_ABS = 0x03


def _ioc(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (ord(typ) << 8) | nr


def EVIOCGMTSLOTS(length):       # _IOC(_IOC_READ, 'E', 0x0a, len)
    return _ioc(2, 'E', 0x0a, length)


def EVIOCGABS(abs_code):         # _IOR('E', 0x40 + abs, struct input_absinfo)
    return _ioc(2, 'E', 0x40 + abs_code, 24)  # input_absinfo = 6 * int32


def find_egalax():
    """Return (busnum, devnum, syspath) or None. Same scan as the Rust."""
    for path in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(os.path.join(path, "idVendor")) as f:
                vid = f.read().strip().lower()
            with open(os.path.join(path, "idProduct")) as f:
                pid = f.read().strip().lower()
        except OSError:
            continue
        if vid == EGALAX_VID and pid == EGALAX_PID:
            with open(os.path.join(path, "busnum")) as f:
                bus = int(f.read().strip())
            with open(os.path.join(path, "devnum")) as f:
                dev = int(f.read().strip())
            return bus, dev, path
    return None


def find_event_node(syspath):
    """Find /dev/input/eventN belonging to this USB device."""
    for ev in glob.glob(os.path.join(syspath, "*/*/input/input*/event*")):
        name = os.path.basename(ev)
        if name.startswith("event"):
            return "/dev/input/" + name
    # fallback: match by name containing eGalax
    for ev in glob.glob("/dev/input/event*"):
        try:
            fd = os.open(ev, os.O_RDONLY | os.O_NONBLOCK)
            buf = bytearray(256)
            fcntl.ioctl(fd, _ioc(2, 'E', 0x06, 256), buf)  # EVIOCGNAME
            os.close(fd)
            if b"eGalax" in buf or b"eTouch" in buf:
                return ev
        except OSError:
            pass
    return None


def num_slots(fd):
    buf = array.array('i', [0] * 6)
    try:
        fcntl.ioctl(fd, EVIOCGABS(ABS_MT_SLOT), buf, True)
        return buf[2] + 1          # maximum + 1
    except OSError:
        return 0


def read_slots(event_node):
    """Return list of tracking_ids per slot, or None if unreadable."""
    try:
        fd = os.open(event_node, os.O_RDONLY | os.O_NONBLOCK)
    except OSError as e:
        print(f"  cannot open {event_node}: {e}")
        return None
    try:
        n = num_slots(fd)
        if n <= 0:
            return []
        # buffer: [ABS_MT_TRACKING_ID, slot0, slot1, ...]
        buf = array.array('i', [ABS_MT_TRACKING_ID] + [0] * n)
        fcntl.ioctl(fd, EVIOCGMTSLOTS(len(buf) * 4), buf, True)
        return list(buf[1:])
    except OSError as e:
        print(f"  EVIOCGMTSLOTS failed: {e}")
        return None
    finally:
        os.close(fd)


def show_slots(label, slots):
    if slots is None:
        print(f"  {label}: <unreadable>")
        return
    stuck = [i for i, t in enumerate(slots) if t != -1]
    print(f"  {label}: {slots}")
    if stuck:
        print(f"    -> STUCK slots {stuck} (tracking_id != -1) == fingers "
              f"the controller still thinks are down")
    else:
        print(f"    -> clean (all tracking_id == -1)")


def reset(busnum, devnum):
    node = f"/dev/bus/usb/{busnum:03d}/{devnum:03d}"
    fd = os.open(node, os.O_WRONLY)
    try:
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
        print(f"  USBDEVFS_RESET issued on {node}")
    finally:
        os.close(fd)


def main():
    found = find_egalax()
    if not found:
        print("eGalax 0eef:c000 not present")
        return 1
    bus, dev, syspath = found
    print(f"eGalax at bus {bus} dev {dev} ({syspath})")

    ev = find_event_node(syspath)
    print(f"event node: {ev}")

    print("\nBEFORE reset:")
    before = read_slots(ev) if ev else None
    show_slots("slots", before)

    print("\nResetting...")
    reset(bus, dev)
    time.sleep(0.8)              # let udev re-enumerate

    after = find_egalax()
    if after:
        bus2, dev2, syspath2 = after
        print(f"  re-enumerated at bus {bus2} dev {dev2} "
              f"(devnum {'changed' if dev2 != dev else 'UNCHANGED'})")
        ev2 = find_event_node(syspath2)
    else:
        print("  device gone after reset (still re-enumerating?)")
        ev2 = None

    print("\nAFTER reset:")
    after_slots = read_slots(ev2) if ev2 else None
    show_slots("slots", after_slots)

    print("\nVerdict:")
    if before and any(t != -1 for t in before):
        if after_slots is not None and all(t == -1 for t in after_slots):
            print("  reset CLEARED the stuck slots -> USBDEVFS_RESET is enough")
        else:
            print("  stuck slots SURVIVED the reset -> firmware ignored it; "
                  "need the HW power gate (POR)")
    else:
        print("  no stuck slots before reset -- reproduce the bad state first, "
              "then run this while it's stuck")
    return 0


if __name__ == "__main__":
    if os.geteuid() != 0:
        print("warning: not root; reset/ioctls will likely fail", file=sys.stderr)
    sys.exit(main())
