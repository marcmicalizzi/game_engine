# engine_shell: the browser shell over the protocol (apps)

**Purpose.** A spike of the authoring editor's other candidate (roadmap A36, [06 §6.13](../plan/06-agent-tooling.md#613-human-developer-tooling)): a page that **speaks nothing but the engine protocol over a socket**. engine-editor's panels are Dear ImGui drawn by the renderer ([ADR-0054](../adr/0054-the-tools-ui-is-dear-imgui-drawn-by-the-renderer.md), proposed) and work, but do not have the feel the owner wants; Qt he knows and finds utilitarian. This is the third option made concrete enough to judge by using it: static HTML, one stylesheet and a handful of ES modules in `apps/engine_shell/`, served by `engine-host --listen` itself ([apps](apps.md#engine-host---listen-the-protocol-over-a-loopback-websocket)) and opened in a browser tab. The same files can later run in an OS webview inside our own window, or in an embedded Chromium if the game UI ever wants one; in every form, no panel has a private path into the engine.

**The rule: it holds no engine knowledge beyond the protocol.** Every string it shows about the world came out of a method — the outliner from `doc.objects`, the fields, their kinds, their documentation and defaults from `schema.describe`, the journal from `doc.journal`, the identity from `engine.roles`, the completions from `engine.methods`, the picture from `render.capture`. It never reads a document file, never names a record type (the only types it knows are the protocol's own field kinds, `f32` or `worldpos`), and every change it makes is a request a script could make. A feature it needs that the protocol lacks is a protocol change, made in the host where every client gets it — which is how `render.load {session}`, `render.capture`'s `keep`, `records` and A32's `camera` came to exist ([apps](apps.md#engine-host---listen-the-protocol-over-a-loopback-websocket)).

**Open it.** One host command; the browser is the user's:

```
build/msvc-debug/bin/engine-host --listen 127.0.0.1:0 --doc <dir> [--create] --actor <name>
```

The first line it prints is `{"listen":"ws://127.0.0.1:<port>/","session":"s1","shell":"http://127.0.0.1:<port>/?token=...&session=s1","token":"..."}`; open the `shell` URL. The token in it is what the listener checks on the socket and on capture fetches ([protocol](protocol.md#transport)); a page without it says so and does not retry, since the host would refuse it again. The host ends when its standard input closes (Ctrl+C in its terminal).

## What it is

- **Viewport.** `render.load {session}` once, then `render.capture` at the panel's size with `channels: ["color", "ids"]` and `keep: true`, the PNG fetched from `GET /capture/<id>`. It is a **slideshow, and says so on screen**: every gesture ends in one full-size capture, and while a gesture lasts the page asks for half-size captures back to back (never more than one in flight) so motion can be followed. engine-view's fly controls on the keyboard (W A S D, Q E down and up, Shift to hurry), a drag to look, the wheel to dolly, F to have the host frame the scene again, R to capture again. The camera is the host's: every capture reports the one it drew with (A32), the page flies from that report in engine-view's convention and sends the camera back, and the corner shows it with the capture's size, time and id. A click reads the kept id buffer at that pixel (`GET /capture/<id>/ids`, three `u32` a pixel) and selects `records[instance]`. After every transaction the page loads the session again — the host moves or rebuilds in place — and captures again. **The live viewport comes when the native window is a client of this same host** ([06 §6.13](../plan/06-agent-tooling.md#613-human-developer-tooling)), not from this page.
- **Outliner.** Type, name and id tail of every live object, parents before children, collapsible, filtered as you type; the arrow keys walk it.
- **Properties.** Every field of the selected record from its type's `schema.describe`, with an editor by kind: number boxes (integers checked), text, a checkbox, x/y/z(/w) axis boxes for `vec2`, `vec3`, `quat` and `worldpos`, a list for an enum the host describes, JSON for anything else, and an Unset (`ClearProperty`) for an optional field that is set. **A changed field is one `doc.apply` with one `SetProperty`** and the rationale typed in the field under the panel, which defaults to "Set <field> of <name>"; the input flashes when the host committed it and turns red when it did not, with the reason in the status strip. Properties a record holds that its type does not declare are shown too, as JSON.
- **Journal.** The session's transactions newest first, each with its rationale, actor, role, task, command count, layer and time; entries past the undo position faded. Undo and Redo in the title bar (Ctrl+Z, Ctrl+Shift+Z) are `doc.undo` and `doc.redo`, which the host checks against the role like any write.
- **Command line.** One JSON-RPC request typed by hand — `method {params}`, or a whole request object — and the answer, coloured, with its round-trip time. Completion is the engine's own catalogue from `engine.methods`, each with its documentation and a `write` badge on methods that are not read-only; taking one fills in the session. History survives a reload. A `session.open` typed here becomes the page's session; any other write refreshes the panels.
- **Status strip.** The host, the session, the actor and role the host attributes edits to, whether the page has the token, and the last error a method returned.

**When another client edits.** There are no subscriptions yet (roadmap A2), so the page reads `session.info` every two seconds while it is visible and refreshes everything when the journal length or the undo position moved: an agent's edit through engine-mcp, or a line on the host's stdin, shows up within that.

**It reconnects, and says so.** A dropped socket turns the title bar's pill amber ("reconnecting"), the page retries with a growing pause up to five seconds, and when it is back it loads the session again; a request in flight when the socket dropped fails rather than being sent twice, since the host may have run it.

## Feel: what the page spends effort on

The point of the spike is feel, so the page is designed rather than defaulted: the system's UI face at 13 px with tabular figures, monospace only where the text is data; an 8 px spacing scale with a 4 px half step; one accent on a near-black ground, borders a step lighter than the panels; a visible focus ring on everything focusable; a hover and an active state on every control; splits between the panels that highlight under the pointer, drag smoothly, move with the arrow keys and are remembered; axis labels coloured x red, y green, z blue in the vector editors; a pulse on the connection pill while it waits; and `prefers-reduced-motion` honoured. No component library, no framework, nothing generated: about 2,700 lines with the stylesheet, which a reader can follow.

**No bundler, no package manager, no CDN, nothing fetched at run time** but what the host it came from answers. The page opens offline from the host's listener, and the build only copies the files (`apps/engine_host/CMakeLists.txt`, into `build/<preset>/shell/`); an edited file is served on the next reload because every response says `Cache-Control: no-store`.

## Files

`index.html` (the layout), `shell.css` (the one stylesheet), `main.js` (the connection, the session, the selection, and "after a change, ask again"), `rpc.js` (JSON-RPC over the WebSocket, reconnecting), `dom.js` (element and storage helpers, the JSON colouring), `splits.js`, `viewport.js`, `outliner.js`, `properties.js`, `journal.js`, `console.js`.

## Testing

The shell is tested through its clients, not in a browser — no test opens a window on the machine. `apps/engine_host/tests/transport_tests.cpp` checks that the listener serves `index.html` and the stylesheet and nothing outside the directory, and every protocol path the page uses: `render.load {session}`, the kept capture and its ids over HTTP, `records`, the reported camera given back, the move in place and the rebuild. `tools/host-ws.Tests.ps1` fetches the page with .NET's HTTP client. The modules are checked to parse (`node --check`) by whoever changes them; a person opens the page to judge it.

## Not yet

- **A live viewport.** Each picture is a `render.capture`; at the panel's size on a desktop GPU that is tens of milliseconds a frame plus the PNG, which is a slideshow, not a view. The live one is the native window as a client of the host.
- **Selection in the picture.** A selected record is not outlined in the viewport (engine-editor draws its box); the shell would need the host to draw it, which is a capture option nobody has asked for yet.
- **A gizmo, multi-select, create and delete**, the review queue (A23, A24), a layer picker, leases and proposals in the UI. Each is a protocol call away; none is built.
- **Subscriptions instead of a two-second look**, and the bulk channel instead of HTTP GETs of kept captures (roadmap A2).
- **The webview window.** The same files in an OS webview of our own window is the next form; nothing in the page assumes a browser tab beyond `location`.
