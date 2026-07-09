# eGalax USB reset

Replacement for the `xhci_hcd` PCI driver unbind hack. Issues a
targeted `USBDEVFS_RESET` on the eGalax `0EEF:C000` touchscreen
instead of tearing down the whole USB host controller.

## Reset routine

Requires `libc` in `Cargo.toml`.

```rust
use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::io::AsRawFd;

const USBDEVFS_RESET: libc::c_ulong = 0x5514; // _IO('U', 20)
const EGALAX_VID: &str = "0eef";
const EGALAX_PID: &str = "c000";

fn find_egalax() -> io::Result<Option<(u32, u32)>> {
    for entry in fs::read_dir("/sys/bus/usb/devices")? {
        let path = entry?.path();

        let vendor = fs::read_to_string(path.join("idVendor"))
            .ok()
            .map(|s| s.trim().to_lowercase());
        let product = fs::read_to_string(path.join("idProduct"))
            .ok()
            .map(|s| s.trim().to_lowercase());

        if vendor.as_deref() == Some(EGALAX_VID)
            && product.as_deref() == Some(EGALAX_PID)
        {
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

fn reset_egalax() -> io::Result<()> {
    let Some((bus, dev)) = find_egalax()? else {
        eprintln!("eGalax not present, skipping reset");
        return Ok(());
    };

    let node = format!("/dev/bus/usb/{:03}/{:03}", bus, dev);
    let f = OpenOptions::new().write(true).open(&node)?;

    let r = unsafe { libc::ioctl(f.as_raw_fd(), USBDEVFS_RESET) };
    if r != 0 {
        return Err(io::Error::last_os_error());
    }
    eprintln!("USBDEVFS_RESET issued on {node}");
    Ok(())
}
```

## Callsite

Replace the previous `xhci_hcd` unbind block with this:

```rust
match version_info() {
    Ok(v) if v.product == "explorer" => {
        if let Err(e) = reset_egalax() {
            eprintln!("could not reset eGalax: {e}");
        }
        // Give udev/libinput a moment to re-enumerate before we proceed.
        sleep(Duration::from_millis(500));
    }
    Ok(_) => {}
    Err(e) => eprintln!("could not get version info: {e}"),
}
```

After the reset, the device disappears from the USB bus and reappears
within ~100–300 ms. udev fires remove + add events; libinput closes
the old evdev node and opens the new one. With the weston patches from
[`egalax-stuck-touch.md`](./egalax-stuck-touch.md), weston handles that
transition without getting stuck.

## Notes

- The ioctl number `0x5514` is `_IO('U', 20)` and is stable across
  Linux kernel versions.
- The `/dev/bus/usb/BUS/DEV` node requires write permission; running
  as root or with `CAP_SYS_RAWIO` is sufficient.
- To verify whether the reset actually cleared the controller's slot
  state, snapshot `EVIOCGMTSLOTS` on `/dev/input/event3` before and
  after. If slot `tracking_id` values are still non-`-1` after the
  reset, the firmware ignored it and a hardware power gate is the
  only software-controllable fix.
- Worth running on every wake from sleep as a preventive measure,
  not just before reboot. The cure is uncertain; the prophylaxis is
  what may actually pay off.
