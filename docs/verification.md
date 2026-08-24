# Verification

Measurements behind the claims in the README. Everything here was taken from a running KWin —
either a nested `kwin_wayland --virtual` session with two outputs, or real hardware where noted.

## Paint clipping

Nested two-output session: Virtual-0 at 0,0 and Virtual-1 at 1024,0, seam at x=1024. One window
at 624..1224, so 200px overhang past the seam. Magenta pixels counted on each output:

| | Virtual-0 | Virtual-1 |
|---|---|---|
| effect off | 7795 | 3397 |
| effect on | 7792 | 0 |

The visible part is untouched and the spill is gone. KWin 6.7's GL scene does honour a reduced
device region in `paintWindow` for untransformed windows.

Moving the window's centre across the seam flips `EffectWindow::screen()`, and the effect then
clips to the other output (Virtual-0=0, Virtual-1=7347) — a window is always fully drawn on
exactly one screen and never spills onto the other.

## Input clipping

Same session. Window A at 700..1100 owned by Virtual-0, so 1024..1100 is invisible overhang,
sitting above window B at 1024..1424 owned by Virtual-1. Asking `workspace.windowAt()` — which is
`Window::hitTest()`, the same call the pointer uses — who is under three points:

| point | input clipping off | input clipping on |
|---|---|---|
| 900,200 — A's own screen | A | A |
| 1050,200 — A's overhang, over B | **A**, B | B |
| 1200,200 — B alone | B | B |

The middle row is the bug: an invisible overhang on top takes the click meant for B.

### What the vtable patch touched

On KWin 6.3.6 the slot index read out of `&Window::hitTest` came to 40, and the function it
displaced was at `0x4560b0` — matching `nm -D libkwin.so.6` for `KWin::Window::hitTest` exactly.

After `unloadEffect` the overhang answers hit tests again and KWin keeps running, confirming the
destructor's restore.

One trap worth recording: the vtable page lives in `.data.rel.ro`, which full RELRO has already
made read-only. Its current protection is read from `/proc/self/maps` and restored afterwards. That
file must be read with stdio, not `QFile` — procfs reports it as zero length, so `QFile::atEnd()`
says "empty" before a single line has been read, which silently disabled the hook on the first
attempt.

## Occlusion culling

Seen on real hardware: System Settings at x=1178..2488 with the seam at 1920, owned by HDMI-A-2,
so 1178..1920 on eDP-1 was drawn by nobody. The Konsole beside it ended at x=1615, leaving
1615..1920 showing a frozen copy of that Konsole from *before* it was resized — the repaint that
should have painted wallpaper there had been culled.

Both scene implementations show the same mechanism and the same cure:

| | KWin 6.3.6 | KWin 6.7.4 |
|---|---|---|
| damage cull | `damage += paintData.region - opaque` | `deviceDamage \|= paintData.deviceRegion - opaque` |
| paint cull | `visible -= data->opaque` | same shape, device coordinates |
| opaque contributed only if | `!(mask & PAINT_WINDOW_TRANSLUCENT \| ..._TRANSFORMED)` | same, plus an `opacity() == 1.0` gate before the region is built |

`WindowPrePaintData::setTranslucent()` clears exactly that mask bit in both, withdrawing the
window from the cull.

This class of bug cannot be reproduced in a nested session — the virtual backend repaints in full
every frame, so a wrongly culled region still comes out correct. The nested session can only check
the decision: that `setTranslucent()` is applied to the straddling window and to no other.

### Why "overhangs another output" and not "extends past its own output"

A frame poking off the outer edge of the desktop covers nothing on any screen, so no cull ever
suppresses a repaint there. The distinction is not academic: a column was seen at x=2531..3841
against a desktop edge at 3840, and a plain containment test would have put it on the slower
blended path for its entire life over one pixel.

## Screen attribution

Plasma repeats KWin's centre-based guess in its own process:

```cpp
// libtaskmanager/waylandtasksmodel.cpp:938
} else if (role == ScreenGeometry) {
    return screenGeometry(window->geometry.center());
}

// libtaskmanager/taskfilterproxymodel.cpp:355 — exact rect equality against the panel's screen
if (screenGeometry.isValid() && screenGeometry != d->screenGeometry) {
    return false;
}
```

