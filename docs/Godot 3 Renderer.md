---
tags: [architecture, renderer, godot3]
---

# Godot 3 Renderer

A gate declaring `godot_version = "3.6"` gets a renderer binary built from
upstream **Godot 3.6.3** instead of the [[Custom Godot Fork]]. It speaks the
same IPC protocol as the 4.x renderers, so the launcher barely changes — see
[[Two-Process Model]] for the protocol itself.

Linux/X11, macOS and Windows. See § Gaps for what a Godot 3 gate still cannot do.

## Why a second engine at all

`godot_version` already selects the renderer binary
(`RendererExecutable.get_download_url`), so "support Godot 3" means "publish a
renderer binary a Godot 3 `.pck` can run in." None of that belongs in launcher
GDScript.

## Where the code lives

```
godot3/                       ── upstream Godot 3.6.3, submodule, no patches
godot3-modules/
├── build.py                  ── scons entry point (mirrors godot/tools/build.py)
├── tests/
│   ├── test_keycode_map.py   ── checks the keycode table against both engines
│   └── testgate/             ── local gate + backend stub for end-to-end runs
└── the_gates/                ── the module, built via scons custom_modules=
    ├── config.py, SCsub
    ├── register_types.cpp    ── engages the renderer
    ├── ipc/
    │   ├── zmq_runtime        ── context, ipc:// resolver, argv reader
    │   ├── command_sync       ── renderer -> launcher, zmq PAIR; writes Command text itself
    │   ├── input_sync         ── launcher -> renderer, zmq PAIR
    │   └── input_event_compat ── Godot 4 InputEvent text -> Godot 3 InputEvent
    └── renderer/
        ├── gl_external_texture ── GL_EXT_memory_object_fd import + blit
        └── renderer_lifecycle  ── handshake, per-frame loop
```

Every class the module registers becomes a global name in the gate's GDScript,
and Godot 3 refuses to parse a script whose enum or class shadows one — a gate
with `enum Command` failed to load its autoloads while `Command` was registered.
Keep ClassDB registrations `TG`-prefixed; the wire's `Object(Command,…)` text is
written by hand for that reason.

`godot3/` stays pristine. Everything the fork does with `#ifdef TG_RENDERER`
blocks in `main.cpp` and the display servers has an out-of-tree equivalent:

| Fork hook (Godot 4) | Godot 3 equivalent |
|---|---|
| `tg_renderer_engage` in `Main::setup2` | `register_the_gates_types()`, which `main.cpp` calls at line 1630 — after the window, GL context, VisualServer and scene types exist |
| `tg_renderer_loop_iterate` in `Main::iteration` | `VisualServer::frame_post_draw`, emitted on the thread owning the GL context |
| `--tg-ipc-dir` global parsed in `main.cpp` | `tg_cmdline_value()`, reading `/proc/self/cmdline` |
| display-server window hiding | upstream's own `--no-window`, which the launcher passes for 3.x gates |
| `tg_renderer_boot` `[RENDERER-READY]` marker | same marker, at the end of `tg_renderer_engage` |

libzmq, cppzmq and flingfd are **not** vendored a second time — `SCsub` compiles
them straight out of `godot/thirdparty/`, so the two engines cannot drift on the
wire format.

## Frame transport: Vulkan export, GL import

Godot 3.6 has no `RenderingDevice` and no Vulkan backend, so
[[External Texture Sharing]]'s `external_texture_import` has no counterpart. The
allocation is still the launcher's — it is imported into GL instead:

```
LAUNCHER (Godot 4, Vulkan)                    RENDERER (Godot 3, GLES3)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
vmaCreateImage, dedicated,
  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD
        │
        └── fd ────── flingfd (SCM_RIGHTS) ──────►  lseek(fd, 0, SEEK_END)
                                                      = allocation size
                                                    glCreateMemoryObjectsEXT
                                                    glMemoryObjectParameterivEXT
                                                      GL_DEDICATED_MEMORY_OBJECT_EXT
                                                    glImportMemoryFdEXT(size, fd)
                                                    glTexStorageMem2DEXT(GL_RGBA8)
                                                            │
                                                    both processes now back the
                                                    same GPU allocation
                                                            │
   ext_texure.copy_to(texture_rid)      ◄───────────  glBlitFramebuffer from the
   (unchanged, every _process)                        root viewport, every
                                                      frame_post_draw
```

