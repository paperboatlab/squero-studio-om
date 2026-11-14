# Squero Studio: OpenModelica

Squero Studio OM (`squero studio [ OM ]`) is an acausal Modelica editor in Paperboat's flat-mono
look: a library tree, a canvas of components and connectors, a split view of `.mo` and the
generated C++, and a dock that checks the model's balance and exports a Paperboat backend
(Download `.ppk`, or Start it as `ssom_<model>` on a free port and Stop it). Both generate into
`<tmp>/ssom_<model>` and run `paperboat pack`/`start` there: the sibling `../paperboat/bin/paperboat`,
else `$PAPERBOAT_BIN` or `paperboat` on PATH. The first export of a model builds the engine (minutes).
In the backend, the model's parameters (e.g. a `parameter Real C1F`) are props of its Station
object, edited on the Station page; the Flow block carries only the inputs and outputs.
Simulating a `Modelica.Mechanics.MultiBody` model also fills the stage's **3d** view (next to
**diagram**) with its shapes, played back against the run's time (try `bin/squero-om examples/multibody/DoublePendulum.mo`). Every edit goes through `omc`, so the `.mo` stays OMEdit-compatible.

The **Model** menu runs `checkModel` and shows its messages as cards in the Console's **messages** tab;
**Show Flat Model** opens `instantiateModel` beside the generated C. The stage's **text** view shows the
model's `.mo`, and the Console's **console** tab streams omc's own log as it works.

The left rail switches between **files** — the model's folder as packages and classes, `package.order`
included — and **library**, the MSL browser. It opens on **files**. Opening a `.mo` adds a tab above the
stage; a model outside a package needs no package at all, and a class inside a package is opened with its
package loaded first.

Started without a model, the Studio opens its own folder, `~/Documents/paperboat/squero/om` (created if
missing; `SQUERO_OM_WORKSPACE` overrides it), as Squero MX opens `~/Documents/paperboat/squero-studio/mx`:
the first `.mo` in it, or a new `Untitled.mo` there. It never writes into the directory it was launched
from. The rail's **+** (or File ▸ New
Class…) opens the New Class sheet: name, kind, package, an optional base class and a live skeleton. It
writes the `.mo` and appends `package.order` when the class lands in a package.

This repo contains both the React web application (`src/`) and the native C++ server & tools (`server/`).
The server provides `squero/0`, the `omc` bridge, the `.mo` importer (`extends` resolver, `Icon(graphics)` and
`iconTransformation` quantisers), the backend generator, and the `squero-om` binary.

## Layout

Clone side by side:

```
paperboat/           engine, paperboat CLI, apps/web
paperboat-shared/    @paperboat/shared, the UI kit
squero-studio-om/    this repo (server/ and src/)
```

## Run

From the `paperboat` CLI:

```sh
paperboat start squero-studio-om [Model.mo]  # http://localhost:59201 (SQR01; the next free one up to 59299 if taken)
paperboat stop squero-studio-om
```

From a checkout:

```sh
(cd ../paperboat-shared && npm install)
npm install
npm run build                       # builds dist/
npm run build:server                # builds server/build/squero-om
bin/squero-om [Model.mo]            # server + app on http://localhost:59201 (SQR01; the next free one up to 59299 if taken)
                                    # no Model.mo: ~/Documents/paperboat/squero/om (SQUERO_OM_WORKSPACE)
```

For the UI alone: `npm run dev` (Vite on :3048; start `bin/squero-om` for the server).

The text editor's settings and themes live in the user config file `squero.toml`, shared with
Squero Studio MX: `~/.config/paperboat/` (`$XDG_CONFIG_HOME`) on Linux and macOS,
`%APPDATA%\paperboat\` on Windows. The first run writes it with every key commented; the server's
`config` and `config_write` ops read and write it, and `@paperboat/shared/editor` parses it.

## omc

Studio finds `omc` itself (`squero-om doctor` says which and checks the MSL loads):
`$SQUERO_OMC_BIN`, else `omc` on PATH if its install can build FMUs, else podman (or docker)
running `openmodelica/openmodelica:v1.27.1-minimal`, pulled on first use (`squero-om update-omc`
pulls it again). The container runs as you (rootless podman `--userns=keep-id`, docker `--user`)
with your `/tmp` and `$HOME` at the same paths and no SELinux relabelling, so everything it writes
(backends, FMUs, `~/.openmodelica/libraries`) is yours. `SQUERO_OMC=native|podman|docker` forces one.

**Windows** runs omc natively only: `$SQUERO_OMC_BIN`, else `omc` on PATH, else
`%OPENMODELICAHOME%\bin\omc.exe`, which the OpenModelica installer sets
(`winget install OpenModelica.OpenModelica.Official`). Build the server from a Developer
PowerShell for VS (`npm run build:server`); `paperboat start squero-studio-om` runs `bin/squero-om`
through Git for Windows' bash.

## Check

```sh
npm run typecheck
npm test            # node tests against a fake StudioApi; no omc, no browser
npm run test:server # C++ Catch2 tests ([modelica],[squero]; needs omc)
npm run build
```

`test/e2e/*.mjs` drive a live server in headless Chromium (playwright-core; see `lib.mjs`);
`test/e2e/reset.sh` starts a fresh one. They are manual, not CI.

## Layout of the source

`src/omc` squero/0 client · `src/store` the `StudioApi` (omc is the only source of truth for the
model) · `src/library`, `canvas`, `split`, `simulate`, `shell` · `src/popover`, `sheet`, `dock` ·
`src/model/types.ts` the seam between the two halves.
