// Properties: every field of the selected record, from its type's `schema.describe`, with an
// editor chosen by the field's kind — numbers, text, a checkbox, axis boxes for vectors, a list for
// an enum (described too), JSON for the rest. A changed field is **one** `doc.apply` with one
// `SetProperty` and the rationale typed below the panel, which defaults to naming the change, so
// the journal says why. Nothing here knows a type: the fields, their kinds, their documentation and
// their defaults all come from the host.
import { $, clear, fitPopup, h } from "./dom.js";

const AXES = { vec2: 2, vec3: 3, vec4: 4, dvec3: 3, worldpos: 3, quat: 4 };
const AXIS_NAMES = ["x", "y", "z", "w"];
const INTEGERS = new Set(["i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"]);
const FLOATS = new Set(["f32", "f64"]);

export class Properties {
  constructor(app) {
    this.app = app;
    this.el = $("properties");
    this.meta = $("properties-meta");
    this.rationale = $("rationale");
    this.types = new Map(); // qualified name -> description (or null when it does not describe)
    this.record = null;
    app.addEventListener("selection", () => this.refresh());
  }

  async describe(type) {
    if (this.types.has(type)) return this.types.get(type);
    let description = null;
    try {
      description = (await this.app.call("schema.describe", { type })).description;
    } catch {
      description = null;
    }
    this.types.set(type, description);
    return description;
  }

  async refresh() {
    const id = this.app.selection;
    if (!id) {
      this.record = null;
      clear(this.el);
      this.el.append(h("div", { class: "empty-note" },
        "Select a record in the outliner or click it in the viewport."));
      this.meta.textContent = "";
      this.rationale.placeholder = "Why — recorded in the journal with the change";
      return;
    }
    const record = this.app.outliner.byId.get(id);
    if (!record) {
      clear(this.el);
      this.el.append(h("div", { class: "empty-note" }, "That record is not in the document any more."));
      return;
    }
    this.record = record;
    const description = await this.describe(record.type);
    if (this.app.selection !== id) return; // the selection moved on while the type was described
    this.render(record, description);
  }

  render(record, description) {
    clear(this.el);
    const name = this.app.outliner.name(record.id);
    this.meta.textContent = record.type.slice(record.type.lastIndexOf(".") + 1);
    this.rationale.placeholder = `Default: “Set <field> of ${name}”`;
    this.el.append(
      h("div", { class: "prop-header" },
        h("div", { class: "prop-header-name" }, name),
        h("div", { class: "prop-header-sub" }, `${record.type} · ${record.id}`)),
    );
    if (description && description.doc) {
      this.el.append(h("div", { class: "prop-type-doc" }, description.doc));
    }
    const shown = new Set();
    const fields = description && Array.isArray(description.fields) ? description.fields : [];
    for (const field of fields) {
      if (field.transient) continue;
      shown.add(field.name);
      this.el.append(this.row(record, field));
    }
    // Properties the record holds that its type does not declare are shown too, as JSON: hiding
    // them would hide the problem, not the property.
    for (const [key, value] of Object.entries(record.properties || {})) {
      if (shown.has(key)) continue;
      this.el.append(this.row(record, { name: key, type: "json", doc: "Not a field of this type." }, value));
    }
    if (!description) {
      this.el.append(h("div", { class: "empty-note" }, `schema.describe knows no type ${record.type}.`));
    }
  }

  row(record, field, explicit) {
    const has = Object.prototype.hasOwnProperty.call(record.properties || {}, field.name);
    const value = explicit !== undefined ? explicit : has ? record.properties[field.name] : field.default;
    const optional = typeof field.type === "string" && field.type.endsWith("?");
    const kind = optional ? field.type.slice(0, -1) : field.type || "json";
    const editor = h("div", { class: "prop-editor" });
    const commit = (newValue, input) => this.commit(record, field.name, newValue, input);
    this.buildEditor(editor, kind, value, commit, optional && !has);
    if (optional && has) {
      editor.append(h("button", {
        class: "btn btn-ghost btn-small prop-clear",
        title: "ClearProperty: remove this layer's value",
        onclick: () => this.apply(record, [{ kind: "ClearProperty", id: record.id, name: field.name }],
                                  `Clear ${field.name} of ${this.app.outliner.name(record.id)}`),
      }, "Unset"));
    }
    return h("div", { class: "prop" },
      h("div", { class: "prop-label" },
        h("div", { class: "prop-name", title: field.name }, field.name),
        h("div", { class: "prop-kind" }, field.type + (has ? "" : optional ? " · unset" : " · default"))),
      editor,
      field.doc ? h("div", { class: "prop-doc" }, field.doc) : null);
  }

