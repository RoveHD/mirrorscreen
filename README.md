# DisplayMirror

Mirrors one physical display onto another on Windows, while **both displays keep
their own resolution and their own refresh rate**.

Windows stays on "Extend these displays". The source monitor is never dragged
down to the TV's resolution or refresh rate — it keeps running at its native
mode while the TV shows a scaled copy.

```
Monitor   2560x1440 @ 360 Hz   (source, keeps running at 360 Hz)
TV        3840x2160 @ 120 Hz   (target, keeps running at 120 Hz)
Windows   "Extend these displays"
```

Native C++17, Win32 + Direct3D 11 + DXGI + the Desktop Duplication API. No
Electron, no Qt, no .NET, no FFmpeg, no OBS code, no GDI capture. The only
dependency is the Windows SDK.

---

## Why this works with games

DisplayMirror captures **a whole physical output**, never a window and never a
process. It does not look for a game, does not enumerate processes, does not
read another process's memory, does not hook any API, and does not inject any
code. It only calls documented Windows and DXGI functions.

That has two consequences:

* It is renderer-agnostic. D3D9, D3D10, D3D11, D3D12, Vulkan and OpenGL all
  composite to the same desktop, so all of them are captured the same way, in
  windowed, borderless and (with the caveat below) fullscreen modes.
* There is nothing for an anti-cheat to object to. No injection, no hooking, no
  process access — the game cannot tell DisplayMirror is running, and
  DisplayMirror never touches it.

---

## How a frame gets from the monitor to the TV

Everything stays on the GPU. There is no `Map`, no readback, no `memcpy` of
frame data, and no CPU-side framebuffer anywhere in the path.

```
Desktop Duplication (source output)
    IDXGIOutput5::DuplicateOutput1        <- original scan-out format, no forced conversion
        |
        v
    AcquireNextFrame -> ID3D11Texture2D   <- the live desktop surface
        |
        | ID3D11DeviceContext::CopyResource   (GPU -> GPU)
        v
    owned ID3D11Texture2D + SRV           <- so ReleaseFrame can happen immediately
        |
        | full-screen triangle, linear sampling, aspect-fit viewport
        v
    Flip-model swap chain of the borderless output window on the TV
        |
        v
    Present
```

`ReleaseFrame` is called immediately after the copy, so the duplication surface
is never held across a `Present`. Holding it would stall the source display's
compositor — precisely the thing that must not happen while a game is running.

The cursor is composited as a second pass. Desktop Duplication reports the
pointer separately from the desktop image, so it has to be drawn by us.

### 360 Hz source, 120 Hz target

The two rates are deliberately decoupled. DisplayMirror does **not** try to
output every source frame.

* The loop is paced by the **target**: it waits on the swap chain's frame
  latency waitable object, which signals once per TV frame (120 Hz).
* Only then does it ask Desktop Duplication for a frame. The API coalesces:
  `AcquireNextFrame` returns *the current desktop image*, not a backlog. Frames
  produced by the source since the last call are collapsed into one and
  reported via `AccumulatedFrames`.
* So at 360 Hz in and 120 Hz out, roughly every third source frame is shown and
  the rest are dropped by the API before they ever reach us. Nothing is
  buffered, nothing is queued, and the frame that gets presented is always the
  newest one available.

Doing the wait **before** the acquire is what keeps latency low: by the time we
ask for a frame, the target is already ready to display it, so the frame we grab
is as fresh as possible.

### How frame queues are prevented

Four separate mechanisms, all pulling in the same direction:

| Mechanism | Effect |
|---|---|
| `DXGI_SWAP_EFFECT_FLIP_DISCARD`, `BufferCount = 2` | Minimum flip-model chain: one buffer on screen, one to draw into. |
| `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` | The loop blocks until the swap chain is ready, instead of letting `Present` build a queue behind it. |
| `IDXGISwapChain2::SetMaximumFrameLatency(1)` | At most one frame in flight. |
| `IDXGIDevice1::SetMaximumFrameLatency(1)` | Same, at the device level. |

The Desktop Duplication side needs no queue management at all, because the API
does not queue — it always hands back the current image.

### VSync vs. waitable vs. allow-tearing

The requirement is a clean picture on the TV by default, with minimum latency.
Those pull in opposite directions, so:

