// The engine shell's entry point (docs/subsystems/engine_shell.md). It holds the connection, the
// session, the selection, and the one rule every panel follows: a change to the document is a
// protocol call, and after one, every panel asks the host again. Nothing here reads a file or
// knows a type; the page's only source of truth is the host it was served from.
//
// The page learns of changes from **events** (`session.subscribe`; protocol.md, "Events"): on every
// connect it subscribes to the session's journal, selection, scene and session events, and a
// journal event — anyone's commit, undo or redo: this page's, an agent's through engine-mcp, the
// editor's viewport — is what refreshes the panels, a selection event what moves the outliner's
// highlight. When the host cannot push (an older host), the page polls `session.info` every ten
// seconds instead, and the status strip's "events" badge says which it is doing.
//
// The URL carries the host's token and, when the host was started with --doc, the session:
// http://127.0.0.1:<port>/?token=<token>&session=<id>, which is the `shell` the host printed.
import { Console } from "./console.js";
import { $, toast } from "./dom.js";
import { Journal } from "./journal.js";
import { Outliner } from "./outliner.js";
import { Properties } from "./properties.js";
import { Rpc } from "./rpc.js";
import { installSplits } from "./splits.js";
import { Viewport } from "./viewport.js";
import { EmbeddedViewport } from "./embedded.js";

class App extends EventTarget {
  constructor() {
    super();
    const query = new URLSearchParams(location.search);
    this.token = query.get("token") || "";
    this.session = query.get("session") || "";
    // `?embedded=1`: the page fills engine-editor's webview beside its native viewport (embedded.js).
    this.embedded = query.get("embedded") === "1";
    document.documentElement.classList.toggle("embedded", this.embedded);
    this.selection = "";
    this.identity = { actor: "", role: "" };
    this.journalLength = -1;
    this.undoPosition = -1;
    this.started = false;
    this.rpc = new Rpc(`ws://${location.host}/?token=${encodeURIComponent(this.token)}`);
    this.outliner = new Outliner(this);
    this.properties = new Properties(this);
    this.journal = new Journal(this);
    this.console = new Console(this);
    this.viewport = this.embedded ? new EmbeddedViewport(this) : new Viewport(this);
    this.refreshing = null;
    this.refreshAgain = false;
    // True while the host pushes this page its events; false polls (see the badge).
    this.subscribed = false;
  }

  start() {
    installSplits((done) => {
      if (done) this.viewport.capture("final");
    });
    this.status();
    if (!this.token) {
      this.setConnection("refused", "no token in this page's address: open the shell URL engine-host printed");
      return;
    }
    this.rpc.addEventListener("state", (e) => this.onState(e.detail));
    this.rpc.start();
    $("btn-undo").addEventListener("click", () => this.step("doc.undo"));
    $("btn-redo").addEventListener("click", () => this.step("doc.redo"));
    document.addEventListener("keydown", (e) => {
      const typing = e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement;
      if (typing || !(e.ctrlKey || e.metaKey) || e.code !== "KeyZ") return;
      e.preventDefault();
      this.step(e.shiftKey ? "doc.redo" : "doc.undo");
    });
    // Events drive the panels; the poll is the fallback for a host that cannot push, and looks
    // only while no subscription stands (`poll` returns at once otherwise).
    this.rpc.addEventListener("notification", (e) => this.onEvent(e.detail));
    setInterval(() => this.poll(), 10000);
    this.properties.refresh();
  }

  // Subscribes this connection to the session's events, on every (re)connect: a subscription is
  // the connection's and goes with it.
  async subscribe() {
    this.subscribed = false;
    if (this.session) {
      try {
        await this.rpc.call("session.subscribe", {
          session: this.session,
          kinds: ["journal", "selection", "scene", "session"],
        });
        this.subscribed = true;
      } catch (err) {
        this.eventsWhy = err && err.message ? err.message : String(err);
      }
    }
    this.status();
  }

  onEvent(message) {
    const p = message.params || {};
    if (p.session && p.session !== this.session) return;
    // A full queue dropped some: what was missed is unknown, so everything is read again.
    if (p.dropped) {
      this.changed();
      return;
    }
    switch (message.method) {
      case "event.journal":
        if (p.journal_length !== this.journalLength || p.undo_position !== this.undoPosition) {
          this.changed();
        }
        break;
      case "event.selection":
        this.select(p.selected || "", true);
        break;
      case "event.scene":
        if (this.viewport.onScene) this.viewport.onScene(p);
        break;
      case "event.session":
        if (p.change === "closed") this.reportError({ message: `session ${p.session} was closed` });
        else if (p.change === "lease_acquired") toast(`${p.actor} leased ${p.layer || "a layer"}`);
        else if (p.change === "lease_released") toast(`${p.actor} released a lease on ${p.layer || "a layer"}`);
        break;
      default:
        break;
    }
  }

  call(method, params) {
    return this.rpc.call(method, params).catch((err) => {
      this.reportError(err);
      throw err;
    });
  }

  onState({ state, detail }) {
    this.setConnection(state, detail);
    if (state === "open") {
      if (this.started) toast("Reconnected to the host");
      this.started = true;
      this.init();
    } else {
      this.subscribed = false;
      this.status();
    }
  }

  setConnection(state, detail) {
    const pill = $("conn-pill");
    pill.dataset.state = state;
    pill.textContent = {
      open: "connected",
      connecting: "connecting",
      reconnecting: "reconnecting",
      refused: "refused",
    }[state] || state;
    pill.title = detail || "";
    if (state === "reconnecting") this.reportError({ message: `connection lost (${detail}); reconnecting` });
    if (state === "refused") {
      this.reportError({ message: detail });
      $("viewport-empty").textContent = `The host refused this page: ${detail}`;
    }
  }