Three things this needed that the 4.x path did not:

- **The allocation size.** `glImportMemoryFdEXT` needs the exact
  `VkMemoryAllocateInfo::allocationSize`, and it is not `width * height * 4`: a
  1440x2416 RGBA8 image measures 14008320 bytes against 13916160 bytes of
  pixels, the rest being tiling alignment. Nothing in the protocol carries it —
  but nothing has to. Mesa hands out Vulkan's external memory as a dma-buf, and
  dma-buf implements `llseek` precisely so importers can size it, so
  `recv_filehandle` reads it off the fd it was just passed. When the fd reports
  no length, the size is measured instead (§ Measuring the allocation). Either
  way the change stays self-contained: no new IPC command, and no change to the
  Godot 4 fork.
- **A source texture, not the screen.** Godot 3 emits `frame_post_draw` *after*
  `end_frame()` has already swapped buffers, so the default framebuffer's
  contents are undefined by then. The blit reads the root viewport's render
  target (`VisualServer::viewport_get_texture` → `texture_get_texid`) instead,
  which retains its contents.
- **A Y flip.** GL's framebuffer origin is bottom-left and the launcher samples
  the shared image with Vulkan's top-left origin, so `copy_from_texture`
  inverts the source rectangle.

The import only works when both processes are on the same GPU and driver stack,
which is what `GL_EXT_memory_object_fd` is for. A missing extension, or a failed
import, kills the renderer rather than leaving the gate blank.

### GPU driver support

`GL_EXT_memory_object_fd` is a POSIX-handle extension, used on Linux only; macOS
takes the IOSurface path below. The question is therefore which Linux GPU stacks
expose it, and the answer is most of them:

| Stack | Since | Notes |
|-------|-------|-------|
| Mesa | 17.3, December 2017 | Shipped on i965 (Intel) and radeonsi (AMD); current Mesa exposes it on the drivers a desktop picks up, including llvmpipe (software) and zink. This is the default renderer stack on most distributions. |
| NVIDIA proprietary | R515, June 2022 | The whole `EXT_external_objects` family (memory and semaphore, fd variants) arrived together, in the same generation the proprietary stack gained its dma-buf Vulkan export, which the launcher side needs too. |

Any desktop from roughly 2023 onward covers both stacks: Ubuntu 22.04 shipped
Mesa 23 and NVIDIA 515. The renderer probes the five entry points at startup
and refuses to engage with a named error when any is missing, so an
unsupported driver is a printed reason, not a blank gate. If a driver without
the extension ever matters, a CPU-side fallback (blit to PBO, read, upload)
would keep the wire format unchanged at the cost of a round trip per frame;
nothing here depends on that today.

### Measuring the allocation

A Win32 `HANDLE` reports no size, and neither does an fd that is not a
dma-buf. `vulkan_memory_probe` then asks Vulkan directly: it loads the system
loader (`vulkan-1.dll` / `libvulkan.so.1`, which the launcher already requires),
picks the physical device whose `deviceUUID` equals the GL context's
`GL_DEVICE_UUID_EXT`, creates the image `RenderResult.create_external_texture`
creates — 2D `R8G8B8A8_UNORM`, one mip, `TRANSFER_SRC | TRANSFER_DST`, optimal
tiling, the platform's external handle type — and reads
`VkMemoryRequirements::size`. The launcher allocates that image with
`VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, so the requirement *is* the
allocation. On Mesa/Intel the probe matches `llseek` byte for byte.

If no Vulkan device has the GL context's UUID, the launcher and the renderer
are on different GPUs (a hybrid laptop is the usual way), and no import could
work; the renderer says so instead of importing at a wrong size.

### Windows: `GL_EXT_memory_object_win32`

The handshake is the 4.x renderer's. The renderer asks for the handle with
`send_filehandle("<ipc://user://external_texture>|<pid>")`; the launcher
`DuplicateHandle`s its exported `HANDLE` into that pid and sends the value as
an `int64` over a zmq PAIR the renderer binds. `command_sync` and `input_sync`
use the `ipc://user://` addresses too, resolved against `--tg-ipc-dir`.

