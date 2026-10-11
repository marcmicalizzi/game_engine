// The viewport: the session's scene as the host draws it — `render.load {session}` once, then
// `render.capture` at the panel's size with `keep`, fetched from the listener (`GET /capture/<id>`)
// so the page never needs a file path. It is a **slideshow**, not a live view: every gesture ends
// in a capture, and while a gesture lasts the page asks for half-size captures back to back so the
// motion can be followed. The camera is the host's (A32: every capture reports the camera it drew
// with, and the page flies from that report, in engine-view's fly convention), and a click reads
// the capture's id buffer at that pixel and selects the record the host says that instance is.
import { $ } from "./dom.js";

const MOVE_KEYS = new Set(["KeyW", "KeyA", "KeyS", "KeyD", "KeyQ", "KeyE"]);
const LOOK_SPEED = 0.0042; // radians per pixel of drag
const NO_INSTANCE = 0xffffffff;
const NULL_ID = "00000000000000000000000000000000";

export class Viewport {
  constructor(app) {
    this.app = app;
    this.el = $("viewport");
    this.image = $("viewport-image");
    this.empty = $("viewport-empty");
    this.hud = $("viewport-camera");
    this.busy = $("viewport-busy");
    this.marker = $("viewport-marker");
    this.meta = $("viewport-meta");
    this.scene = "";
    this.radius = 1;
    this.camera = null; // {position: [x,y,z], yaw, pitch, fov, znear}; null: the host frames
    this.shot = null; // the capture on screen: {id, width, height, records, ms}
    this.ids = new Map(); // capture id -> Promise<Uint32Array>
    this.capturing = false;
    this.again = null; // a capture asked for while one was in flight: "preview" or "final"
    this.keys = new Set();
    this.hurry = false;
    this.flying = false;
    this.drag = null;
    this.unavailable = "";
    // Counts every change the page makes to the camera, so a capture's report is adopted only when
    // nothing moved while it was being drawn (a late preview must not pull the camera back).
    this.motion = 0;
    this.install();
  }

