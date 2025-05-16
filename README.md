A gamepad/trackpad for linux phones/tablets implemented using wayland’s layer shell protocol. As no-one has yet to.

While running it consumes ~2-5% cpu. While disabled it consumes 0% cpu, so you can always leave it running.

Controls:
- **Activate edit:** mode by taping top left screen.
- **Enable/disable:** hold volume down for 250ms
- **Switch orientation:** volume up for 250ms
- **Control volume:** press volume up/down FASTER than 250ms

Only **PHOSH** and **PLASMA MOBILE** are tested and supported. i.e, see:
https://wayland.app/protocols/wlr-layer-shell-unstable-v1#compositor-support

ironic… considering most layer shell projects use GTK :/

### **WARNING** **WARNING** **WARNING**!!!
Due to "wayland?" limitations, the ***UI WILL NOT BE RENDERED ON THE LOCK SCREEN.***   so if you lock while in edit mode, you may be confused why no touches are registered, and nothing is visible.

Manually disable using volume down, unlock screen, the re-enable with volume down. There is no solution yet.

## Building

Debian/Mobian:
```
apt install build-essential libwayland-dev libwayland-protocols-dev libegl-dev libgles-dev libgl-dev
```

Alpine
```
apk add build-base pkgconf wayland-dev wayland-protocols mesa-dev
```
Build:
```
make
```

## Running
Root

(the -E is important, otherwise it will not connect to wayland)

```
sudo -E ./wlr-gamepad
```

"Rootless"
```
sudo chmod 666 /dev/input/* && sudo chmod 777 /dev/uinput
```
```
./wlr-gamepad
```

## Settings
Currently hardcoded:
```c
static const float kTrackpadSensitivity = 1.0f;
```
```c
static float gMasterOpacity = 0.5f;
```


## Architecture
- OpenGL immediate mode rendering
- Minimal dependencies.
- Old school C UI look


## Limitations

Due to the fact that layer shell is not well supported yet, many basic elements are implemented from scratch, code is quite difficult to follow, and I won’t even try to pretend to understand what “handle_evdev_event” does. if you know some minimal dependency abstractions which could help the complexity, I would love to know.

Things considered:
- Raylib: Good library, a replacement for drawbacks of immediate mode OpenGL while using immediate mode api, but because of simple shapes, there was actually an increase in cpu utilization by default even with little elements.
- Imgui: Impossible, does not support multi touch natively
- Nukklear: overly complicated for what I was trying to accomplish, would need to implement ui gamepad elements except buttons myself anyway, so better to implement the entire ui elements myself.
- gtk-layer-shell: Good project, but not a good fit for such simple ui elements
- SDL3: Can't draw to layer shell egl. Wayland support is not included in the base ubuntu build from what I understand and would require additional building steps.
- GLFW: Can't draw to layer shell egl at all

I’ve even considered rust, but adding just a single egui dependency resulted in a 1.6gb project….

## Todo:
- Test larger screen sizes like tablets, but I’ve tried, and it should work.
- Implement full mouse buttons and scrolling support just like TouchpadEmulator, but with improvements
- Fix font, but I find it charming. xD
- Saving/loading
- Preference menu for opacity, (currently hardcoded) etc.
- Squash bugs
