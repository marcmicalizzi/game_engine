# window (foundation)

**Purpose.** OS windows, input events, and the two pieces of Vulkan glue a window system owns: the instance extensions a surface needs and surface creation. SDL3 (ADR-0012, zlib) sits underneath, statically linked with video and events only; nothing outside this module includes SDL. Headless machines keep working: `init()` reports failure and callers run without a window, which is how the CI runners and `engine-host` behave.

**API.** `window::init()` starts the video subsystem (false with the reason on a machine without a display). `Window::create(WindowDesc)` opens a resizable, high-DPI-aware window; `pixel_width()/pixel_height()` are the framebuffer size, refreshed on every `Resized` event. `poll(Event&)` pulls one event at a time: `Quit`, `CloseRequested`, `Resized`, focus, `KeyDown/KeyUp` (a small `Key` enum for the common keys plus the raw scancode), mouse move, buttons, and wheel. `Window::vulkan_instance_extensions()` feeds `gfx::DeviceOptions::instance_extensions`; `create_vulkan_surface(instance)` and `destroy_vulkan_surface` bracket a `gfx::Swapchain`. Vulkan handle types are forward-declared the way SDL does it, so the header depends on neither SDL nor the Vulkan headers.

**Build.** `cmake/EngineGraphics.cmake` fetches SDL `release-3.4.16` (`ENGINE_SDL_TAG`) and builds it static with audio, render, GPU, camera, and sensor subsystems off; joystick and haptics stay on for gamepads. On Linux without X11 or Wayland development headers SDL builds with no video driver, and `init()` fails cleanly.

**Depends on.** `base`, `containers`, `log`; SDL3 (third_party/LICENSES.md).

**Testing.** `tools/dev.ps1 test -Filter window`: creates a hidden window, checks the pixel size, the surface extension list, event draining, rename, destroy, and shutdown; records a skip on machines without a display. `domain/gfx`'s swapchain test and `engine-view`'s end-to-end test open real windows.

**Not yet.** Gamepads, text input and IME, clipboard, multiple windows and per-output presentation for surround (docs/plan/04-renderer.md §4.8), cursor capture for first-person controls, and the input mapping layer that games see (a later module; this one stays raw).

**Performance notes.** Event polling is a few microseconds per frame. SDL's static library adds about a minute to a cold CI build.