  buildEditor(editor, kind, value, commit, unset) {
    if (kind === "bool") {
      const box = h("input", { class: "check", type: "checkbox" });
      box.checked = value === true;
      box.addEventListener("change", () => commit(box.checked, box));
      editor.append(box);
      return;
    }
    if (INTEGERS.has(kind) || FLOATS.has(kind)) {
      const input = h("input", { class: "input", type: "number", step: INTEGERS.has(kind) ? "1" : "any" });
      input.value = typeof value === "number" ? String(value) : "";
      if (unset) input.placeholder = "unset";
      input.addEventListener("change", () => {
        const n = Number(input.value);
        if (input.value.trim() === "" || !Number.isFinite(n) || (INTEGERS.has(kind) && !Number.isInteger(n))) {
          input.classList.add("invalid");
          return;
        }
        input.classList.remove("invalid");
        commit(n, input);
      });
      editor.append(input);
      return;
    }
    if (AXES[kind]) {
      const count = AXES[kind];
      const values = Array.isArray(value) ? value.slice(0, count) : new Array(count).fill(0);
      if (!Array.isArray(value) && kind === "quat") values[3] = 1;
      const inputs = [];
      for (let i = 0; i < count; ++i) {
        const input = h("input", { class: "input", type: "number", step: "any", "aria-label": AXIS_NAMES[i] });
        input.value = typeof values[i] === "number" ? String(values[i]) : "";
        if (unset) input.placeholder = "unset";
        inputs.push(input);
        input.addEventListener("change", () => {
          const next = inputs.map((x) => Number(x.value));
          if (inputs.some((x) => x.value.trim() === "") || next.some((n) => !Number.isFinite(n))) {
            input.classList.add("invalid");
            return;
          }
          for (const x of inputs) x.classList.remove("invalid");
          commit(next, input);
        });
        editor.append(h("span", { class: "axis", dataset: { axis: AXIS_NAMES[i] } }, input));
      }
      // A quaternion is read in degrees beside it: yaw about y, pitch about x, roll about -z, the
      // order core/math's quat_from_euler composes (derived, never written; A39).
      if (kind === "quat") {
        const [x, y, z, w] = values.map((n) => (typeof n === "number" ? n : 0));
        const deg = 180 / Math.PI;
        const pitch = Math.asin(Math.max(-1, Math.min(1, 2 * (w * x - y * z))));
        const yaw = Math.atan2(2 * (w * y + x * z), 1 - 2 * (x * x + y * y));
        const roll = Math.atan2(2 * (w * z + x * y), 1 - 2 * (x * x + z * z));
        const f = (v) => (Math.round(v * deg * 10) / 10 + 0).toFixed(1);
        editor.append(
          h("span", { class: "quat-euler", title: "yaw, pitch, roll in degrees (derived from the quaternion)" },
            `${f(yaw)}° ${f(pitch)}° ${f(-roll)}°`),
        );
      }
      return;
    }
    if (kind === "string" || kind === "id128") {
      const input = h("input", { class: "input" + (kind === "id128" ? " mono" : ""), spellcheck: "false" });
      input.value = typeof value === "string" ? value : "";
      if (unset) input.placeholder = "unset";
      input.addEventListener("change", () => commit(input.value, input));
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter") input.blur();
      });
      editor.append(input);
      return;
    }
    // A named type the host can describe as an enum becomes a list of its values; anything else —
    // a struct, an array, a map, json — is edited as JSON.
    const area = h("textarea", { class: "input", spellcheck: "false", rows: "3" });
    area.value = value === undefined ? "" : JSON.stringify(value, null, 1);
    area.addEventListener("change", () => {
      let parsed;
      try {
        parsed = JSON.parse(area.value);
      } catch {
        area.classList.add("invalid");
        return;
      }
      area.classList.remove("invalid");
      commit(parsed, area);
    });
    editor.append(area);
    if (/^[A-Za-z_][\w.]*$/.test(kind) && kind.includes(".")) {
      this.describe(kind).then((d) => {
        if (!d || d.kind !== "enum" || !Array.isArray(d.values) || !area.isConnected) return;
        const combo = enumList(d.values, typeof value === "string" ? value : "", (name, button) =>
          commit(name, button), this.el);
        area.replaceWith(combo);
      });
    }
  }

  rationaleFor(fieldName) {
    const typed = this.rationale.value.trim();
    return typed || `Set ${fieldName} of ${this.app.outliner.name(this.record ? this.record.id : "")}`;
  }

  async commit(record, name, value, input) {
    const current = (record.properties || {})[name];
    if (JSON.stringify(current) === JSON.stringify(value)) return;
    input.classList.add("saving");
    const ok = await this.apply(record, [{ kind: "SetProperty", id: record.id, name, value }],
                                this.rationaleFor(name));
    input.classList.remove("saving");
    if (ok) {
      input.classList.remove("saved");
      void input.offsetWidth; // restart the animation
      input.classList.add("saved");
    } else {
      input.classList.add("invalid");
    }
  }

  async apply(record, commands, rationale) {
    const ok = await this.app.apply(commands, rationale);
    if (ok) this.rationale.value = "";
    return ok;
  }
}