  async init() {
    try {
      const [info, roles] = await Promise.all([this.call("engine.info", null), this.call("engine.roles", null)]);
      this.engine = info;
      this.identity = { actor: roles.actor, role: roles.role || roles.default_role };
      this.status();
      await this.subscribe();
      await this.useSession(this.session, true);
    } catch {
      /* reported */
    }
  }

  async useSession(session, force = false) {
    if (!force && session === this.session) return;
    const switched = session !== this.session;
    this.session = session;
    if (switched && this.rpc.state === "open") await this.subscribe();
    if (session) {
      const url = new URL(location.href);
      url.searchParams.set("session", session);
      history.replaceState(null, "", url);
    }
    this.status();
    await Promise.allSettled([this.console.load()]);
    if (!session) {
      $("doc-title").textContent = "no session: start the host with --doc <dir>, or session.open on the command line";
      await this.viewport.load();
      return;
    }
    await this.refreshAll(true);
  }

  async refreshAll(withViewport) {
    // One refresh at a time; a change during one runs another after it.
    if (this.refreshing) {
      this.refreshAgain = true;
      return this.refreshing;
    }
    this.refreshing = (async () => {
      try {
        const info = await this.call("session.info", { session: this.session });
        this.journalLength = info.journal_length;
        this.undoPosition = info.undo_position;
        const doc = $("doc-title");
        doc.textContent = "";
        doc.append(document.createElement("strong"));
        doc.firstChild.textContent = info.name;
        doc.append(`  ${info.path}  ·  ${info.layers.length} layer${info.layers.length === 1 ? "" : "s"}`);
        await this.outliner.refresh();
        await Promise.all([this.journal.refresh(), this.properties.refresh()]);
        if (withViewport) await this.viewport.sync();
      } catch {
        /* reported */
      }
    })();
    await this.refreshing;
    this.refreshing = null;
    if (this.refreshAgain) {
      this.refreshAgain = false;
      await this.refreshAll(true);
    }
  }

  changed() {
    return this.refreshAll(true);
  }

  // The fallback: only while the host pushes nothing (the badge says "poll 10 s").
  async poll() {
    if (this.subscribed) return;
    if (!this.session || this.rpc.state !== "open" || document.visibilityState !== "visible") return;
    if (this.refreshing) return;
    try {
      const info = await this.rpc.call("session.info", { session: this.session });
      if (info.journal_length !== this.journalLength || info.undo_position !== this.undoPosition) {
        await this.changed();
      }
    } catch {
      /* the next look will say */
    }
  }

  // `fromEditor`: the editor reported it, so it is not sent back. Otherwise, embedded, the editor's
  // viewport is told (`editor.select`) so it outlines the record and its gizmo attaches.
  select(id, fromEditor = false) {
    if (id === this.selection) return;
    this.selection = id;
    this.dispatchEvent(new Event("selection"));
    if (this.embedded && !fromEditor) {
      this.call("editor.select", { id: id || null }).catch(() => {
        /* reported */
      });
    }
  }

  // One transaction with the person's rationale; true when it committed.
  async apply(commands, rationale) {
    try {
      const result = await this.call("doc.apply", {
        session: this.session,
        commands,
        attribution: { rationale },
      });
      if (!result.committed) {
        const why = (result.diagnostics || []).map((d) => d.message || JSON.stringify(d)).join("; ");
        this.reportError({ message: `not committed: ${why || "no reason given"}` });
        return false;
      }
      this.clearError();
      // Subscribed, the commit's own journal event refreshes the panels (one refresh, whoever made
      // the change); without events the page refreshes now.
      if (!this.subscribed) await this.changed();
      return true;
    } catch {
      return false;
    }
  }

  async step(method) {
    if (!this.session) return;
    try {
      const result = await this.call(method, { session: this.session });
      if (result.stepped === 0) toast(method === "doc.undo" ? "Nothing to undo" : "Nothing to redo");
      if (!this.subscribed) await this.changed();
    } catch {
      /* reported */
    }
  }

  setUndoRedo(canUndo, canRedo) {
    $("btn-undo").disabled = !canUndo;
    $("btn-redo").disabled = !canRedo;
  }

  reportError(err) {
    const el = $("st-error");
    const code = err && err.code ? ` (${err.code})` : "";
    el.textContent = err && err.message ? `${err.message}${code}` : "";
    el.title = el.textContent;
  }

  clearError() {
    $("st-error").textContent = "";
  }

  status() {
    const item = (id, label, value) => {
      const el = $(id);
      el.textContent = "";
      const b = document.createElement("b");
      b.textContent = label;
      el.append(b, ` ${value}`);
    };
    item("st-host", "host", location.host);
    item("st-session", "session", this.session || "none");
    item("st-actor", "actor", this.identity.actor || "—");
    item("st-role", "role", this.identity.role || "unrestricted");
    item("st-token", "token", this.token ? "present" : "missing");
    // Which way the page learns of changes: pushed events, or the ten-second poll and why.
    const events = $("st-events");
    events.dataset.mode = this.subscribed ? "events" : "poll";
    item("st-events", "updates", this.subscribed ? "events" : "poll 10 s");
    events.title = this.subscribed
      ? "The host pushes this page every commit, undo, redo, selection and scene change as it happens"
      : `No subscription: the page asks the host every ten seconds${this.eventsWhy ? ` (${this.eventsWhy})` : ""}`;
  }
}

const app = new App();
window.engineShell = app; // for a person at the browser's console; nothing reads it
app.start();
