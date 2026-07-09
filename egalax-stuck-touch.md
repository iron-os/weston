# eGalax touchscreen stuck-slot recovery

Notes on a sticky failure mode where touch input stops working after a
sleep/wake cycle on an eGalax `0EEF:C000` touchscreen (Apollo Lake host,
weston 10.0.0). The symptom is the log line:

    touch event received with 3 points down but no surface focused

after which restarting weston or the client (chromium) does not help —
only a full power cut clears it.

## Root cause (short version)

Two bugs interact:

1. The touch controller's firmware leaks an MT slot — its internal
   bookkeeping says fingers are still down when they aren't. A USB
   protocol-level reset does not clear it on this controller; only
   power-on reset (POR) does.
2. Weston has no recovery path for `focus == NULL && num_tp > 0`. Once
   it lands there, every subsequent `WL_TOUCH_DOWN` hits the
   "no surface focused" branch and is dropped permanently.

Patch (2) so that weston can recover gracefully even when (1) happens.

## Patch 1 — `libweston/input.c`

Recover focus mid-session when it has been lost, and make the default
cancel handler actually reset state so any future cancel path (sleep,
seat release, etc.) clears `num_tp` and focus.

```diff
 static void
 default_grab_touch_cancel(struct weston_touch_grab *grab)
 {
+    struct weston_touch *touch = grab->touch;
+
+    touch->num_tp = 0;
+    weston_touch_set_focus(touch, NULL);
 }
```

```diff
     switch (touch_type) {
     case WL_TOUCH_DOWN:
         /* the first finger down picks the view, and all further go
          * to that view for the remainder of the touch session i.e.
-         * until all touch points are up again. */
-        if (touch->num_tp == 1) {
+         * until all touch points are up again.
+         *
+         * If focus was lost mid-session (focused surface destroyed,
+         * or libinput counter drifted because the controller reported
+         * spurious DOWNs without matching UPs), recover by picking a
+         * new view on this DOWN instead of dropping the event. */
+        if (touch->num_tp == 1 || !touch->focus) {
             ev = weston_compositor_pick_view(ec, *pos);
             weston_touch_set_focus(touch, ev);
-        } else if (!touch->focus) {
-            /* Unexpected condition: We have non-initial touch but
-             * there is no focused surface.
-             */
-            weston_log("touch event received with %d points down "
-                       "but no surface focused\n", touch->num_tp);
+        }
+
+        if (!touch->focus) {
+            weston_log("touch DOWN with %d points but nothing to focus\n",
+                       touch->num_tp);
             return;
         }
```

## Patch 2 — `libweston/compositor.c`

Cancel any in-flight touch session when the compositor goes to sleep,
so a sleep/wake cycle doesn't carry stale touch state across.

```diff
 WL_EXPORT void
 weston_compositor_sleep(struct weston_compositor *compositor)
 {
+    struct weston_seat *seat;
+
     if (compositor->state == WESTON_COMPOSITOR_SLEEPING)
         return;

+    wl_list_for_each(seat, &compositor->seat_list, link) {
+        struct weston_touch *touch = weston_seat_get_touch(seat);
+        if (touch)
+            weston_touch_cancel_grab(touch);
+    }
+
     wl_event_source_timer_update(compositor->idle_source, 0);
     compositor->state = WESTON_COMPOSITOR_SLEEPING;
     weston_compositor_dpms(compositor, WESTON_DPMS_OFF);
 }
```

With the cancel handler from Patch 1, `weston_touch_cancel_grab()` now
zeros `num_tp` and clears focus. Clients receive `wl_touch.cancel`,
which matches Wayland semantics for "the session is over."

## Conclusion

The proximate bug is in weston: a sticky `focus == NULL && num_tp > 0`
state with no recovery. Patch 1 makes it self-healing on the next
`WL_TOUCH_DOWN`. Patch 2 is defensive — it prevents one of the known
ways to enter that state in the first place (custom sleep/wake protocol
not cancelling touches before DPMS off).

The underlying cause is a controller firmware issue that allows MT
slots to leak. As long as that exists, weston should be defensive about
its bookkeeping rather than trusting that `num_tp` and `focus` will
stay in sync.

Recommended companion measures in the application/OS layer:

- Replace the `xhci_hcd` PCI driver unbind with a targeted
  `USBDEVFS_RESET` on the eGalax (`0EEF:C000`). May or may not clear
  the controller's slot state — depends on firmware — but it's cheap,
  targeted, and the right tool. Run it on every wake from sleep as a
  preventive measure.
- If `USBDEVFS_RESET` proves insufficient (i.e. stuck slots persist
  across the reset, observable via `EVIOCGMTSLOTS` before/after), spec
  a GPIO-controlled load switch (e.g. AP22804) on the 5 V line into
  the panel in the next hardware revision. That is the only software-
  controllable way to force a true POR on this controller.