// An enum's values as a list the page draws itself, not a native <select>: a native list is the
// platform's own popup, which a test cannot open or measure, and the page's own can be kept inside
// the panel (`fitPopup`) and proven to stay out of engine-editor's viewport cell (engine_shell.md,
// "Popups stay in their panels"). A button shows the value; a click, Enter, Space or Alt+Down opens
// the list; the arrow keys walk it, Enter or a click takes a value, Escape or a click elsewhere
// closes it.
function enumList(values, current, onPick, panel) {
  const button = h("button", { class: "input combo-button", type: "button", "aria-haspopup": "listbox" },
    current || "unset");
  const list = h("ul", { class: "combo-list", role: "listbox", hidden: true });
  const wrap = h("div", { class: "combo" }, button, list);
  let active = -1;
  const items = values.map((v, i) => h("li", {
    class: "combo-item" + (v.name === current ? " selected" : ""),
    role: "option",
    title: v.doc || "",
    onmousedown: (e) => {
      e.preventDefault();
      take(i);
    },
  }, v.name));
  list.append(...items);
  const mark = (i) => {
    active = i;
    items.forEach((li, k) => li.classList.toggle("active", k === active));
    // The list's own scroll only: scrollIntoView would scroll the panel under a fixed list too.
    const li = items[active];
    if (!li) return;
    if (li.offsetTop < list.scrollTop) list.scrollTop = li.offsetTop;
    else if (li.offsetTop + li.offsetHeight > list.scrollTop + list.clientHeight)
      list.scrollTop = li.offsetTop + li.offsetHeight - list.clientHeight;
  };
  const open = () => {
    list.hidden = false;
    fitPopup(list, button, panel.closest(".panel") || panel);
    mark(Math.max(0, values.findIndex((v) => v.name === current)));
  };
  const close = () => {
    list.hidden = true;
    if (!wrap.isConnected) panel.removeEventListener("scroll", close);  // the panel was redrawn
  };
  const take = (i) => {
    close();
    if (!values[i] || values[i].name === current) return;
    current = values[i].name;
    button.textContent = current;
    items.forEach((li, k) => li.classList.toggle("selected", k === i));
    onPick(current, button);
  };
  button.addEventListener("click", () => (list.hidden ? open() : close()));
  // A fixed list stays where it opened, so it closes when the panel under it scrolls.
  panel.addEventListener("scroll", close, { passive: true });
  button.addEventListener("blur", () => setTimeout(close, 120));
  button.addEventListener("keydown", (e) => {
    if (list.hidden) {
      if (e.key === "Enter" || e.key === " " || (e.altKey && e.key === "ArrowDown")) {
        e.preventDefault();
        open();
      }
      return;
    }
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      mark((active + (e.key === "ArrowDown" ? 1 : -1) + items.length) % items.length);
    } else if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      take(active);
    } else if (e.key === "Escape") {
      e.preventDefault();
      close();
    }
  });
  return wrap;
}
