// Properties: every field of the selected record, from its type's `schema.describe`, with an
// editor chosen by the field's kind — numbers, text, a checkbox, axis boxes for vectors, a list for
// an enum (described too), JSON for the rest. A changed field is **one** `doc.apply` with one
// `SetProperty` and the rationale typed below the panel, which defaults to naming the change, so
// the journal says why. Nothing here knows a type: the fields, their kinds, their documentation and
// their defaults all come from the host.
import { $, clear, h } from "./dom.js";

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
        const select = h("select", { class: "input" },
          d.values.map((v) => h("option", { value: v.name, title: v.doc || "" }, v.name)));
        select.value = typeof value === "string" ? value : "";
        select.addEventListener("change", () => commit(select.value, select));
        area.replaceWith(select);
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
