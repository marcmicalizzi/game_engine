// JSON-RPC 2.0 over the host's WebSocket (docs/subsystems/protocol.md, "Transport"): one request
// per text message, one response per message, matched by id. The connection is re-opened when it
// closes, with a growing pause, and every listener hears the state change so the page can say so.
// A request made while the socket is down waits for it to come back rather than failing at once;
// one that was in flight when it went down is rejected, since the host may or may not have run it.

export class RpcError extends Error {
  constructor(error) {
    super(error && error.message ? error.message : String(error));
    this.code = error && typeof error.code === "number" ? error.code : 0;
    this.data = error ? error.data : undefined;
  }
}

export class Rpc extends EventTarget {
  constructor(url) {
    super();
    this.url = url;
    this.socket = null;
    this.nextId = 1;
    this.pending = new Map();
    this.waiting = [];
    this.state = "connecting";
    this.retryMs = 400;
    this.closeReason = "";
    this.stopped = false;
  }

  start() {
    this.open();
  }

  setState(state, detail = "") {
    this.state = state;
    this.dispatchEvent(new CustomEvent("state", { detail: { state, detail } }));
  }

  open() {
    let socket;
    try {
      socket = new WebSocket(this.url);
    } catch (e) {
      this.setState("reconnecting", String(e));
      this.retry();
      return;
    }
    this.socket = socket;
    socket.addEventListener("open", () => {
      this.retryMs = 400;
      this.setState("open");
      const waiting = this.waiting;
      this.waiting = [];
      for (const send of waiting) send();
    });
    socket.addEventListener("message", (event) => this.receive(event.data));
    socket.addEventListener("close", (event) => {
      this.socket = null;
      for (const [, entry] of this.pending) {
        entry.reject(new RpcError({ code: -32000, message: "the connection closed before the answer" }));
      }
      this.pending.clear();
      // 1008 is the host refusing this page (no token, a wrong one, another origin): retrying
      // with the same URL would be refused again, so the page says why and stops.
      if (event.code === 1008) {
        this.stopped = true;
        this.setState("refused", event.reason || "refused by the host (1008)");
        return;
      }
      this.closeReason = event.reason || `closed (${event.code})`;
      this.setState("reconnecting", this.closeReason);
      this.retry();
    });
  }

  retry() {
    if (this.stopped) return;
    const delay = this.retryMs;
    this.retryMs = Math.min(this.retryMs * 1.7, 5000);
    setTimeout(() => this.open(), delay);
  }

  receive(text) {
    let message;
    try {
      message = JSON.parse(text);
    } catch {
      return;
    }
    const answers = Array.isArray(message) ? message : [message];
    for (const answer of answers) {
      const entry = this.pending.get(answer.id);
      if (!entry) continue;
      this.pending.delete(answer.id);
      if (answer.error) entry.reject(new RpcError(answer.error));
      else entry.resolve(answer.result);
    }
  }

  // Calls `method` with `params` and resolves with its result, or rejects with an RpcError.
  call(method, params) {
    return this.callRaw({ jsonrpc: "2.0", method, params: params === undefined ? null : params });
  }

  // A JSON-RPC notification: no id, so the host sends nothing back. Dropped, not queued, while the
  // socket is down: a notification carries the page's current state (editor.layout), and the page
  // sends it again when the connection opens.
  notify(method, params) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return false;
    this.socket.send(JSON.stringify({ jsonrpc: "2.0", method, params: params === undefined ? null : params }));
    return true;
  }

  // Sends a request object as given (the command line's), with an id of this client's own.
  callRaw(request) {
    return new Promise((resolve, reject) => {
      const id = this.nextId++;
      const body = JSON.stringify({ ...request, jsonrpc: "2.0", id });
      const send = () => {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
          this.waiting.push(send);
          return;
        }
        this.pending.set(id, { resolve, reject });
        this.socket.send(body);
      };
      if (this.stopped) {
        reject(new RpcError({ code: -32000, message: "the host refused this page" }));
        return;
      }
      send();
    });
  }
}