So a straddling window's task button lands on the wrong panel: drawn and clicked on one screen,
its icon on the other.

`Window::windowManagementInterface()` and `PlasmaWindowInterface::setGeometry()` are both public in
6.3.6 and 6.7.4. KWin fills the geometry in from a lambda connected to `frameGeometryChanged` at
window creation, so an effect connecting later is called second and gets the last word.

The correction keeps the top-left corner and changes the width, because the published x is what a
position-ordered task manager sorts on:

| | real | published | Plasma reports |
|---|---|---|---|
| straddler, owner Virtual-1 | x=700 w=600, centre 999 | x=700 w=649, centre 1024 | Virtual-1 |
| control, owner Virtual-0 | x=100 w=400, centre 299 | unchanged | Virtual-0 |

Verified end to end by reading Plasma's own `TasksModel` through a probe plasmoid under
`plasmawindowed` — the only client KWin authorises to read it:

```
attribution off : geom x=700 w=600 | SCREEN x=0      <- wrong panel
attribution on  : geom x=700 w=649 | SCREEN x=1024   <- the owner
```

Also verified: `setOwner` moves the icon immediately in both directions, and unloading the effect
hands the honest rectangle back.

### Parked columns

Width cannot fix a window lying wholly beyond its owner's far edge — growing it only pushes the
centre further away — so the corner has to move, and every such column lands on the same pixel. A
position-ordered task manager then sees them tie and falls back to model order, which scrambles
the panel. At 100% column width that is every column except the visible one, i.e. the normal case.

Parked corners are therefore pinned one pixel apart, in the order the columns really sit in:

| real x | published x |
|---|---|
| 2100 | 2045 |
| 4000 | 2046 |
| 5900 | 2047 |

Columns parked off the *left* edge keep their real x, so they are already distinct and already in
order. The three zones stay monotonic end to end; only a genuinely odd layout could interleave
them — enough narrow columns visible right against the screen edge that a visible x falls inside
the parked band.

### Batching

A tiler's scroll moves every window in a grid one at a time, each through its own synchronous
`setGeometry()`. Publishing on every `frameGeometryChanged` meant a 4-window scroll made 4 trips
to Plasma, each showing a different incomplete mix, and a position-ordered task manager reordered
on every one — visible as a flicker of wrong orderings before the correct one.

Counted directly, moving 4 windows one at a time in a loop:

| | calls to Plasma | what each showed |
|---|---|---|
| before | 4 | a different incomplete mix |
| after | 1 | only the settled state |

`QTimer::singleShot(0, ...)` is enough: a synchronous script loop cannot reach the next turn of
the event loop until it returns.

## Other consumers of the published geometry

Checked rather than assumed. Besides the task manager, the only shipped plasmoid that reads it is
the Pager (`main.qml:468`), which would draw a straddling window narrower or wider than it is;
that has not been reviewed. Highlight-on-hover goes through KWin's D-Bus by window id and the
minimise animation travels the other way, so neither is affected.

## Build and load behaviour

- 6.3.6 does **not** cache its effect plugin list at startup: a `.so` for an effect not previously
  loaded is picked up by `loadEffect` immediately. 6.7.4 enumerates once at startup and silently
  refuses until the next login.
- Replacing an effect that is *already loaded* needs a re-login on both. `unloadEffect` destroys
  the object but leaves the library mapped, so the following `loadEffect` re-runs the old code.
  Measured on 6.3.6 by calling a method that only exists in the new build — still `UnknownMethod`
  after a full unload/load cycle. This is why `buildStamp()` exists: every other way of telling
  two builds apart relied on a feature that happened to be new at the time.
- `KWIN_EFFECT_FACTORY` sets `enabledByDefault() -> true`, which would auto-enable the effect on
  the next login regardless of `"EnabledByDefault": false` in `metadata.json`. This build uses
  `KWIN_EFFECT_FACTORY_ENABLED(..., return false;)` instead.
- 6.3's `KWinConfig.cmake` does not pull ECM in, so ECM's find-modules — where `Findepoxy.cmake`
  lives — are not on the module path and `find_package(KWin)` fails with a confusing "could not
  find epoxy". `CMakeLists.txt` requires ECM explicitly, which is correct on both.
