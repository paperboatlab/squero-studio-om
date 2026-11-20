// Simulate (contracts §8.2): a local check, not a second Flow page. Scopes on Paperboat's board
// grid (#7), each plotting the channels picked in its dropdown, and Paperboat's floating run dock.
// omc runs it; nothing leaves Studio and no block, board or station is involved.

import {
  addDecoration,
  addTab,
  editDecoration,
  freshTabName,
  loadSaved,
  moveDecoration,
  moveInstrument,
  moveToTab,
  removeDecoration,
  removeTab,
  renameTab,
  resizeInstrument,
  rowsOf,
  saveLayout,
  setSettings,
  SIZE_LABEL,
  tabIsEmpty,
  type BoardLayout,
} from "@paperboat/shared";
import { useEffect, useLayoutEffect, useRef, useState, type CSSProperties, type DragEvent } from "react";
import { createPortal } from "react-dom";
import { Dock, NumberField, SolverSettings } from "@paperboat/shared/components";
import { Scope, type ScopeChannel, type ScopeSettings } from "@paperboat/shared/scope";
import { DEFAULTS, loadSolver, OPTIONS, saveSolver, SOLVERS, summary, type SolverChoice } from "./solver.ts";
import type { StudioModel } from "../model/types.ts";
import type { Actions, Stimulus } from "../store/index.tsx";
import { explainOmcError } from "../dock/dock.ts";
import { addScope, arrangeScopes, picksOf, removeScope, START } from "./scopes.ts";

type Run = Awaited<ReturnType<Actions["simulate"]>>;

/** The variables worth plotting first: not derivatives, fewest dots, a few of them. */
export function defaultPicks(names: string[], limit = 3): string[] {
  return [...names]
    .filter((n) => !n.startsWith("der("))
    .sort((a, b) => a.split(".").length - b.split(".").length || a.localeCompare(b))
    .slice(0, limit);
}

/** Why the model won't balance, in words: a top-level output wired to nothing has no equation. */
export function outputWarnings(model: StudioModel): string[] {
  return model.entries
    .filter((e) => e.kind === "output" && !e.target)
    .map((e) => `${e.name} isn't wired to anything: an output needs an equation, so the model is unbalanced. Wire it to a port or remove it.`);
}

const KINDS: Stimulus["kind"][] = ["constant", "step", "ramp", "sine"];
const K_LABEL: Record<Stimulus["kind"], string> = { constant: "value", step: "height", ramp: "slope", sine: "amp" };

/** One row per top-level input: what drives it in this local run (nothing is exported). */
function Stimuli({ inputs, value, onChange }: { inputs: string[]; value: Record<string, Stimulus>; onChange: (v: Record<string, Stimulus>) => void }) {
  if (!inputs.length) return null;
  return (
    <div className="stimuli" role="group" aria-label="Input stimuli">
      {inputs.map((name) => {
        const s = value[name] ?? { kind: "constant", k: 0 };
        const set = (patch: Partial<Stimulus>) => onChange({ ...value, [name]: { ...s, ...patch } });
        return (
          <div key={name} className="stimulus">
            <span className="lbl mono">{name}</span>
            <select className="well" aria-label={`Stimulus for ${name}`} value={s.kind} onChange={(e) => set({ kind: e.target.value as Stimulus["kind"], ...(s.k === 0 && e.target.value !== "constant" ? { k: 1 } : {}) })}>
              {KINDS.map((k) => (
                <option key={k}>{k}</option>
              ))}
            </select>
            <span className="lbl">{K_LABEL[s.kind]}</span>
            <NumberField className="well" aria-label={`${name} ${K_LABEL[s.kind]}`} value={s.k} onChange={(k) => set({ k })} />
            {(s.kind === "step" || s.kind === "ramp") && (
              <>
                <span className="lbl">at t</span>
                <NumberField className="well" aria-label={`${name} start time`} value={s.t0 ?? 0} onChange={(t0) => set({ t0 })} />
              </>
            )}
            {s.kind === "sine" && (
              <>
                <span className="lbl">Hz</span>
                <NumberField className="well" aria-label={`${name} frequency`} value={s.f ?? 1} onChange={(f) => set({ f })} />
              </>
            )}
          </div>
        );
      })}
    </div>
  );
}

