// The viewport cell of the embedded shell (docs/subsystems/engine_shell.md, "Embedded in the
// editor"). In engine-editor's window the page fills the client area of an OS webview and the
// editor's own Vulkan viewport is a native child window laid over this cell, so the cell draws
// nothing: it is a dark backing and a rectangle. The page tells the editor where the rectangle is —
// `editor.layout`, a notification, in CSS pixels from the page's corner with the device pixel ratio
// — on load, on every split drag, on a window resize and on a change of ratio, and the editor
// moves its viewport there. Nothing is composited and nothing is sent per frame: the viewport runs
// at the display's rate, and the page speaks only when its layout changes.
//
// The same object stands where the slideshow's `Viewport` stands in main.js (`capture`, `load`,
// `sync`), so nothing else in the shell knows which mode it is in.
import { $ } from "./dom.js";

export class EmbeddedViewport {
  constructor(app) {
    this.app = app;
    this.el = $("viewport");
    this.meta = $("viewport-meta");
    this.last = "";
    this.queued = false;
    this.ratio = null;
    // The cell's own size changes (a split, the window) and the page's ratio (a move to another
    // display, the zoom) are the two things that move the rectangle.
    new ResizeObserver(() => this.capture()).observe(this.el);
    window.addEventListener("resize", () => this.capture());
    this.watchRatio();
    app.rpc.addEventListener("state", (e) => {
      if (e.detail.state === "open") this.send(true);
    });
  }

  watchRatio() {
    if (this.ratio) this.ratio.removeEventListener("change", this.onRatio);
    this.ratio = matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`);
    this.onRatio = () => {
      this.watchRatio();
      this.capture();
    };
    this.ratio.addEventListener("change", this.onRatio);
  }

  // Called where the slideshow would capture (a split moved, the window resized): coalesced to one
  // layout a frame, sent once the browser has laid the page out.
  capture() {
    if (this.queued) return;
    this.queued = true;
    requestAnimationFrame(() => {
      this.queued = false;
      this.send(false);
    });
  }

  send(force) {
    const r = this.el.getBoundingClientRect();
    const layout = {
      x: r.left,
      y: r.top,
      width: r.width,
      height: r.height,
      device_pixel_ratio: window.devicePixelRatio || 1,
    };
    const key = JSON.stringify(layout);
    if (!force && key === this.last) return;
    if (this.app.rpc.notify("editor.layout", layout)) this.last = key;
    this.meta.textContent = `native · ${Math.round(r.width * layout.device_pixel_ratio)} × ${Math.round(
      r.height * layout.device_pixel_ratio)}`;
  }

  async load() {
    this.send(true);
  }

  async sync() {
    this.send(false);
  }
}
