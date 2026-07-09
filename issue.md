
touch event received with 3 points down but no surface focused
Open created Jun 11, 2022 by Sören Meier

Well this is another weird one but on other hardware than #625 (closed).
Setup

Weston version 10.0.0 with a protocol addition (to kiosk-shell) which triggers weston_compositor_sleep or weston_compositor_wake via a motion sensor. For reference (soerenmeier/weston@6ddfbd67) don't think it matters particularly. The application that is run is chromium.
Device

Touchscreen: eGalax Inc. eGalaxTouch P80H100 2700 vW50-T6 k07_117 weston_startup.txt
Problem

The problem occurred only twice in the last 2 weeks and in the previous 3 months i never observed it, weston was never updated in that time but the os (buildroot) was but just bugfixes (systemd, linux...).

At some point the touch just stops working and weston outputs the following touch event received with 3 points down but no surface focused. Launching weston-flower or restarting chromium does not fix the problem.

Is this a hardware bug or driver bug? Since there is already this message that is printed, maybe weston could recover from it?

https://gitlab.freedesktop.org/wayland/weston/-/blob/main/libweston/input.c#L2405-2415
Edit

Running weston-debug timeline or proto shows that its always updating the screen weston_debug_proto.txt.

Will keep the device in this state if i should run some more tests or get some more data.
