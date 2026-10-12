// The page's two layouts (docs/subsystems/engine_shell.md, "The narrow layout"). The three columns
// want about 950 CSS pixels — the outliner and journal 280, the viewport at least 320, the
// properties 340, two 5 px splits — and a page narrower than that used to overflow: in a browser
// the right column went under the window's edge, and in engine-editor's window at 150 % (a 1280
// pixel window is 853 CSS pixels) the native viewport, placed over a cell that ran past the client
// area, was cut off with the properties. So below `NARROW_BELOW` the page is two columns: the
// outliner over the properties on the left, the journal a drawer under them (its head only, until
// it is opened), and the viewport over the command line on the right. The viewport cell stays one
// rectangle in both, which is all the editor needs to place its child window over it.
//
// A `ResizeObserver` on the page decides; CSS (`.narrow` on the root) does the rest, except moving
// the properties panel between the columns, which no stylesheet can do. The decision has no
// hysteresis: the two layouts' widths do not overlap, so a resize across the line flips once.
import { $ } from "./dom.js";

// CSS pixels of the page's width below which the narrow layout is on.
export const NARROW_BELOW = 960;

export class Layout {
  constructor(onChange) {
    this.onChange = onChange;
    this.narrow = null;
    this.app = $("app");
    this.left = $("col-left");
    this.right = $("col-right");
    this.properties = $("panel-properties");
    this.journal = $("panel-journal");
    // The drawer opens and closes from its head, and only in the narrow layout.
    this.journal.querySelector(".panel-head").addEventListener("click", () => {
      if (!this.narrow) return;
      this.journal.classList.toggle("open");
      this.onChange(this.narrow);
    });
    new ResizeObserver(() => this.decide()).observe(this.app);
    this.decide();
  }

  decide() {
    const narrow = this.app.getBoundingClientRect().width < NARROW_BELOW;
    if (narrow === this.narrow) return;
    this.narrow = narrow;
    document.documentElement.classList.toggle("narrow", narrow);
    if (narrow) {
      this.left.insertBefore(this.properties, this.journal);
    } else {
      this.right.append(this.properties);
      this.journal.classList.remove("open");
    }
    this.onChange(narrow);
  }
}