**Default: waitable swap chain + `Present(1, 0)`.** The waitable object removes
the *queueing* latency (the part that is pure waste) while `Present(1, 0)` keeps
the output synchronised to the TV's scanout, so there is no tearing. This is the
combination Microsoft documents for low-latency presentation, and it is the
right default for a TV: a torn picture on a 120 Hz TV looks worse than the ~8 ms
a vblank costs.

**`ALLOW_TEARING`, on by default.** It is the lower-latency path and the one
that behaves best on a cross-GPU pair, so the checkbox starts ticked; untick it
for a tear-free picture. Offered only when
`IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` reports
support — tearing is never assumed. When enabled, the swap chain is created with
`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` and presented with
`Present(0, DXGI_PRESENT_ALLOW_TEARING)`, which is the only sync interval the
flag is legal with. In this mode the loop is no longer paced by the target, so
it presents as fast as the source produces frames — lowest latency, visible
tearing, higher GPU load. The checkbox is greyed out on systems that do not
report support.

#### The waitable object is a semaphore, and that has a rule

`GetFrameLatencyWaitableObject` does not return an event that is simply "set"
when the swap chain is ready. It returns a **semaphore**, created with one
token per allowed frame in flight, and `Present` releases a token back when the
frame retires. Waiting takes a token.

The rule that falls out of it: **a wait must be followed by a `Present`.** A
wait that is not spent leaks its token, and since only `Present` gives one
back, the semaphore stays empty and the *next* wait blocks until its timeout.

That is easy to get wrong here, because the loop deliberately does not present
on every pass. It waits first and only then asks Desktop Duplication for a
frame, and the acquire can legitimately come back with nothing to show — a
`WAIT_TIMEOUT` on a source that has not changed, an access loss, a
pointer-only update while the cursor is switched off. Every one of those paths
returns without presenting.

The symptom is specific and misleading: the desktop looks perfect while
anything is moving continuously, because a moving window repaints on every
source refresh and the acquire never times out. Video is where it shows. A
video repaints at its own frame rate, not the monitor's, so acquires that come
back empty are routine — and each one costs a full timeout on the very next
pass. It reads as stutter that only affects video, and it disappears entirely
in tearing mode, where the swap chain has no waitable object at all.

The fix is to hold the token rather than spend it: `WaitForPresentReady` takes
one at most once, keeps it across calls, and `RenderAndPresent` is the only
thing that clears it. A caller may wait, decide there is nothing worth showing,
and come back later without paying twice.

`SetFullscreenState(TRUE)` is deliberately **not** used. A borderless
flip-model window is better here on every axis: DXGI never takes ownership of
the TV's display mode (which is the whole point — the TV must keep its own
resolution and refresh rate), there is no mode-switch flicker when the mirror
starts or stops, and no exclusive-fullscreen transition on the target can
disturb the game running on the source. Alt+Enter is blocked with
`DXGI_MWA_NO_ALT_ENTER` so DXGI cannot do it behind our back either.

### `DXGI_ERROR_ACCESS_LOST`

Access loss is normal, not exceptional. It happens whenever a fullscreen
application takes the display, on a mode change, and on every desktop switch —
the lock screen, a UAC prompt, fast user switching.

The handling is:

1. `AcquireNextFrame` returns the error; it is classified as *duplication lost*
   rather than *device lost*.
2. The `IDXGIOutputDuplication` is destroyed completely.
3. A retry is scheduled — 200 ms for the first 25 attempts, then 1 s — so a
   display that stays unavailable never turns into a busy loop.
4. On retry the output is looked up **by device name**, not by index, because
   indices shift when a display is switched off and back on.
5. `DuplicateOutput1` runs again. If the capture format changed (a fullscreen
   HDR game, say), the swap chain is rebuilt to match; if it did not, the
   existing swap chain is kept, so recovering from an ordinary alt-tab does not
   flash the output.
6. The transition is logged **once**, not once per attempt.

`DXGI_ERROR_WAIT_TIMEOUT` is explicitly **not** an error. It means "no new
frame", which is the normal state of an idle desktop. Nothing is presented (the
flip model keeps the last frame on screen) and the loop goes straight back to
blocking inside `AcquireNextFrame`. That call *is* the idle wait — there is no
polling and no spin anywhere in the program.

`DXGI_ERROR_DEVICE_REMOVED` / `_RESET` are treated differently: the entire
pipeline including the D3D device is rebuilt, and `GetDeviceRemovedReason` is
logged.

### Fullscreen games

* **Borderless fullscreen** — works normally. This is the recommended setup and
  what most modern games default to.