/** The channel picker: a dropdown with a filter, each variable a checkbox in its trace colour. */
function Channels({ names, picks, onChange }: { names: string[]; picks: string[]; onChange: (picks: string[]) => void }) {
  const [filter, setFilter] = useState("");
  const shown = names.filter((n) => n.toLowerCase().includes(filter.toLowerCase()));
  return (
    <details className="channels">
      <summary className="key small" title="Pick the channels this scope plots">
        {picks.length ? `${picks.length} channel${picks.length > 1 ? "s" : ""}` : "channels"} ▾
      </summary>
      <div className="channels-pop" role="group" aria-label="Channels">
        <input className="well" placeholder="filter…" aria-label="Filter channels" value={filter} autoFocus onChange={(e) => setFilter(e.target.value)} />
        {names.length === 0 && <span className="lbl">Simulate first</span>}
        {shown.map((n) => {
          const i = picks.indexOf(n);
          return (
            <label key={n} className="channel" title={n} style={i >= 0 ? ({ "--trace": `var(--trace-${(i % 8) + 1})` } as CSSProperties) : undefined}>
              <input type="checkbox" checked={i >= 0} onChange={() => onChange(i >= 0 ? picks.filter((x) => x !== n) : [...picks, n])} />
              <span>{n}</span>
            </label>
          );
        })}
      </div>
    </details>
  );
}

const SCOPE_DRAG = "application/x-squero-scope";
const ROW = 110; // px a grid row
const GAP = 8;

type Phase = { state: "idle" } | { state: "running"; since: number; stage: string } | { state: "done"; ms: number } | { state: "failed" } | { state: "cancelled" };

