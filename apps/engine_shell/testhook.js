// The embedded page's test hook (docs/subsystems/engine_shell.md, "The test hook"). Loaded only when
// the page's address carries both `embedded=1` and `test=1`, which only `engine-editor
// --test-page` puts there; a browser, and the editor without the flag, never load this file. It
// checks from inside the page what only the page can see, and reports each check to the editor as
// an `editor.test_report` notification (a method only `--test-page` registers), which the editor
// puts in its summary for the window test to read:
//
// - "completions": the command line's completion list, opened by typing into its input, stays
//   inside the command line's panel and off the viewport cell (it opens upward, over the output).
// - "enum": the properties panel's list for an enum field, opened by a click on its button after
//   the record was chosen by a click in the outliner, stays inside the properties panel and off
//   the cell too.
// - "reconnect": after the socket dropped, the pill said "reconnecting" while it was down and
//   "connected" once the page was back (sent on every reconnect).
// - "done": the popup checks have run.
//
// The clicks and the typing are DOM events the page dispatches to itself: what is tested is the
// page's layout of its own popups, which is the page's; the platform's input is the editor's own
// test (`--test-keys`).
import { $ } from "./dom.js";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function until(look, ms) {
  const start = performance.now();
  while (performance.now() - start < ms) {
    const found = look();
    if (found) return found;
    await sleep(50);
  }
  return null;
}

function box(el) {
  const r = el.getBoundingClientRect();
  return { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height) };
}

// Whether `a` lies within `b` (a pixel of rounding allowed).
function inside(a, b) {
  return a.x >= b.x - 1 && a.y >= b.y - 1 && a.x + a.w <= b.x + b.w + 1 && a.y + a.h <= b.y + b.h + 1;
}

function overlaps(a, b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

export function install(app) {
  const report = (name, ok, detail) =>
    app.rpc.notify("editor.test_report", { name, ok: Boolean(ok), detail: JSON.stringify(detail) });
  window.__test = { report };

  let down = "";
  app.rpc.addEventListener("state", (e) => {
    const state = e.detail.state;
    if (state === "reconnecting" && !down) down = $("conn-pill").textContent;
    if (state === "open" && down) {
      const after = $("conn-pill").textContent;
      // The "Reconnected" toast is a popup too: it must not reach the cell either.
      const toast = document.querySelector(".toast");
      const cell = box($("viewport"));
      const toastBox = toast ? box(toast) : null;
      const clear = !toastBox || !overlaps(toastBox, cell);
      report("reconnect", down === "reconnecting" && after === "connected" && clear,
        { while_down: down, after, toast: toastBox, cell });
      down = "";
    }
  });

  (async () => {
    const ready = await until(() => app.rpc.state === "open" && app.started && document.querySelector(".tree-row"), 30000);
    if (!ready) {
      report("done", false, "the page never connected and listed the document");
      return;
    }
    await sleep(300); // the layout settles

    // The command line's completions.
    const input = $("console-input");
    const list = $("console-completions");
    input.focus();
    input.value = "doc.";
    input.dispatchEvent(new Event("input"));
    if (await until(() => !list.hidden && list.children.length > 0, 5000)) {
      const popup = box(list);
      const cell = box($("viewport"));
      const panel = box($("panel-console"));
      report("completions", popup.h > 0 && !overlaps(popup, cell) && inside(popup, panel),
        { popup, cell, panel, items: list.children.length });
    } else {
      report("completions", false, "the completion list did not open");
    }
    input.value = "";
    input.dispatchEvent(new Event("input"));
    input.blur();

    // The properties panel's enum list, for a record chosen by a click in the outliner.
    const row = [...document.querySelectorAll(".tree-row")].find(
      (r) => r.querySelector(".tree-type") && r.querySelector(".tree-type").textContent === "AssetProvenance");
    const button = row ? (row.click(), await until(() => document.querySelector("#properties .combo-button"), 10000)) : null;
    if (!button) {
      report("enum", false, row ? "no enum list in the properties" : "no AssetProvenance in the outliner");
    } else {
      button.scrollIntoView({ block: "nearest" }); // as a person would see it before clicking
      await sleep(50);
      button.click();
      const options = button.parentElement.querySelector(".combo-list");
      if (await until(() => !options.hidden, 3000)) {
        const popup = box(options);
        const cell = box($("viewport"));
        const panel = box($("panel-properties"));
        report("enum", popup.h > 0 && !overlaps(popup, cell) && inside(popup, panel),
          { popup, cell, panel, items: options.children.length });
      } else {
        report("enum", false, "the enum list did not open");
      }
      button.blur();
    }
    report("done", true, { narrow: Boolean(app.layout && app.layout.narrow) });
  })();
}