* **Fullscreen on Windows 10/11** — modern D3D11/D3D12 games in "fullscreen"
  usually get *independent flip*, which Desktop Duplication still captures.
  `DuplicateOutput1` is used specifically so the game's own scan-out format
  (10-bit, HDR) comes back unconverted, preserving both performance and colour
  gamut.
* **True legacy exclusive fullscreen** — mostly older D3D9 titles. These take
  the display away from the compositor, and Desktop Duplication returns
  `DXGI_ERROR_ACCESS_LOST` for as long as they hold it. This is a limitation of
  the OS API, not something a program can work around without hooking, which is
  out of scope by design. DisplayMirror retries cleanly in the background and
  resumes the moment the game releases the display. **Switch such games to
  borderless windowed.**

---

## Scaling

Aspect-fit only, in this version:

* The source keeps its aspect ratio; nothing is stretched or distorted.
* Different aspect ratios produce black bars.
* The image is centred.

Implementation is deliberately trivial: the render target is cleared to black
and the viewport is set to the fitted rectangle, so the black bars fall out of
the clear and the shader needs no letterboxing maths at all. Scaling itself is
a linear-filtered GPU sample — 2560x1440 to 3840x2160 costs one texture fetch
per output pixel.

Stretch and crop/fill are intentionally not implemented yet; the viewport
calculation is the only place that would need to change.

Rotated source displays (portrait mode) are handled: the duplication surface is
always un-rotated with the image rotated inside it, so the rotation is undone in
the pixel shader.

---

## HDR

HDR is detected, not ignored. On startup, and again after every display change,
DisplayMirror reads `IDXGIOutput6::GetDesc1` for the active colour space and
bit depth, plus `QueryDisplayConfig` for the advanced-colour state and the SDR
white level of each display.

The rule is **follow the source**:

