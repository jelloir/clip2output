# clip2output

A KWin effect that confines a window to one output — in paint, in input, and in what Plasma is
told about it — based on an owner announced over D-Bus rather than on the window's position.

KWin decides which screen a window is on from the centre of its frame. That is the right answer
for windows the user drags around, and the wrong answer for a scrolling tiler, which deliberately
parks columns partly or wholly outside the screen they belong to. Once a parked column's centre
crosses the seam between two monitors, KWin re-homes it and the column reappears on the
neighbouring screen.

clip2output replaces that guess with an explicit claim. A window manager (or any KWin script)
tells the effect which output owns a window; the effect then draws that window only within the
owner's bounds, hit-tests it only there, and reports it to Plasma as living there. Windows nobody
claims are left completely alone.

It was written for [Karousel](https://github.com/peterfajdiga/karousel) running one grid per
screen, but the interface is generic: nothing in the effect knows about Karousel.

## What it actually does

Three separate things, each independently switchable at runtime:

**Paint clipping.** `paintWindow` intersects the paint region with the owning output's geometry,
so the overhang past the seam is simply not drawn. A window being interactively moved or resized
is exempt — you have to be able to see what you are dragging.

Clipping the paint leaves a hole that KWin's occlusion culling does not know about: the scene
treats the window as opaque over the whole frame, culls the repaint of everything under the
overhang, and then nothing draws there — leaving a band of stale content along the seam. The
effect therefore marks a straddling window translucent for the frame, which withdraws it from the
cull. Only windows that overhang *another output* pay that cost; one hanging off the outer edge
of the desktop occludes nothing and is left alone.

**Input clipping.** An undrawn overhang is still hit-testable, so it swallows clicks meant for the
windows that really live on the neighbouring screen. KWin funnels every "which window is under
this point" decision — pointer, touch, tablet, drag-and-drop, `workspace.windowAt()` — through the
single virtual `Window::hitTest()`. The effect redirects that one slot so input honours the same
boundary as the paint.

An effect cannot subclass `Window`, so this is done by patching the vtable. That is blunt, and
it is worth knowing exactly how contained it is:

- the slot index is read out of `&Window::hitTest` through the Itanium ABI's `{ptr, adjustment}`
  representation, so it follows KWin's own layout instead of being hard-coded;
- one vtable per `Window` subclass, patched lazily as new kinds of window appear;
- the original is kept and called first, so an unclaimed window behaves exactly as before;
- the destructor restores every slot before the library is unmapped;
- the vtable page's protection is read from `/proc/self/maps` and restored, not assumed.

**Screen attribution.** Plasma makes the same centre-based guess a second time, in its own
process: `libtaskmanager` attributes a window to the screen containing its centre, so a task
manager set to "current screen only" puts a straddling window's button on the wrong panel. The
effect publishes a corrected rectangle over `org_kde_plasma_window_management`, keeping the
top-left corner wherever possible and adjusting the width so the centre lands on the owner. The
corner matters because a position-ordered task manager sorts on it.

Republishing is deferred to the next turn of the event loop, so a scroll that moves a dozen
windows one `setGeometry()` at a time results in one publish of the settled state rather than a
dozen publishes of incomplete ones.

## Ownership rules

- A window is claimed by a `setOwner` call and stays claimed until another call changes it or
  clears it. Ownership does not follow the window's position.
- An interactive drag between monitors reassigns ownership on drop, so dragging still works
  normally.
- Passing an empty output name releases the window: it stops being clipped, and its attribution is
  handed back to whichever output actually holds it.
- **A window that has never been claimed is never touched.** An earlier version guessed an owner
  for every window at first paint, and that guess did not follow programmatic moves — KWin placing
  a dialog, an app repositioning itself, the "Window to Next Screen" shortcut — which left windows
  drawn and clickable only on the screen they had left. A modal dialog in that state locks its
  whole application. Unclaimed windows keep stock KWin behaviour, bugs included, rather than
  gaining new ones.

Because ownership is load-bearing for input as well as pixels, the claiming side has to announce
every change: on taking a window over, on moving it between grids, and on evacuating a screen that
has been unplugged.

## Requirements

- KWin 6.3 or later on Wayland. Built and tested on 6.3.6 and 6.7.4; one source handles both API
  generations.
- The KWin development headers, at **exactly** the version of the KWin you are running. The effect
  links against `libkwin` and uses private headers; a mismatch will not load.

On Debian/Ubuntu:

```
sudo apt install kwin-dev g++ cmake extra-cmake-modules pkg-config \
                 libdrm-dev libwayland-dev libepoxy-dev \
                 qt6-declarative-dev libkf6windowsystem-dev
```

Other distributions ship the same headers as `kwin-devel` (Fedora, openSUSE) or `kwin` itself
(Arch).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

That produces `build/clip2output.so`.

## Install

```
./install.sh
```

The plugin directory is `.../qt6/plugins/kwin/effects/plugins/` — note the second `plugins`. A
`.so` placed in `kwin/effects/` is found by `KPluginMetaData` but never loaded, silently.

`install.sh` writes a temporary file and renames it into place rather than copying over the
installed library. Overwriting a mapped `.so` in place corrupts the mapping in every process that
has it open, which is a reliable way to crash a running KWin.

## Enable

The effect never enables itself. Load it explicitly:

```
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect   clip2output
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect clip2output
```

Two things about reloading, both of which will otherwise waste an afternoon:

- **KWin 6.7 enumerates effect plugins once at startup.** A newly installed `.so` is invisible to
  the running compositor and `loadEffect` returns `false` with nothing in the journal. 6.3 does
  discover new plugins immediately.
- **On every version, replacing an effect that is already loaded needs a re-login.**
  `unloadEffect` destroys the object but leaves the library mapped, so the next `loadEffect`
  re-runs the old code while cheerfully reporting success.

For iterating on the effect, run a nested `kwin_wayland --virtual` session, which `dlopen`s fresh
at its own startup. `buildStamp()` (below) says which binary is really live.

## D-Bus interface

Service `org.kde.KWin`, path `/ClipToOutput`, interface `org.kde.kwin.KWin.ClipToOutputEffect`.

| Method | Purpose |
|---|---|
| `setOwner(windowId, outputName)` | Claim a window for an output. Empty `outputName` releases it. |
| `ownerOf(windowId)` | The recorded owner, or `(unassigned)`. |
| `setInputClipping(bool)` | Turn input clipping off; paint clipping stays on. |
| `inputClippingStatus()` | On/off, the hooked vtable slot, and how many window classes are hooked. |
| `setScreenAttribution(bool)` | Stop correcting what Plasma is told. |
| `screenAttributionStatus()` | Real vs published geometry for every claimed window. |
| `buildStamp()` | Compile timestamp of the binary KWin actually has mapped. |

`windowId` is KWin's `internalId` **as a string**. It is a `QUuid`, which `QDBusMarshaller` cannot
serialise — passing it directly makes the call fail silently.

From a KWin script:

```js
callDBus("org.kde.KWin", "/ClipToOutput", "org.kde.kwin.KWin.ClipToOutputEffect",
         "setOwner", String(window.internalId), "DP-1");
```

## Diagnosing

When a window seems unclickable, `setInputClipping false` is the first thing to reach for: if it
becomes clickable again, the effect's idea of who owns it is wrong. `setScreenAttribution false`
does the same job for a panel showing a window on the wrong screen.

`tools/repro-unclaimed-move.sh` moves an unclaimed window to the next screen and asks
`workspace.windowAt()` — the same `hitTest` path a real click takes — whether anything is there.
It should report a hit.

## KWin version support

One source builds against both API generations, switched on `__has_include("core/region.h")`:

| | 6.3 / 6.5 | 6.7 and later |
|---|---|---|
| output type | `Output` | `LogicalOutput` |
| region type | `QRegion` | `KWin::Region` |
| paint region coordinates | logical | device |
| `prePaintWindow` signature | takes `presentTime` | takes a `RenderView *` |

The feature test is deliberate rather than a version comparison: 6.3.6's `config-kwin.h` does not
define `PROJECT_VERSION_MAJOR`/`MINOR` at all, so a version check silently evaluates to zero and
picks a branch by accident.

## Limits

- **Wayland only.** X11 has not been tested.
- The published geometry is read by Plasma's Pager plasmoid as well as its task manager; a
  straddling window may be drawn the wrong width there. Not reviewed.
- A nested `kwin_wayland --virtual` session repaints in full every frame, so occlusion-cull bugs
  cannot be reproduced there. What it *can* verify is the decision — that a straddling window is
  marked translucent and nothing else is. Confirm pixels on real hardware.
- The vtable hook is x86-64 Itanium-ABI specific in its assumptions about member pointer layout.
  It fails safe: if the slot cannot be read or made writable, input clipping stays off and the
  rest of the effect works.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
