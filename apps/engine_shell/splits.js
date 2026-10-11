// Resizable splits: each separator drives one CSS variable on its container, by pointer drag or by
// the arrow keys when it has focus, and the sizes are remembered per browser.
import { store } from "./dom.js";

const SPLITS = {
  // name: [container id, variable, axis, which side the size measures]
  left: ["workspace", "--col-left", "x", "start"],
  right: ["workspace", "--col-right", "x", "end"],
  "left-rows": ["col-left", "--row-left-bottom", "y", "end"],
  "center-rows": ["col-center", "--row-center-bottom", "y", "end"],
};

const LIMITS = { x: [180, 900], y: [96, 1200] };

export function installSplits(onResize) {
  const saved = store.get("splits", {});
  for (const [name, [containerId, variable]] of Object.entries(SPLITS)) {
    if (typeof saved[name] === "number") {
      document.getElementById(containerId).style.setProperty(variable, `${saved[name]}px`);
    }
  }
  for (const handle of document.querySelectorAll("[data-split]")) {
    const name = handle.dataset.split;
    const spec = SPLITS[name];
    if (!spec) continue;
    const [containerId, variable, axis, side] = spec;
    const container = document.getElementById(containerId);
    const set = (size) => {
      const [lo, hi] = LIMITS[axis];
      const clamped = Math.round(Math.min(hi, Math.max(lo, size)));
      container.style.setProperty(variable, `${clamped}px`);
      saved[name] = clamped;
      return clamped;
    };
    const sizeAt = (event) => {
      const r = container.getBoundingClientRect();
      if (axis === "x") return side === "start" ? event.clientX - r.left : r.right - event.clientX;
      return side === "start" ? event.clientY - r.top : r.bottom - event.clientY;
    };
    handle.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.preventDefault();
      handle.setPointerCapture(event.pointerId);
      handle.classList.add("dragging");
      document.body.classList.add("resizing");
      document.body.style.cursor = axis === "x" ? "col-resize" : "row-resize";
      const move = (e) => {
        set(sizeAt(e));
        onResize();
      };
      const up = () => {
        handle.removeEventListener("pointermove", move);
        handle.removeEventListener("pointerup", up);
        handle.removeEventListener("pointercancel", up);
        handle.classList.remove("dragging");
        document.body.classList.remove("resizing");
        document.body.style.cursor = "";
        store.set("splits", saved);
        onResize(true);
      };
      handle.addEventListener("pointermove", move);
      handle.addEventListener("pointerup", up);
      handle.addEventListener("pointercancel", up);
    });
    handle.addEventListener("keydown", (event) => {
      const grow = axis === "x" ? (side === "start" ? "ArrowRight" : "ArrowLeft")
                                : (side === "start" ? "ArrowDown" : "ArrowUp");
      const shrink = axis === "x" ? (side === "start" ? "ArrowLeft" : "ArrowRight")
                                  : (side === "start" ? "ArrowUp" : "ArrowDown");
      if (event.key !== grow && event.key !== shrink) return;
      event.preventDefault();
      const current = parseFloat(getComputedStyle(container).getPropertyValue(variable)) ||
        (axis === "x" ? 300 : 200);
      set(current + (event.key === grow ? 16 : -16));
      store.set("splits", saved);
      onResize(true);
    });
  }
}