  install() {
    const el = this.el;
    el.addEventListener("keydown", (e) => {
      if (e.code === "ShiftLeft" || e.code === "ShiftRight") this.hurry = true;
      if (MOVE_KEYS.has(e.code)) {
        e.preventDefault();
        this.keys.add(e.code);
        this.startFlying();
      } else if (e.code === "KeyF" && !e.ctrlKey) {
        this.frame();
      } else if (e.code === "KeyR" && !e.ctrlKey) {
        this.capture("final");
      }
    });
    el.addEventListener("keyup", (e) => {
      if (e.code === "ShiftLeft" || e.code === "ShiftRight") this.hurry = false;
      if (this.keys.delete(e.code) && this.keys.size === 0) this.endGesture();
    });
    el.addEventListener("blur", () => {
      if (this.keys.size) {
        this.keys.clear();
        this.endGesture();
      }
    });
    el.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      el.focus();
      el.setPointerCapture(e.pointerId);
      this.drag = { x: e.clientX, y: e.clientY, moved: false };
    });
    el.addEventListener("pointermove", (e) => {
      if (!this.drag || !this.camera) return;
      const dx = e.clientX - this.drag.x;
      const dy = e.clientY - this.drag.y;
      if (!this.drag.moved && Math.hypot(dx, dy) < 4) return;
      this.drag.moved = true;
      el.classList.add("looking");
      this.drag.x = e.clientX;
      this.drag.y = e.clientY;
      this.motion++;
      this.camera.yaw -= dx * LOOK_SPEED;
      this.camera.pitch = Math.max(-1.5, Math.min(1.5, this.camera.pitch - dy * LOOK_SPEED));
      this.showCamera();
      this.capture("preview");
    });
    const up = (e) => {
      if (!this.drag) return;
      const { moved } = this.drag;
      this.drag = null;
      el.classList.remove("looking");
      if (moved) this.capture("final");
      else if (e.type === "pointerup") this.pick(e.clientX, e.clientY);
    };
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    el.addEventListener("wheel", (e) => {
      if (!this.camera) return;
      e.preventDefault();
      const step = Math.max(this.radius, 1) * 0.08 * (e.deltaY > 0 ? -1 : 1) * (e.shiftKey ? 4 : 1);
      this.move(this.forward(), step);
      this.showCamera();
      clearTimeout(this.wheelTimer);
      this.capture("preview");
      this.wheelTimer = setTimeout(() => this.capture("final"), 220);
    }, { passive: false });
    $("btn-frame").addEventListener("click", () => this.frame());
    $("btn-capture").addEventListener("click", () => this.capture("final"));
    new ResizeObserver(() => {
      clearTimeout(this.resizeTimer);
      this.resizeTimer = setTimeout(() => this.capture("final"), 250);
    }).observe(el);
  }

  // ---- the camera, in engine-view's convention: forward = (-cos p sin y, sin p, -cos p cos y) ----

  forward() {
    const { yaw, pitch } = this.camera;
    return [-Math.cos(pitch) * Math.sin(yaw), Math.sin(pitch), -Math.cos(pitch) * Math.cos(yaw)];
  }

  right() {
    const { yaw } = this.camera;
    return [Math.cos(yaw), 0, -Math.sin(yaw)];
  }

  move(direction, distance) {
    if (distance === 0) return;
    this.motion++;
    const p = this.camera.position;
    for (let i = 0; i < 3; ++i) p[i] += direction[i] * distance;
  }

  startFlying() {
    if (this.flying || !this.camera) return;
    this.flying = true;
    let last = performance.now();
    const step = (now) => {
      if (this.keys.size === 0) {
        this.flying = false;
        return;
      }
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      const speed = Math.max(this.radius, 1) * 0.6 * (this.hurry ? 4 : 1) * dt;
      const f = this.forward();
      const r = this.right();
      const k = this.keys;
      const axis = (pos, neg) => (k.has(pos) ? 1 : 0) - (k.has(neg) ? 1 : 0);
      this.move(f, axis("KeyW", "KeyS") * speed);
      this.move(r, axis("KeyD", "KeyA") * speed);
      this.move([0, 1, 0], axis("KeyE", "KeyQ") * speed);
      this.showCamera();
      this.capture("preview");
      requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  }

  endGesture() {
    this.capture("final");
  }

  frame() {
    this.motion++;
    this.camera = null;
    this.capture("final");
  }

  cameraParams() {
    if (!this.camera) return undefined;
    const f = this.forward();
    const d = Math.max(this.radius, 1);
    const p = this.camera.position;
    return {
      position: p.slice(),
      target: [p[0] + f[0] * d, p[1] + f[1] * d, p[2] + f[2] * d],
      fov_deg: this.camera.fov,
      znear: this.camera.znear,
    };
  }

  showCamera(report) {
    const c = this.camera;
    if (!c) {
      this.hud.textContent = "";
      return;
    }
    const deg = (r) => (r * 180 / Math.PI).toFixed(1).padStart(6);
    const num = (v) => v.toFixed(2).padStart(9);
    let text = `pos  ${c.position.map(num).join("")}\nyaw ${deg(c.yaw)}°  pitch ${deg(c.pitch)}°  fov ${c.fov.toFixed(0)}°`;
    const shot = report || this.shot;
    if (shot) text += `\n${shot.width}×${shot.height} · ${shot.ms.toFixed(0)} ms · ${shot.id}`;
    this.hud.textContent = text;
  }

  // ---- loading and capturing ------------------------------------------------------------------

  async load() {
    if (!this.app.session) {
      this.empty.textContent = "No session. Start the host with --doc <dir>, or session.open one on the command line.";
      this.empty.hidden = false;
      return;
    }
    try {
      const info = await this.app.call("render.load", { session: this.app.session });
      this.scene = info.scene;
      this.radius = info.radius || 1;
      this.unavailable = "";
      this.meta.textContent = `${info.scene} · ${info.instances} instance${info.instances === 1 ? "" : "s"} · ${info.raster}`;
      return info;
    } catch (err) {
      this.scene = "";
      this.unavailable = err.code === 1007
        ? `This machine cannot render: ${err.message}`
        : `render.load failed: ${err.message}`;
      this.empty.textContent = this.unavailable;
      this.empty.hidden = false;
      this.app.reportError(err);
      return null;
    }
  }

  // After a transaction: the same session loaded again moves its instances in place, or rebuilds
  // when the bridge says it must, and then the picture is taken again.
  async sync() {
    const info = await this.load();
    if (info) {
      if (info.rebuilt === false && info.moved) this.meta.textContent += ` · moved ${info.moved}`;
      await this.capture("final");
    }
  }

  size(kind) {
    const r = this.el.getBoundingClientRect();
    const scale = Math.min(window.devicePixelRatio || 1, 1.5) * (kind === "preview" ? 0.5 : 1);
    const w = Math.max(32, Math.min(3840, Math.round(r.width * scale)));
    const h = Math.max(32, Math.min(2160, Math.round(r.height * scale)));
    return [w, h];
  }

  async capture(kind = "final") {
    if (!this.scene) return;
    if (this.capturing) {
      // One capture at a time; the newest request waits, and a final one is never dropped for a
      // preview.
      if (this.again !== "final") this.again = kind;
      return;
    }
    this.capturing = true;
    this.busy.hidden = false;
    this.image.classList.add("stale");
    const [width, height] = this.size(kind);
    const started = performance.now();
    const motion = this.motion;
    try {
      const result = await this.app.call("render.capture", {
        scene: this.scene,
        width,
        height,
        channels: ["color", "ids"],
        keep: true,
        camera: this.cameraParams(),
      });
      const url = `/capture/${encodeURIComponent(result.capture)}?token=${encodeURIComponent(this.app.token)}`;
      const img = new Image();
      img.src = url;
      await img.decode();
      this.image.src = url;
      this.empty.hidden = true;
      const report = result.camera;
      // The host's report is the camera from here on: what it drew is what the page flies from.
      if (motion === this.motion && !this.drag && this.keys.size === 0) {
        this.camera = {
          position: report.position.slice(),
          yaw: report.yaw_deg * Math.PI / 180,
          pitch: report.pitch_deg * Math.PI / 180,
          fov: report.fov_deg,
          znear: report.znear,
        };
      }
      this.shot = {
        id: result.capture,
        width: result.width,
        height: result.height,
        records: result.records || [],
        ms: performance.now() - started,
        kind,
      };
      this.showCamera();
    } catch (err) {
      this.app.reportError(err);
    } finally {
      this.capturing = false;
      this.busy.hidden = true;
      this.image.classList.remove("stale");
      const next = this.again;
      this.again = null;
      if (next) this.capture(next);
    }
  }

  // ---- picking --------------------------------------------------------------------------------

  idsOf(shot) {
    if (!this.ids.has(shot.id)) {
      const url = `/capture/${encodeURIComponent(shot.id)}/ids?token=${encodeURIComponent(this.app.token)}`;
      this.ids.set(shot.id, fetch(url).then((r) => {
        if (!r.ok) throw new Error(`the host no longer keeps capture ${shot.id}`);
        return r.arrayBuffer();
      }).then((b) => new Uint32Array(b)));
      // Keep a few: the host keeps eight.
      if (this.ids.size > 4) this.ids.delete(this.ids.keys().next().value);
    }
    return this.ids.get(shot.id);
  }

  async pick(clientX, clientY) {
    const shot = this.shot;
    if (!shot) return;
    const r = this.el.getBoundingClientRect();
    const scale = Math.min(r.width / shot.width, r.height / shot.height);
    const ox = (r.width - shot.width * scale) / 2;
    const oy = (r.height - shot.height * scale) / 2;
    const px = Math.floor((clientX - r.left - ox) / scale);
    const py = Math.floor((clientY - r.top - oy) / scale);
    this.marker.style.left = `${clientX - r.left}px`;
    this.marker.style.top = `${clientY - r.top}px`;
    this.marker.hidden = false;
    this.marker.style.animation = "none";
    void this.marker.offsetWidth;
    this.marker.style.animation = "";
    if (px < 0 || py < 0 || px >= shot.width || py >= shot.height) {
      this.app.select("");
      return;
    }
    try {
      const ids = await this.idsOf(shot);
      const instance = ids[(py * shot.width + px) * 3];
      const record = instance !== NO_INSTANCE && instance < shot.records.length ? shot.records[instance] : "";
      this.app.select(record && record !== NULL_ID ? record : "");
    } catch (err) {
      this.app.reportError(err);
    }
  }
}
