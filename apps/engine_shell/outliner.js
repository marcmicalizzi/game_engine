// The outliner: every live object of the session, from `doc.objects`, as a tree — parents before
// children, roots and siblings in the order the host lists them (by id, which is the document's
// own order). Each row shows the type's short name, the record's `name` property when it has one,
// and the id's tail. Click selects; the arrow keys move the selection; a filter narrows the rows.
import { $, clear, h } from "./dom.js";

const NULL_ID = "00000000000000000000000000000000";

export class Outliner {
  constructor(app) {
    this.app = app;
    this.el = $("outliner");
    this.filter = $("outliner-filter");
    this.count = $("outliner-count");
    this.objects = [];
    this.byId = new Map();
    this.open = new Set();
    this.rows = [];
    this.filter.addEventListener("input", () => this.render());
    this.el.addEventListener("keydown", (e) => this.key(e));
    app.addEventListener("selection", () => this.markSelection());
  }

  async refresh() {
    if (!this.app.session) return;
    const all = [];
    let offset = 0;
    // Paged, as the method is: a document with more objects than one page still lists whole.
    for (;;) {
      const page = await this.app.call("doc.objects", { session: this.app.session, offset, limit: 1000 });
      all.push(...page.objects);
      offset += page.objects.length;
      if (page.objects.length === 0 || offset >= page.total) break;
    }
    this.objects = all;
    this.byId = new Map(all.map((o) => [o.id, o]));
    this.render();
  }

  label(o) {
    const name = o.properties && typeof o.properties.name === "string" ? o.properties.name : "";
    return name || o.id.slice(-8);
  }

  render() {
    clear(this.el);
    this.rows = [];
    const query = this.filter.value.trim().toLowerCase();
    const children = new Map();
    for (const o of this.objects) {
      const parent = o.parent && o.parent !== NULL_ID && this.byId.has(o.parent) ? o.parent : NULL_ID;
      if (!children.has(parent)) children.set(parent, []);
      children.get(parent).push(o);
    }
    const matches = (o) =>
      !query || this.label(o).toLowerCase().includes(query) || o.type.toLowerCase().includes(query) ||
      o.id.includes(query);
    // With a filter, a row shows when it or a descendant matches, and every match's ancestors open.
    const visible = new Set();
    const mark = (o) => {
      let any = matches(o);
      for (const c of children.get(o.id) || []) any = mark(c) || any;
      if (any) visible.add(o.id);
      return any;
    };
    for (const root of children.get(NULL_ID) || []) mark(root);

    const walk = (o, depth) => {
      if (!visible.has(o.id)) return;
      const kids = children.get(o.id) || [];
      const isOpen = query ? true : this.open.has(o.id);
      const shortType = o.type.slice(o.type.lastIndexOf(".") + 1);
      const row = h(
        "div",
        {
          class: "tree-row" + (isOpen && kids.length ? " open" : ""),
          role: "treeitem",
          "aria-selected": o.id === this.app.selection ? "true" : "false",
          style: { "--depth": String(depth) },
          dataset: { id: o.id },
          title: `${o.type}\n${o.id}\nlayer ${o.defining_layer}`,
          onclick: () => this.app.select(o.id),
        },
        h("span", {
          class: "tree-twist" + (kids.length ? "" : " leaf"),
          onclick: (e) => {
            e.stopPropagation();
            if (this.open.has(o.id)) this.open.delete(o.id);
            else this.open.add(o.id);
            this.render();
          },
        }),
        h("span", { class: "tree-type" }, shortType),
        h("span", { class: "tree-name" }, this.label(o)),
        h("span", { class: "tree-id" }, o.id.slice(-6)),
      );
      this.rows.push(row);
      this.el.append(row);
      if (isOpen) for (const k of kids) walk(k, depth + 1);
    };
    for (const root of children.get(NULL_ID) || []) walk(root, 0);
    if (this.objects.length === 0) {
      this.el.append(h("div", { class: "empty-note" }, "The document holds no objects yet."));
    }
    this.count.textContent = this.objects.length ? `${this.objects.length}` : "";
    this.markSelection();
  }

  markSelection() {
    for (const row of this.rows) {
      const selected = row.dataset.id === this.app.selection;
      row.classList.toggle("selected", selected);
      row.setAttribute("aria-selected", selected ? "true" : "false");
      if (selected) row.scrollIntoView({ block: "nearest" });
    }
  }

  key(e) {
    if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
    e.preventDefault();
    const i = this.rows.findIndex((r) => r.dataset.id === this.app.selection);
    const next = e.key === "ArrowDown" ? Math.min(this.rows.length - 1, i + 1) : Math.max(0, i - 1);
    if (this.rows[next]) this.app.select(this.rows[next].dataset.id);
  }

  name(id) {
    const o = this.byId.get(id);
    return o ? this.label(o) : id ? id.slice(-8) : "";
  }
}