| Source | Target | What happens |
|---|---|---|
| SDR | SDR | Direct pass-through. 8-bit, or 10-bit if both ends support it. |
| SDR | HDR | SDR swap chain, `G22_NONE_P709`. Windows composites SDR onto an HDR display correctly; nothing to do. |
| HDR | HDR | **True pass-through.** Duplication delivers scRGB `R16G16B16A16_FLOAT`; the swap chain is created in the same format with `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. No conversion happens at all. |
| HDR | SDR | **Limited mode.** Tone mapped in the shader and logged as an approximation. |

The HDR→SDR path is the only lossy one. It rescales scRGB by the source's real
SDR white level (so the picture is not washed out, which is what a naive clamp
produces), rolls the highlights off with an extended Reinhard curve using the
display's reported peak luminance, and re-encodes to sRGB. It is an
approximation, it is documented as one, and it writes a warning to the log every
time it is used. It never silently emits wrong colours.

If the target refuses the wide-gamut colour space, the swap chain is rebuilt as
SDR and the tone-mapping path is enabled — again with a log line, never
silently.

---

## Protected content

No attempt is made to defeat DRM, protected swap chains, the secure desktop or
the UAC secure desktop. When Windows hands back a black region for protected
content, that black region is what gets mirrored. DisplayMirror is for mirroring
your own display, and there is no code path that tries to be anything else.

---

## Multiple GPUs

The DXGI adapter that owns each output is resolved properly, and both displays
are matched by adapter LUID. Source and target on **different GPUs** is
supported — a monitor on the discrete card and a TV on a second card, or on the
integrated GPU, all work.

There is still only ever **one D3D device**, and it lives on the adapter that
owns the *source* display. That is not a choice: Desktop Duplication only
accepts a device on the adapter driving the output it duplicates. So the
capture, the scaling pass and the swap chain all stay on the source GPU, and
the borderless output window happens to sit on a display driven by the other
one.

The transfer is then the OS's job. A windowed flip-model swap chain is
composited by DWM, and when the window lives on another adapter's display DWM
performs the cross-adapter copy of each presented frame. This is the same path
a laptop takes when the discrete GPU renders onto the panel wired to the
integrated GPU. It is a GPU-to-GPU transfer: **there is still no `Map`, no
readback and no CPU-side framebuffer** anywhere in the frame path.

What it costs:

| | Same GPU | Across GPUs |
|---|---|---|
| Frame path | Capture → scale → Present | Capture → scale → Present → DWM copy to the other adapter |
| CPU readback | None | None |
| Extra latency | — | Roughly one frame |
| Bus traffic | — | One target-sized frame per presented frame, over PCIe |
| `ALLOW_TEARING` | Independent flip | No independent flip, only an unpaced loop |

Tearing mode stays available across adapters, but it does something different
there. A cross-adapter present is composited by DWM and cannot reach
independent flip, so the flag's real effect is only that the loop stops waiting
on the target: every source frame — up to 360 per second on a 360 Hz monitor —
is then copied across the bus instead of one per target refresh. That is a
bandwidth cost, not a correctness problem, so it is logged rather than refused.

The cross-adapter state is logged on start, and re-evaluated on every display
change: a display can come back on a different adapter than it left on, and the
present mode follows it in both directions.

If DXGI does refuse to create the swap chain across the two adapters on some
system, *that* is reported as a GPU mismatch with a clear message — the pairing
is never rejected up front any more.

---

## Building

Requirements:

* Visual Studio 2022 (or any current MSVC), x64
* Windows 11 SDK (10.0.17763 or newer — `DISPLAYCONFIG_SDR_WHITE_LEVEL` needs it)
* CMake 3.21+

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binary lands in `build\Release\DisplayMirror.exe`.

It links the static CRT, so there is nothing to install and no redistributable
to ship — a single `.exe`. Shaders are compiled at startup with `D3DCompile`;
`d3dcompiler_47.dll` is a Windows component on Windows 10 and 11, so that adds
no dependency either.

Opening the folder directly in Visual Studio ("Open a local folder") also works,
since VS picks up `CMakeLists.txt` natively.

---

## Using it

Start `DisplayMirror.exe`. The config window lists every active display with its
name, resolution, refresh rate, desktop position, GPU and HDR state, and marks
the primary one.

1. Pick the **source display** (the one to capture — your main monitor).
2. Pick the **target display** (the one to mirror onto — the TV).
3. Press **Start mirroring**.

The config window disappears into the notification area and a borderless,
title-bar-free window covers the whole target display.

| Control | Action |
|---|---|
| `Ctrl` + `Alt` + `M` | Start / stop mirroring, from anywhere including inside a game |
| `ESC` | Stop, when the output window has focus |
| **Stop mirroring** | Same, from the config window |
| Tray icon, double-click | Bring the config window back |
| Tray icon, right-click | Show, start/stop, or exit |

### It lives in the tray, not the taskbar

Minimising, closing and starting to mirror all **hide** the window rather than
minimise it — a minimised window still owns a taskbar button, which is the
thing being avoided. The tray icon is then the only way back to it, and its
tooltip carries the state: either `not mirroring`, or the pair being mirrored.

The X button therefore does not quit. This is a program meant to sit there
waiting for a TV to be switched on, so closing the window is not the same as
being done with it; **Exit** in the tray menu is what quits. (If the tray icon
could not be registered at all, the X button closes normally rather than
leaving no way back.)

Stopping does *not* pop the window back up. Stopping from the hotkey usually
means a game is on the source display, and throwing a window in front of it
would be the wrong answer.

Source and target cannot be the same display; the Start button stays disabled
until two different ones are selected.

The output window is created with `WS_EX_NOACTIVATE`, so **it never steals focus
from a running game**, and `WS_EX_TOOLWINDOW` keeps it out of Alt+Tab.

### Settings, autostart and unattended mirroring

Everything the config window can change — the display pair and all four
checkboxes — is written to the registry the moment it changes. There is no save
step to forget and no settings file to lose.

| Checkbox | What it does |
|---|---|
| **Mirror the mouse cursor** | Composites the pointer. Can be toggled while mirroring. |
| **Allow tearing** | **On by default.** Greyed out when DXGI reports no tearing support. Takes effect at the next start, since the flag is baked into the swap chain. |
| **Start with Windows** | Adds this executable to `HKCU\...\CurrentVersion\Run`, with `--minimized`, which starts it straight into the tray: no window and no taskbar button at logon. |
| **Start mirroring when both displays are connected** | Off until you ask for it. Once on, mirroring starts by itself whenever the saved pair is complete. |

The saved pair is stored as each monitor's **device path**, not as its name or
its `\\.\DISPLAYn` number. That matters: `\\.\DISPLAY2` is reassigned when a
display is switched off and back on, and two monitors of the same model share a
friendly name. The device path survives a reboot, survives the TV being switched
off, and tells two identical panels apart.

With both boxes ticked the intended flow needs no interaction at all: log in,
switch the TV on, and mirroring starts. Switch the TV off and DisplayMirror
waits; switch it on again and it resumes.

Auto-start deliberately loses to you. Stopping by hand — the button, `ESC`, or
`Ctrl+Alt+M` — suppresses it, so it will not immediately restart what you just
stopped. The suppression clears as soon as the pair is incomplete again, which
means switching the TV off and on is what re-arms it.

A log is written to `DisplayMirror.log` next to the executable and shown live in
the config window. It logs state transitions only — adapters and displays
detected, capture started, resolution changed, duplication reinitialised, target
lost, device lost, error codes. **Nothing is logged per frame.**

---

## What it survives

All of these are handled automatically, without stopping the session:

| Situation | Handling |
|---|---|
| Alt+Tab | Access lost, silent reinit |
| Game starts / exits | Access lost, silent reinit |
| Windowed ↔ borderless ↔ fullscreen | Access lost, reinit; swap chain rebuilt only if the capture format changed |
| Source resolution or refresh rate changes | `WM_DISPLAYCHANGE`, full rebuild |
| Target resolution changes | Window repositioned, `ResizeBuffers` |
| Target moved in the desktop layout | Window repositioned |
| HDR toggled on either display | Colour plan recomputed, swap chain rebuilt |
| TV switched off / on | Target lost, retried until it returns |
| HDMI briefly unplugged | Same |
| Windows locked / unlocked | Access lost on the secure desktop, reinit on return |
| Sleep / wake | `WM_POWERBROADCAST`, topology re-checked |
| GPU device reset / removed | Device rebuilt, removal reason logged |
| Output window occluded | DXGI throttles presentation; logged once |

Retries are throttled and logged once per transition, so a game that holds the
display for an hour produces one log line, not thousands.

---

## Performance

At the target case — 2560x1440 @ 360 Hz source, 3840x2160 @ 120 Hz target —
the per-frame GPU work is one `CopyResource` plus one full-screen triangle with
a linear sample per output pixel, ~120 times a second. There is no encoding, no
colour conversion in the SDR path, and no CPU involvement in the frame data at
all.

CPU use is near zero when idle: with a static desktop the thread blocks inside
`AcquireNextFrame` and nothing is presented. There is no polling loop and no
`Sleep`-based pacing in the steady state.

The one CPU-side operation on image data is decoding the cursor bitmap, which
happens only when the cursor *shape* changes (not when it moves), on a 32x32-ish
image.

---

## Architecture

Six files, no class hierarchy, RAII throughout, COM via
`Microsoft::WRL::ComPtr`. No hand-written smart pointers.

| File | Responsibility |
|---|---|
| `src/main.cpp` | Entry point, config window, hotkeys, message loop |
| `src/displays.h/.cpp` | Display enumeration: DXGI topology + `QueryDisplayConfig` for friendly names, exact rational refresh rates, HDR state, SDR white level |
| `src/capture.h/.cpp` | Desktop Duplication: `DuplicateOutput1` with `DuplicateOutput` fallback, acquire/release, pointer metadata and shape decoding |
| `src/renderer.h/.cpp` | Output window, flip-model swap chain, colour plan, scaling and cursor passes, present |
| `src/mirror.h/.cpp` | The session: owns device + capture + renderer, and the recovery state machine |
| `src/log.h/.cpp` | Logging to file, debugger and the UI pane |

A single thread runs the message pump and the capture/present loop. A capture
thread was considered and rejected: it would need cross-thread D3D
synchronisation for no latency benefit, because the loop already waits on the
target's vblank *before* acquiring, which is the point at which the freshest
frame is available anyway. Fewer moving parts, and stability ranks above latency
in the requirements.

---

## Limitations

* **True legacy exclusive fullscreen** (mostly old D3D9 games) cannot be
  captured while it holds the display. Use borderless windowed. See above.
* **HDR source on an SDR target** is tone mapped, not exact. Enable HDR on the
  target for true pass-through.
* **Across two GPUs**, DWM copies each presented frame to the target's adapter:
  about one frame of extra latency, and `ALLOW_TEARING` cannot reach
  independent flip there.
* **Aspect-fit only.** Stretch and crop/fill are not implemented yet.
* **Protected content** appears black. By design.
* **XOR cursors** (`MONOCHROME` with both mask bits set, and `MASKED_COLOR` with
  alpha 0xFF) are drawn opaque rather than inverted against the background. True
  XOR would need a destination read in the blend. Invisible for normal cursors.
* On a **rotated** source display the cursor bitmap itself is not re-oriented,
  though its position is correct.

---

## Verification status

What was verified:

* The code **cross-compiles clean** for x64 Windows with `-Wall -Wextra` (zero
  warnings) and links to a valid PE32+ binary.
* All four **HLSL entry points compile** cleanly through a real shader compiler.
  This matters because the shaders are built at startup, so a syntax error would
  be a launch failure rather than a build failure.
* The pipeline was reviewed against the current Microsoft documentation for
  Desktop Duplication, `DuplicateOutput1`, the flip model, frame latency
  waitable objects, tearing support and DXGI colour spaces.

What has been exercised on real hardware so far:

* The program **launches**, the config window comes up and the log pane fills.
* **Display enumeration is correct** on a four-display, two-GPU machine: names,
  resolutions, refresh rates, desktop positions, bit depth, HDR state, the
  primary flag, and which adapter owns which output were all read back
  accurately (`AW2725DF 2560x1440 @ 359.979 Hz, 10 bpc` on an RTX 4070;
  `FireTV 1920x1080 @ 60 Hz` on an RTX 3050).

**The frame path itself has not been run.** It was developed in a Linux
container with no Windows machine, no GPU and no displays, so capture, scaling,
presentation and every recovery path are still unexercised. The checklist below
is what needs to be verified on the target machine.

### Frame statistics

While mirroring, one line is written every ten seconds:

```
[19:52:14.031] INFO  Frames: 59.8 presented/s, 60.1 acquired/s, 0.3 acquire timeouts/s, 0 with nothing to show
```

Presented and acquired should track each other. A presented rate well below the
target's refresh while something is moving on the source is the signal that the
loop is stalling somewhere; a high timeout count on its own is normal, and just
means the source is producing fewer frames than the target could show. Nothing
is logged while the desktop is idle.

### Test checklist

Desktop:
- [ ] Move the mouse — cursor appears on the TV, tracks smoothly, correct shape
- [ ] Drag a window across the source display
- [ ] Play a video
- [ ] Scroll a long page quickly — no stutter or tearing in the default mode

Resolutions:
- [ ] 1080p source to 4K target
- [ ] 1440p source to 4K target
- [ ] Identical resolutions (no scaling)
- [ ] A source and target with different aspect ratios — black bars, centred, undistorted

Refresh rates — and confirm in Windows display settings that **both displays
still report their own rate while mirroring**:
- [ ] 60 Hz to 60 Hz
- [ ] 144 Hz to 60 Hz
- [ ] 360 Hz to 120 Hz — the target case

Two GPUs (source and target on different adapters):
- [ ] Mirroring starts at all, and the log shows the cross-GPU warning
- [ ] The picture is correct and smooth on the target
- [ ] The source display keeps its own refresh rate
- [ ] GPU load on the target adapter stays modest (that is the DWM copy)

Settings and autostart:
- [ ] Selection and checkboxes survive a restart of the program
- [ ] The saved pair is still matched after a reboot
- [ ] The saved pair is still matched with two identical monitors attached
- [ ] "Start with Windows" launches it minimised at logon
- [ ] Auto-start fires when the TV is switched on
- [ ] Auto-start does *not* restart a session stopped by hand
- [ ] Switching the TV off and on re-arms auto-start after a manual stop

Tray:
- [ ] The icon appears and is identifiable
- [ ] Its tooltip names the mirrored pair while mirroring, and says so when not
- [ ] Minimise, close and Start mirroring all leave no taskbar button
- [ ] Double-clicking the icon brings the window back
- [ ] The right-click menu starts, stops and exits
- [ ] `--minimized` shows no window at all, only the icon
- [ ] The icon comes back after Explorer is restarted

Games:
- [ ] A D3D11 title, borderless
- [ ] A D3D12 title, borderless
- [ ] A Vulkan title, borderless
- [ ] An older D3D9 title (expect access-lost retry in true exclusive fullscreen)
- [ ] A title with anti-cheat — confirm it launches and runs normally
- [ ] Confirm the source monitor still runs at its full rate while a game is running

State changes:
- [ ] Alt+Tab in and out of a game repeatedly
- [ ] Switch the TV off and on
- [ ] Unplug and replug HDMI
- [ ] Change the source resolution while mirroring
- [ ] Change the target resolution while mirroring
- [ ] Lock and unlock Windows
- [ ] Sleep and wake
- [ ] Toggle HDR on each display

Each of these should either keep working or recover on its own, and should
produce exactly one log line per transition.