export function Simulate({
  actions,
  model,
  onRunningChange,
  onRun,
}: {
  actions: Actions;
  model: StudioModel;
  onRunningChange?: (running: boolean) => void;
  onRun?: (run: Run) => void;
}) {
  const [stop, setStop] = useState("1");
  const [stimuli, setStimuli] = useState<Record<string, Stimulus>>({});
  const [solver, setSolverState] = useState<SolverChoice>(() => loadSolver(model.name));
  useEffect(() => setSolverState(loadSolver(model.name)), [model.name]);
  const setSolver = (v: SolverChoice) => (setSolverState(v), saveSolver(model.name, v));
  const [solverOpen, setSolverOpen] = useState(false);
  // Run ▸ Solver settings… opens it from the menu bar
  useEffect(() => {
    const open = () => setSolverOpen(true);
    window.addEventListener("squero:solver", open);
    return () => window.removeEventListener("squero:solver", open);
  }, []);
  const inputs = model.entries.filter((e) => e.kind === "input").map((e) => e.name);
  const warnings = outputWarnings(model);
  const [run, setRun] = useState<Run | null>(null);
  const [runVersion, setRunVersion] = useState(0);
  const [phase, setPhase] = useState<Phase>({ state: "idle" });
  const [now, setNow] = useState(0);
  const [error, setError] = useState("");
  const ticket = useRef(0);
  const key = `squero-scopes:${model.name}`;
  const [layout, setLayout] = useState<BoardLayout>(() => loadSaved(key) ?? START);
  useEffect(() => setLayout(loadSaved(key) ?? START), [key]);
  const [tab, setTab] = useState("");
  const a = arrangeScopes(layout);
  const page = a.tabs.length ? (a.tabs.includes(tab) ? tab : a.tabs[0]) : "";
  const gridRef = useRef<HTMLDivElement>(null);
  // The run dock floats at the bottom middle of the stage, as in Paperboat: portal it there.
  const [canvas, setCanvas] = useState<Element | null>(null);
  useLayoutEffect(() => setCanvas(document.querySelector(".studio .stage-view")), []);

  const edit = (next: BoardLayout) => {
    setLayout(next);
    saveLayout(key, next);
  };
  const setPicks = (block: string, picks: string[]) => edit(setSettings(a, block, { picks }));

  useEffect(() => {
    onRunningChange?.(phase.state === "running");
    return () => onRunningChange?.(false);
  }, [phase.state, onRunningChange]);

  useEffect(() => {
    if (phase.state !== "running") return;
    const timer = setInterval(() => setNow(Date.now()), 100);
    return () => clearInterval(timer);
  }, [phase.state]);

  const go = async () => {
    const t = Number(stop);
    if (!(t > 0)) return setError("stop time must be above 0");
    const mine = ++ticket.current;
    const since = Date.now();
    setNow(since);
    setPhase({ state: "running", since, stage: "starting" });
    setError("");
    const onStage = (stage: string) => mine === ticket.current && setPhase((p) => (p.state === "running" ? { ...p, stage } : p));
    try {
      const result = await actions.simulate(t, Object.fromEntries(Object.entries(stimuli).filter(([n]) => inputs.includes(n))), solver, onStage);
      if (mine !== ticket.current) return;
      setRun(result);
      setRunVersion((v) => v + 1);
      onRun?.(result);
      setPhase({ state: "done", ms: Date.now() - since });
      // a first run fills the first empty scope, so there is something to look at
      const first = a.instruments.find((p) => picksOf(p).length === 0);
      if (first && a.instruments.every((p) => picksOf(p).filter((n) => result.names.includes(n)).length === 0)) setPicks(first.block, defaultPicks(result.names));
    } catch (e) {
      if (mine !== ticket.current) return;
      setError(explainOmcError((e as Error).message));
      setPhase({ state: "failed" });
    }
  };
  /** Stop omc's compile or the model's executable; the run's reply is then dropped. */
  const cancel = () => {
    ticket.current++;
    setPhase({ state: "cancelled" });
    void actions.cancelSimulate();
  };
  // Run ▸ Simulate (Ctrl+Enter) and Run ▸ Cancel simulation reach here as events, like the solver's.
  const latest = useRef({ go, cancel, running: false });
  latest.current = { go, cancel, running: phase.state === "running" };
  useEffect(() => {
    const start = () => !latest.current.running && void latest.current.go();
    const stop = () => latest.current.running && latest.current.cancel();
    // Ctrl+C stops a simulation, as in MX, while there is nothing selected to copy.
    const key = (e: KeyboardEvent) => {
      if (!latest.current.running || !e.ctrlKey || e.shiftKey || e.altKey || e.key.toLowerCase() !== "c") return;
      if (window.getSelection()?.toString() || document.querySelector(".react-flow__node.selected")) return;
      e.preventDefault();
      latest.current.cancel();
    };
    window.addEventListener("squero:simulate", start);
    window.addEventListener("squero:cancel", stop);
    window.addEventListener("keydown", key);
    return () => {
      window.removeEventListener("squero:simulate", start);
      window.removeEventListener("squero:cancel", stop);
      window.removeEventListener("keydown", key);
    };
  }, []);

  const cell = (e: DragEvent): [number, number] => {
    const box = gridRef.current!.getBoundingClientRect();
    return [Math.floor(((e.clientX - box.left) / box.width) * a.columns), Math.floor((e.clientY - box.top) / (ROW + GAP))];
  };
  const area = (p: { col: number; row: number; w: number; h: number }): CSSProperties => ({ gridColumn: `${p.col + 1} / span ${p.w}`, gridRow: `${p.row + 1} / span ${p.h}` });
  const rows = Math.max(1, rowsOf(a, page));
  const status =
    phase.state === "running" ? `${phase.stage}… ${((now - phase.since) / 1000).toFixed(1)} s`
    : phase.state === "done" ? `completed in ${phase.ms} ms`
    : phase.state === "failed" ? "error"
    : phase.state === "cancelled" ? "cancelled"
    : "";

  return (
    <section className="simulate" aria-label="Simulate">
      <Stimuli inputs={inputs} value={stimuli} onChange={setStimuli} />
      {warnings.map((w) => (
        <p key={w} className="warning" role="status">
          {w}
        </p>
      ))}
      {error && <p className="error" role="alert">{error}</p>}
      <div className="scope-tools" role="toolbar" aria-label="Scopes">
        {a.tabs.length > 0 && (
          <div className="scope-tabs" role="tablist">
            {a.tabs.map((t) => (
              <span key={t} className={`scope-tab${t === page ? " on" : ""}`}>
                <button
                  role="tab"
                  aria-selected={t === page}
                  title="Double-click to rename"
                  onClick={() => setTab(t)}
                  onDoubleClick={() => {
                    const to = prompt("Tab name", t)?.trim();
                    if (to) edit(renameTab(a, t, to)), setTab(to);
                  }}
                >
                  {t}
                </button>
                {tabIsEmpty(a, t) && (
                  <button className="scope-x" title={`Remove ${t}`} onClick={() => edit(removeTab(a, t))}>
                    ×
                  </button>
                )}
              </span>
            ))}
          </div>
        )}
        <button className="key small" onClick={() => edit(addScope(layout, page))}>+ Scope</button>
        <button className="key small" onClick={() => edit(addDecoration(a, "label", page))}>+ Label</button>
        <button className="key small" onClick={() => edit(addDecoration(a, "hline", page))}>+ Separator</button>
        <button className="key small" onClick={() => edit(addDecoration(a, "section", page))}>+ Section</button>
        <button
          className="key small"
          onClick={() => {
            const name = freshTabName(a.tabs.length ? a.tabs : ["Main"]);
            edit(addTab(a, name));
            setTab(name);
          }}
        >
          + Tab
        </button>
      </div>
      <div
        ref={gridRef}
        className="scope-grid"
        style={{ gridTemplateColumns: `repeat(${a.columns}, minmax(0, 1fr))`, gridTemplateRows: `repeat(${rows}, ${ROW}px)`, gap: GAP }}
        onDragOver={(e) => e.dataTransfer.types.includes(SCOPE_DRAG) && e.preventDefault()}
        onDrop={(e) => {
          const what = e.dataTransfer.getData(SCOPE_DRAG);
          if (!what) return;
          e.preventDefault();
          const [col, row] = cell(e);
          edit(what.startsWith("deco:") ? moveDecoration(a, Number(what.slice(5)), col, row) : moveInstrument(a, what, col, row));
        }}
      >
        {a.decorations
          .filter((d) => d.tab === page && !d.hidden)
          .map((d) => (
            <div
              key={`d${d.index}`}
              className={`scope-deco deco-${d.kind}`}
              style={d.kind === "hline" ? { gridColumn: `${d.col + 1} / span ${d.w}`, gridRow: `${d.row + 1}` } : area(d)}
              draggable={d.kind !== "section"}
              onDragStart={(e) => e.dataTransfer.setData(SCOPE_DRAG, `deco:${d.index}`)}
            >
              {d.kind !== "hline" && (
                <input
                  className="deco-text"
                  aria-label={d.kind === "label" ? "Label" : "Section title"}
                  defaultValue={d.text}
                  key={d.text}
                  onBlur={(e) => e.target.value !== d.text && edit(editDecoration(a, d.index, { text: e.target.value }))}
                />
              )}
              <button className="scope-x" title="Remove" onClick={() => edit(removeDecoration(a, d.index))}>
                ×
              </button>
            </div>
          ))}
        {a.instruments
          .filter((p) => p.tab === page)
          .map((p) => {
            const picks = picksOf(p).filter((n) => !run || run.names.includes(n));
            const channels: ScopeChannel[] = picks.map((name) => ({
              id: name,
              label: name,
              buf: { times: run?.t ?? [], values: (run?.series[name] ?? []).map((v) => v ?? Number.NaN) },
            }));
            const settings = (p.settings ?? {}) as ScopeSettings;
            return (
              <article key={p.block} className="scope-cell" style={area(p)} aria-label={p.block}>
                <header draggable onDragStart={(e) => e.dataTransfer.setData(SCOPE_DRAG, p.block)} title="Drag to move">
                  <span className="lbl">{p.block}</span>
                  <Channels names={run?.names ?? []} picks={picks} onChange={(next) => setPicks(p.block, next)} />
                  <span className="scope-sizes" role="group" aria-label="Size">
                    {(["compact", "normal", "large"] as const).map((s) => (
                      <button key={s} className={`scope-size${p.size === s ? " on" : ""}`} aria-pressed={p.size === s} onClick={() => edit(resizeInstrument(a, p.block, s))}>
                        {SIZE_LABEL[s]}
                      </button>
                    ))}
                  </span>
                  {a.tabs.length > 1 && (
                    <select className="well scope-move" aria-label={`Tab of ${p.block}`} value={p.tab} onChange={(e) => edit(moveToTab(a, p.block, e.target.value))}>
                      {a.tabs.map((t) => (
                        <option key={t}>{t}</option>
                      ))}
                    </select>
                  )}
                  <button className="scope-x" title={`Remove ${p.block}`} onClick={() => edit(removeScope(layout, p.block))}>
                    ×
                  </button>
                </header>
                <Scope
                  name={p.block}
                  channels={channels}
                  version={runVersion}
                  live={false}
                  settings={settings}
                  onSettings={(next) => edit(setSettings(a, p.block, { ...settings, ...next }))}
                  small={p.size !== "large"}
                  placeholder={run ? "Pick channels ▾" : "Simulate to see signals"}
                />
              </article>
            );
          })}
      </div>
      {canvas &&
        createPortal(
      <Dock
        canRun={phase.state !== "running"}
        whyNot="simulating"
        running={phase.state === "running"}
        status={phase.state === "idle" ? undefined : phase.state === "running" ? "running" : phase.state === "failed" ? "failed" : "done"}
        statusText={status}
        optionsSlot={
          <>
          <label className="dock-stop">
            <span className="lbl">stop</span>
            <input className="well" inputMode="decimal" aria-label="Stop time (s)" value={stop} onChange={(e) => setStop(e.target.value)} onKeyDown={(e) => e.key === "Enter" && void go()} />
            <span className="lbl">s</span>
          </label>
          <label className="dock-stop">
            <span className="lbl">solver</span>
            <button className="key small dock-solver" title="Solver settings (Run ▸ Solver settings…): the method omc runs and its options" onClick={() => setSolverOpen(true)}>
              {summary(solver)} ▾
            </button>
          </label>
          </>
        }
        onRun={() => void go()}
        onCancel={cancel}
      />,
          canvas,
        )}
      {solverOpen &&
        createPortal(
          <SolverSettings
            intro="How omc runs this model's Simulate. An export keeps it as far as an FMU can: CVODE, or Euler if picked."
            solvers={SOLVERS}
            options={OPTIONS}
            solver={solver.method}
            values={solver as unknown as Record<string, number | undefined>}
            defaults={DEFAULTS}
            onSolver={(method) => setSolver({ ...solver, method })}
            onOption={(k, v) => setSolver({ ...solver, [k]: v })}
            onReset={() => setSolver({ method: "dassl" })}
            onClose={() => setSolverOpen(false)}
          />,
          document.body,
        )}
    </section>
  );
}
