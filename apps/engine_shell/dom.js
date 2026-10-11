// A few DOM helpers, so the panels read as what they build rather than as calls to createElement.

// h("div", {class: "x", onclick: f}, child, "text", ...)
export function h(tag, attrs, ...children) {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [key, value] of Object.entries(attrs)) {
      if (value === undefined || value === null || value === false) continue;
      if (key.startsWith("on") && typeof value === "function") {
        el.addEventListener(key.slice(2), value);
      } else if (key === "class") {
        el.className = value;
      } else if (key === "style" && typeof value === "object") {
        for (const [p, v] of Object.entries(value)) el.style.setProperty(p, v);
      } else if (key === "dataset") {
        for (const [p, v] of Object.entries(value)) el.dataset[p] = v;
      } else if (value === true) {
        el.setAttribute(key, "");
      } else {
        el.setAttribute(key, String(value));
      }
    }
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue;
    if (Array.isArray(child)) append(el, child);
    else el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

export function clear(el) {
  while (el.firstChild) el.removeChild(el.firstChild);
}

export const $ = (id) => document.getElementById(id);

// Local storage that never throws: a private window or blocked storage just forgets.
export const store = {
  get(key, fallback) {
    try {
      const v = localStorage.getItem("engine-shell." + key);
      return v === null ? fallback : JSON.parse(v);
    } catch {
      return fallback;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem("engine-shell." + key, JSON.stringify(value));
    } catch {
      /* forgotten */
    }
  },
};

// JSON with colour: keys, strings, numbers and literals as spans, for the command line's output.
export function prettyJson(value) {
  const text = JSON.stringify(value, null, 2) ?? "null";
  const out = document.createDocumentFragment();
  const re = /("(?:\\.|[^"\\])*")(\s*:)?|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b/g;
  let last = 0;
  for (const m of text.matchAll(re)) {
    if (m.index > last) out.append(text.slice(last, m.index));
    if (m[1] !== undefined) {
      out.append(h("span", { class: m[2] ? "j-key" : "j-string" }, m[1]));
      if (m[2]) out.append(m[2]);
    } else if (m[3] !== undefined) {
      out.append(h("span", { class: "j-number" }, m[3]));
    } else {
      out.append(h("span", { class: "j-literal" }, m[4]));
    }
    last = m.index + m[0].length;
  }
  if (last < text.length) out.append(text.slice(last));
  return out;
}

export function relativeTime(ms) {
  if (!ms) return "";
  const s = Math.max(0, (Date.now() - ms) / 1000);
  if (s < 45) return "just now";
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return new Date(ms).toLocaleDateString();
}

export function toast(text) {
  const el = h("div", { class: "toast", role: "status" }, text);
  document.body.append(el);
  setTimeout(() => el.remove(), 2600);
}
