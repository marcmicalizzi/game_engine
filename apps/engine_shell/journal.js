// The journal: the session's committed transactions from `doc.journal`, newest first, each with
// its rationale and attribution (actor, role, task) and the layer it edited. Entries past the undo
// position are shown faded, since a redo would apply them again; the title bar's Undo and Redo go
// through `doc.undo` and `doc.redo`, which the host checks against the role like any write.
import { $, clear, h, relativeTime } from "./dom.js";

const PAGE = 200;

export class Journal {
  constructor(app) {
    this.app = app;
    this.el = $("journal");
    this.meta = $("journal-meta");
    this.total = 0;
    this.position = 0;
  }

  async refresh() {
    if (!this.app.session) return;
    // The newest page: the total first, then the tail.
    const head = await this.app.call("doc.journal", { session: this.app.session, offset: 0, limit: 1 });
    const offset = Math.max(0, head.total - PAGE);
    const page = offset === 0 && head.total <= 1
      ? head
      : await this.app.call("doc.journal", { session: this.app.session, offset, limit: PAGE });
    this.total = page.total;
    this.position = page.position;
    clear(this.el);
    const patches = page.patches || [];
    for (let i = patches.length - 1; i >= 0; --i) {
      const index = offset + i;
      this.el.append(this.entry(patches[i], index));
    }
    if (patches.length === 0) {
      this.el.append(h("div", { class: "empty-note" }, "No transactions yet. An edit in Properties, or a doc.apply on the command line, lands here with its rationale."));
    }
    this.meta.textContent = this.total
      ? `${this.position} of ${this.total}${this.position < this.total ? " · redo available" : ""}`
      : "";
    this.app.setUndoRedo(this.position > 0, this.position < this.total);
  }

  entry(patch, index) {
    const a = patch.attribution || {};
    const who = [a.actor, a.role, a.task].filter((x) => x);
    const commands = (patch.forward || []).length;
    const undone = index >= this.position;
    return h("div", {
      class: "entry" + (undone ? " undone" : "") + (index === this.position - 1 ? " position" : ""),
      title: (patch.forward || []).map((c) => `${c.kind} ${c.name || c.type || ""} ${c.id || ""}`).join("\n"),
    },
      h("div", { class: "entry-line" },
        h("span", { class: "entry-index" }, `#${index}`),
        h("span", { class: "entry-rationale" }, a.rationale || "(no rationale)"),
        h("span", { class: "entry-time", title: a.timestamp_unix_ms ? new Date(a.timestamp_unix_ms).toLocaleString() : "" },
          relativeTime(a.timestamp_unix_ms))),
      h("div", { class: "entry-who" },
        who.length ? who.join(" · ") : "unattributed",
        h("span", { class: "sep" }, "—"),
        `${commands} command${commands === 1 ? "" : "s"} on ${patch.layer || "the edit layer"}`,
        undone ? h("span", { class: "sep" }, "· undone") : null));
  }
}