The import is the Linux one with `glImportMemoryWin32HandleEXT` in place of
`glImportMemoryFdEXT` and the size from the probe. A Win32 import does not take
ownership of the handle, so the renderer closes its duplicate afterwards. The
extension entry points come from `wglGetProcAddress`, and `argv` from
`CommandLineToArgvW`.

### macOS: IOSurface instead of a memory object

macOS GL has no `EXT_external_objects` at all, but it does not need one. The
launcher's MoltenVK exports the image as an `IOSurface` and sends its
`IOSurfaceGetID` over a one-shot zmq PAIR the renderer binds at
`ipc:///tmp/external_texture` — the same exchange the 4.x renderer uses.
`IOSurfaceLookup` turns the id back into the surface, and
`CGLTexImageIOSurface2D` binds it to a `GL_TEXTURE_RECTANGLE`, the only target
macOS GL accepts for a surface. From there the draw FBO and the blit are the
Linux ones.

Two things differ. The size comes with the surface (`IOSurfaceGetWidth` /
`IOSurfaceGetHeight`, checked against `--resolution`), so there is no
allocation size to recover. And the surface declares its own channel order:
the renderer uploads with `GL_BGRA` or `GL_RGBA` to match
`IOSurfaceGetPixelFormat` and reports the matching `ext_texture_format`, which
`RenderResult` already turns into a channel swap.

`argv` comes from `_NSGetArgv()`, since there is no `/proc/self/cmdline`.
Godot 3's `set_window_size` is a no-op under `--no-window` on macOS, so the
window keeps the size `--resolution` gave it at creation.

### Why `--no-window` matters beyond hiding the window

The launcher sizes the shared texture from `RenderResult`, passes the same size
as `--resolution`, and forwards input in that coordinate space. A window manager
that adopts the renderer's window resizes it — observed at 2160x3702 on one run
and 1440x1590 on the next, against a 1440x2416 texture — which distorts the blit
and puts input in the wrong space. `--no-window` leaves the window unmapped, so
no WM ever touches it and the render target is exactly what the launcher asked
for.

The blit scales the root viewport onto the shared texture, because a gate's own
`display/window/stretch` settings can make the root viewport a different size
from the window on purpose. Input is scaled to the *window* instead: Godot 3's
`Viewport` applies the stretch transform to incoming events itself, so scaling
them to the viewport as well lands every click short under `stretch/mode =
"viewport"`.

## Input translation

`input_sync` carries `InputEvent`s as text Variants
(`Object(InputEventKey,"keycode":65,…)`), and Godot 3 cannot consume Godot 4's
property names. `input_event_compat` rewrites the payload's `"k":v` pairs into a
Dictionary — object properties are already dictionary syntax, so it only needs
rebracketing — then applies a rename table (`keycode`→`scancode`,
`alt_pressed`→`alt`, `velocity`→`speed`, …) onto a Godot 3 event instance.

Keycodes need remapping too: Godot 4 numbers special keys from `SPECIAL = 1<<22`
and Godot 3 from `SPKEY = 1<<24`, and Godot 4 inserted F17–F35 at `0x2C`, which
shifted every code above it. Codes at or below F16 and the whole keypad block are
identical, so everything a game normally reads survives; the shifted tail
(SUPER, MENU, media and launch keys) goes through a lookup table, and anything
Godot 3 lacks arrives as `KEY_UNKNOWN`.

`tests/test_keycode_map.py` re-derives that mapping from both engines'
`core/os/keyboard.h` and fails on any table entry that disagrees. It needs no
build; run it after touching the table.

## Gate commands

A 4.x gate reaches the launcher with `get_tree().send_command(name, args)`, a
method the fork adds to `SceneTree`. A Godot 3 module cannot add methods to an
engine class, so the renderer registers an engine singleton instead, and only
when it runs inside TheGates:

```gdscript
if Engine.has_singleton("TheGates"):
	Engine.get_singleton("TheGates").send_command("open_gate", ["other.gate"])
```

