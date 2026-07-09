//! Targeted USB reset for the eGalax `0eef:c000` touchscreen.
//!
//! Issues a `USBDEVFS_RESET` ioctl on the device node instead of tearing
//! down the whole xHCI host controller. Drop this file into a crate (it
//! needs the `libc` dependency) and call [`reset_egalax`].

use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::io::AsRawFd;

const USBDEVFS_RESET: libc::c_ulong = 0x5514; // _IO('U', 20), stable across kernels
const EGALAX_VID: &str = "0eef";
const EGALAX_PID: &str = "c000";

/// Scan `/sys/bus/usb/devices` for the eGalax and return its `(busnum, devnum)`.
fn find_egalax() -> io::Result<Option<(u32, u32)>> {
    for entry in fs::read_dir("/sys/bus/usb/devices")? {
        let path = entry?.path();

        let vendor = fs::read_to_string(path.join("idVendor"))
            .ok()
            .map(|s| s.trim().to_lowercase());
        let product = fs::read_to_string(path.join("idProduct"))
            .ok()
            .map(|s| s.trim().to_lowercase());

        if vendor.as_deref() == Some(EGALAX_VID) && product.as_deref() == Some(EGALAX_PID) {
            let bus: u32 = fs::read_to_string(path.join("busnum"))?
                .trim()
                .parse()
                .map_err(io::Error::other)?;
            let dev: u32 = fs::read_to_string(path.join("devnum"))?
                .trim()
                .parse()
                .map_err(io::Error::other)?;
            return Ok(Some((bus, dev)));
        }
    }
    Ok(None)
}

/// Issue `USBDEVFS_RESET` on the eGalax touchscreen, if present.
///
/// Returns `Ok(true)` if the reset was issued, `Ok(false)` if the device
/// is not on the bus (nothing to do). Requires write access to the
/// `/dev/bus/usb/BBB/DDD` node (root or `CAP_SYS_RAWIO`).
pub fn reset_egalax() -> io::Result<bool> {
    let Some((bus, dev)) = find_egalax()? else {
        return Ok(false);
    };

    let node = format!("/dev/bus/usb/{bus:03}/{dev:03}");
    let f = OpenOptions::new().write(true).open(&node)?;

    // SAFETY: USBDEVFS_RESET takes no argument; the fd is a valid usbfs node.
    let r = unsafe { libc::ioctl(f.as_raw_fd(), USBDEVFS_RESET) };
    if r != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(true)
}

// --- verification: read the controller's multitouch slot state -------------
//
// Use this to tell whether a reset ACTUALLY cleared the firmware's stuck
// slots. Re-enumeration alone proves nothing -- a USB reset always
// re-attaches the device. What matters is the per-slot `tracking_id`:
//   -1   => slot free
//   >= 0 => controller still asserts a finger down in that slot (the leak)
//
// Test flow: get the device stuck, `read_egalax_mt_slots()` (expect some
// id != -1), `reset_egalax()`, then read again. All -1 => firmware reset.
// Still != -1 => firmware ignored the reset; need a hardware POR.

const ABS_MT_SLOT: i32 = 0x2f;
const ABS_MT_TRACKING_ID: i32 = 0x39;
const MAX_SLOTS: usize = 16;

fn eviocgname(len: usize) -> libc::c_ulong {
    // _IOC(_IOC_READ, 'E', 0x06, len)
    (2 << 30) | ((len as libc::c_ulong) << 16) | (b'E' as libc::c_ulong) << 8 | 0x06
}

fn eviocgabs(abs: i32) -> libc::c_ulong {
    // EVIOCGABS(abs) = _IOR('E', 0x40 + abs, struct input_absinfo) (6 * i32)
    (2 << 30) | ((24) << 16) | (b'E' as libc::c_ulong) << 8 | (0x40 + abs as libc::c_ulong)
}

fn eviocgmtslots(len: usize) -> libc::c_ulong {
    // _IOC(_IOC_READ, 'E', 0x0a, len)
    (2 << 30) | ((len as libc::c_ulong) << 16) | (b'E' as libc::c_ulong) << 8 | 0x0a
}

/// Find the `/dev/input/eventN` node whose device name contains "eGalax".
fn find_egalax_event() -> io::Result<Option<String>> {
    for entry in fs::read_dir("/dev/input")? {
        let path = entry?.path();
        let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
            continue;
        };
        if !name.starts_with("event") {
            continue;
        }
        let Ok(f) = OpenOptions::new().read(true).open(&path) else {
            continue;
        };
        let mut buf = [0u8; 256];
        // SAFETY: EVIOCGNAME writes at most `buf.len()` bytes into buf.
        let r = unsafe { libc::ioctl(f.as_raw_fd(), eviocgname(buf.len()), buf.as_mut_ptr()) };
        if r < 0 {
            continue;
        }
        let dev_name = String::from_utf8_lossy(&buf);
        if dev_name.contains("eGalax") || dev_name.contains("eTouch") {
            return Ok(Some(path.to_string_lossy().into_owned()));
        }
    }
    Ok(None)
}

/// Read the eGalax's per-slot `tracking_id`s. `Ok(None)` if device absent.
pub fn read_egalax_mt_slots() -> io::Result<Option<Vec<i32>>> {
    let Some(node) = find_egalax_event()? else {
        return Ok(None);
    };
    let f = OpenOptions::new().read(true).open(&node)?;
    let fd = f.as_raw_fd();

    // How many slots does this device expose? input_absinfo.maximum + 1.
    let mut absinfo = [0i32; 6];
    // SAFETY: EVIOCGABS fills a struct input_absinfo (6 * i32).
    let r = unsafe { libc::ioctl(fd, eviocgabs(ABS_MT_SLOT), absinfo.as_mut_ptr()) };
    let nslots = if r == 0 {
        ((absinfo[2] + 1).max(0) as usize).min(MAX_SLOTS)
    } else {
        MAX_SLOTS
    };

    // Buffer: [ABS_MT_TRACKING_ID, slot0, slot1, ...]; kernel fills slots.
    let mut buf = vec![0i32; nslots + 1];
    buf[0] = ABS_MT_TRACKING_ID;
    let byte_len = buf.len() * std::mem::size_of::<i32>();
    // SAFETY: EVIOCGMTSLOTS writes `byte_len` bytes into buf.
    let r = unsafe { libc::ioctl(fd, eviocgmtslots(byte_len), buf.as_mut_ptr()) };
    if r != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(Some(buf[1..].to_vec()))
}

/// True if the controller currently asserts any finger down (a leak when
/// nothing is touching the panel).
pub fn egalax_has_stuck_slots() -> io::Result<bool> {
    Ok(read_egalax_mt_slots()?
        .map(|slots| slots.iter().any(|&id| id != -1))
        .unwrap_or(false))
}
