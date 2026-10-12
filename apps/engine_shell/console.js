// The command line: one JSON-RPC request typed by hand — `method {params}`, or a whole request
// object — sent over the same socket, the answer shown with its time. The completions are the
// engine's own method catalogue (`engine.methods`), with each method's documentation and whether
// it is a write; Tab or Enter on a completion takes it. Up and Down walk the history, which the
// browser remembers. A write sent here refreshes the panels like any other.
import { $, clear, fitPopup, h, prettyJson, store } from "./dom.js";

export class Console {
  constructor(app) {
    this.app = app;
    this.log = $("console-log");
    this.input = $("console-input");
    this.list = $("console-completions");
    this.methods = [];
    this.matches = [];
    this.active = -1;
    this.history = store.get("history", []);
    this.cursor = this.history.length;
    this.input.addEventListener("input", () => this.complete());
    this.input.addEventListener("keydown", (e) => this.key(e));
    this.input.addEventListener("blur", () => setTimeout(() => this.hide(), 120));
    $("btn-console-clear").addEventListener("click", () => clear(this.log));
  }

  async load() {
    const result = await this.app.call("engine.methods", null);
    this.methods = (result.methods || []).slice().sort((a, b) => a.name.localeCompare(b.name));
    if (this.app.session) this.input.placeholder = `doc.objects {"session":"${this.app.session}"}`;
  }

  complete() {
    const text = this.input.value;
    const word = text.trimStart();
    if (!word || /\s/.test(word) || word.startsWith("{")) {
      this.hide();
      return;
    }
    const q = word.toLowerCase();
    this.matches = this.methods.filter((m) => m.name.toLowerCase().includes(q)).slice(0, 40);
    this.matches.sort((a, b) => (b.name.startsWith(q) ? 1 : 0) - (a.name.startsWith(q) ? 1 : 0));
    if (this.matches.length === 0) {
      this.hide();
      return;
    }
    this.active = 0;
    clear(this.list);
    this.matches.forEach((m, i) => {
      const at = m.name.toLowerCase().indexOf(q);
      const name = h("span", { class: "completion-name" },
        m.name.slice(0, at), h("mark", null, m.name.slice(at, at + q.length)), m.name.slice(at + q.length),
        m.read_only ? null : h("span", { class: "completion-badge", title: "Changes the document or the host: a role must allow it" }, "write"));
      this.list.append(h("li", {
        class: "completion" + (i === 0 ? " active" : ""),
        role: "option",
        onmousedown: (e) => {
          e.preventDefault();
          this.take(i);
        },
      }, name, h("span", { class: "completion-doc", title: m.doc }, m.doc)));
    });
    this.list.hidden = false;
    // Up from the input, never past the top of the command line's own panel.
    fitPopup(this.list, this.input, $("panel-console"));
  }

  hide() {
    this.list.hidden = true;
    this.matches = [];
    this.active = -1;
  }

  take(i) {
    const m = this.matches[i];
    if (!m) return;
    const needsSession = this.app.session && /^(doc|session|lease|render)\./.test(m.name) && m.params_type;
    this.input.value = m.params_type
      ? `${m.name} ${needsSession && m.name !== "render.capture" ? `{"session":"${this.app.session}"}` : "{}"}`
      : m.name;
    this.hide();
    this.input.focus();
    // Put the caret inside the braces, where the parameters go.
    const close = this.input.value.lastIndexOf("}");
    if (close > 0) this.input.setSelectionRange(close, close);
  }

  key(e) {
    if (!this.list.hidden && this.matches.length) {
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        this.active = (this.active + (e.key === "ArrowDown" ? 1 : -1) + this.matches.length) % this.matches.length;
        [...this.list.children].forEach((li, i) => li.classList.toggle("active", i === this.active));
        this.list.children[this.active].scrollIntoView({ block: "nearest" });
        return;
      }
      if (e.key === "Tab" || e.key === "Enter") {
        e.preventDefault();
        this.take(this.active);
        return;
      }
      if (e.key === "Escape") {
        this.hide();
        return;
      }
    }
    if (e.key === "Enter") {
      e.preventDefault();
      this.send();
    } else if (e.key === "ArrowUp" && this.history.length) {
      e.preventDefault();
      this.cursor = Math.max(0, this.cursor - 1);
      this.input.value = this.history[this.cursor] || "";
    } else if (e.key === "ArrowDown" && this.history.length) {
      e.preventDefault();
      this.cursor = Math.min(this.history.length, this.cursor + 1);
      this.input.value = this.history[this.cursor] || "";
    }
  }

  parse(text) {
    if (text.startsWith("{")) {
      const request = JSON.parse(text);
      if (typeof request.method !== "string") throw new Error("a request object needs a method");
      return request;
    }
    const space = text.search(/\s/);
    const method = space < 0 ? text : text.slice(0, space);
    const rest = space < 0 ? "" : text.slice(space).trim();
    return { method, params: rest ? JSON.parse(rest) : null };
  }

  async send() {
    const text = this.input.value.trim();
    if (!text) return;
    let request;
    try {
      request = this.parse(text);
    } catch (err) {
      this.entry(text, { error: { message: `not JSON: ${err.message}` } }, 0);
      return;
    }
    if (this.history[this.history.length - 1] !== text) this.history.push(text);
    this.history = this.history.slice(-100);
    this.cursor = this.history.length;
    store.set("history", this.history);
    this.input.value = "";
    const started = performance.now();
    let answer;
    try {
      answer = { result: await this.app.rpc.callRaw(request) };
    } catch (err) {
      answer = { error: { code: err.code, message: err.message, data: err.data } };
      this.app.reportError(err);
    }
    this.entry(text, answer, performance.now() - started);
    const method = this.methods.find((m) => m.name === request.method);
    // A session opened here becomes the page's session; any other write refreshes the panels.
    if (!answer.error && request.method === "session.open" && answer.result && answer.result.session) {
      this.app.useSession(answer.result.session);
    } else if (!answer.error && method && !method.read_only) {
      this.app.changed();
    }
  }

  entry(text, answer, ms) {
    const isError = Boolean(answer.error);
    const block = h("div", { class: "log-entry" },
      h("div", { class: "log-request" }, text, h("span", { class: "log-meta" }, ms ? `  ${ms.toFixed(0)} ms` : "")),
      h("pre", { class: "log-response" + (isError ? " error" : "") }, prettyJson(isError ? answer.error : answer.result)));
    this.log.append(block);
    this.log.scrollTop = this.log.scrollHeight;
  }
}