The commands and their handling are the 4.x ones (`open_gate` relative to the
current gate, `open_link`, `highlight_button`; see [[Two-Process Model]]).
Going through `Engine.get_singleton` keeps the script loadable in a plain
Godot 3 editor, where the singleton does not exist; referring to `TheGates`
directly would not parse there.

`TheGates` is a global name in the gate's GDScript, like every registered
singleton: a gate declaring a class or autoload of that name fails to parse.

Two things the renderer does on its own: `set_mouse_mode` is forwarded by
polling `Input::get_mouse_mode()` each frame, and a gate that quits sends
`exit_gate` from `TGRendererLifecycle::teardown`, which only a clean exit
reaches.

## Building and testing

```bash
python godot3-modules/build.py                  # dev renderer
python godot3-modules/build.py renderer3-release
python godot3-modules/build.py --stage-to app/renderer
python godot3-modules/build.py renderer3 --platform osx -- arch=arm64       # on a Mac
python godot3-modules/build.py renderer3 --platform windows -- use_mingw=yes
```

Needs both submodules: `godot3/` for the engine, `godot/` for the vendored
libzmq.

End to end, without a published 3.6 renderer on the backend:

```bash
python3 godot3-modules/tests/testgate/serve.py --renderer godot3/bin/godot.x11.opt.debug.64
# set app/resources/api_settings.tres host_type = 0 (Local), then:
godot/bin/godot.linuxbsd.editor.dev.x86_64.llvm --path app -- \
    --autotest --gate-url http://127.0.0.1:8000/test.gate
```

`serve.py` stands in for the whole backend: it packs the test project, serves
the `.gate`, and answers `/api/download_renderer/linux-3.6` with the binary you
point it at. The test gate draws a moving background, a square that tracks the
mouse, and a readout of frame count, mouse position, clicks and last key — so
one look confirms the transport and both input paths.

## Gaps

Known and deliberate, in rough order of how much they hurt:

- **macOS and Windows are compile-verified, not run.** Both follow the 4.x
  renderer's handle exchange and the documented GL interop contracts, and the
  size probe is verified against `llseek` on Linux, but neither has driven a
  launcher on real hardware yet. OpenGL on Apple Silicon is Apple's translation
  layer over Metal; IOSurface binding is part of it. On Windows the renderer
  runs inside the launcher's Chromium sandbox without lowering its token, and
  whether the GL driver loads under that token is untested.
- **No sandbox.** The 4.x renderer lowers its token via `Sandbox::lower_token`
  before loading gate code. The Godot 3 renderer does not, so a 3.6 gate runs
  with the launcher's privileges. `SandboxLinux::spawn_target` applies nothing
  before `exec`, so the renderer is not crippled — it is simply unconfined. See
  [[Sandboxing/Architecture]].
- **No network broker.** `RendererNetClient` / `BrokeredNetSocket` are not
  ported, so a 3.6 gate's HTTP goes straight out instead of through the
  launcher's broker. The inherited `--tg-broker-fd` is ignored. See
  [[Network Isolation]].
- **Assumes the default `render_thread_mode`.** `frame_post_draw` is emitted
  from whichever thread runs `VisualServerRaster::draw()`. Under Godot 3's
  default ("Safe") that is the main thread, which is what the GL blit and
  `Input::parse_input_event` both need. A gate setting it to "Separate" is
  untested.
- **The size probe assumes the launcher's image description.** It re-creates the
  image `RenderResult.create_external_texture` makes. Changing that texture's
  format, usage bits or allocation flags in the launcher without updating
  `vulkan_memory_probe.cpp` would size imports wrong on Windows and on any
  Linux driver whose fd reports no length. Sending the size over IPC would remove
  the coupling, and it needs `VmaAllocationInfo::size` plumbed out of the fork's
  `external_texture_create`.
- **No GL/Vulkan semaphores.** The blit and the launcher's `texture_copy` are
  unsynchronized, exactly as the 4.x path is between its two processes.

## Related

- [[Two-Process Model]] — the IPC protocol both engines implement
- [[External Texture Sharing]] — the Vulkan-to-Vulkan path this mirrors
- [[Gate Format and Lifecycle]] — where `godot_version` comes from
- [[Custom Godot Fork]] — what the 4.5 fork changes, and why Godot 3 needs none of it in tree
