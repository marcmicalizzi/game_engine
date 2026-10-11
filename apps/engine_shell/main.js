// The engine shell's entry point (docs/subsystems/engine_shell.md). It holds the connection, the
// session, the selection, and the one rule every panel follows: a change to the document is a
// protocol call, and after one, every panel asks the host again. Nothing here reads a file or
// knows a type; the page's only source of truth is the host it was served from.
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

class App extends EventTarget {
  constructor() {
    super();
    const query = new URLSearchParams(location.search);
    this.token = query.get("token") || "";
    this.session = query.get("session") || "";
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
    this.viewport = new Viewport(this);
    this.refreshing = null;
    this.refreshAgain = false;
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
    // No subscriptions yet (roadmap A2): the page looks at the session's journal length every two
    // seconds while it is visible, so an edit an agent or stdin makes shows up here too.
    setInterval(() => this.poll(), 2000);
    this.properties.refresh();
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
      await this.useSession(this.session, true);
    } catch {
      /* reported */
    }
  }

  async useSession(session, force = false) {
    if (!force && session === this.session) return;
    this.session = session;
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

  async poll() {
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

  select(id) {
    if (id === this.selection) return;
    this.selection = id;
    this.dispatchEvent(new Event("selection"));
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
      await this.changed();
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
      await this.changed();
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
  }
}

const app = new App();
window.engineShell = app; // for a person at the browser's console; nothing reads it
app.start();
