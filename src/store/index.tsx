// The StudioApi (contracts §8.2): every edit is an omc mutation followed by a re-read, so the
// model on screen is what omc says and the .mo stays OMEdit-compatible. Undo and redo are the
// shared undo.ts over the model's parts and wires; going back replays the difference through omc.

import type { Shape } from "../viewport3d/shapes.ts";
import { fmuSolver, loadSolver, type SolverChoice } from "../simulate/solver.ts";
import { useUndo, type IconDef } from "@paperboat/shared";
import type { Message } from "@paperboat/shared/components";
import type { Edge, Node } from "@xyflow/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Connection, ExportMode, ExportProgress, ExportResult, Placement, StudioApi, StudioModel } from "../model/types.ts";
import type { OmcClient, OmcStatus } from "../omc/omc.ts";
import { parseValue } from "../omc/parse.ts";
import { restoreExprs, same, type Graph, type PartSnap } from "./diff.ts";
import * as x from "./exprs.ts";
import { classLook, type IconCache } from "./looks.ts";
import { readModel } from "./read.ts";
import { freshName, splicePlan, throughPorts } from "./splice.ts";
import { addVariableExprs, entryExprs, connectExprs, iconPortExprs, removeEntryExprs, removeExprs, removeVariableExprs, renameVariableExprs, setParamExprs } from "./plans.ts";
import { endOf, split } from "./wires.ts";

/** What drives a top-level input in a local simulation: k is the value, step height, ramp slope or sine amplitude. */
export type Stimulus = { kind: "constant" | "step" | "ramp" | "sine"; k: number; t0?: number; f?: number };

type PartNode = Node<{ part: PartSnap }>;
type WireEdge = Edge<{ a: string; b: string; points?: [number, number][] }>;

/** What the canvas, library and quick-add do beyond the StudioApi Lane B sees. */
export type Actions = {
  notice: string;
  say(words: string): void;
  move(name: string, to: Placement): Promise<void>;
  /** Redraws a wire through `points` (its bends, a end first). */
  bend(wire: Connection, points: [number, number][]): Promise<void>;
  remove(names: string[], wires: Connection[]): Promise<void>;
  /** Places `cls` at `at` with a fresh name; returns the name. */
  place(cls: string, at: Placement): Promise<string>;
  /** The splice plan for `cls` into `wire`, or the words why not. */
  planSplice(wire: Connection, cls: string): Promise<{ line: string; apply(): Promise<void> } | { why: string }>;
  undo(): void;
  redo(): void;
  canUndo: boolean;
  canRedo: boolean;
  /** .mo text of the model, for the split view. */
  source(): Promise<string>;
  /** checkModel: its balance and every message omc left, for the Console and Messages panel. */
  checkModel(): Promise<{reply: string; equations: number; unknowns: number; messages: Message[]}>;
  /** instantiateModel: the flat Modelica text, for the flat source view. */
  flatModel(): Promise<string>;
  /** The generated station without writing it: path and reason, and the files for the ODE path. */
  preview(): Promise<{ path: "ode" | "fmu"; reason: string; files: Record<string, string>; notices: string[] }>;
  /** A local check run: every variable over [0, stop]; each top-level input fed its stimulus (0 when not given). */
  /** `shapes`: a MultiBody model's visual shapes, for the 3D view. */
  /** `onStage`: compiling, then simulating, as the server sees them. */
  simulate(stop: number, inputs?: Record<string, Stimulus>, solver?: SolverChoice, onStage?: (stage: string) => void): Promise<{ t: number[]; series: Record<string, (number | null)[]>; names: string[]; shapes?: Shape[] }>;
  /** Stop the running simulation; its simulate() rejects with "cancelled". */
  cancelSimulate(): Promise<void>;
  dirty: boolean;
  save(file?: string): Promise<void>;
  saveAs(file: string): Promise<void>;
  /** A fresh model; `file` places it in the current workspace instead of the server's cwd. */
  newModel(name: string, file?: string): Promise<void>;
  openFile(file: string, content?: string): Promise<void>;
  /** Show a model already read this session, without another omc round trip. */
  restore(shown: StudioModel): void;
  /** Writes a new class file (and package.order); opening it is the caller's next step. */
  createClass(spec: { file: string; name: string; kind: string; within: string; base?: string }): Promise<{ file: string; model: string }>;
  newPackage(parent: string, name: string, within: string): Promise<{ path: string; name: string }>;
  duplicateClass(spec: { file: string; name: string; newName: string; within: string }): Promise<{ file: string; model: string }>;
  renameClass(spec: { file: string; name: string; newName: string; within: string }): Promise<{ file: string; model: string }>;
  deleteClass(spec: { file: string; name: string }): Promise<void>;
  deletePackage(spec: { dir: string; root: string }): Promise<void>;
  moveClass(spec: { file: string; targetDir: string; within: string }): Promise<{ file: string; model: string }>;
  /** The .mo file itself, for the text editor (list() is the class body only). */
  fileText(): Promise<string>;
  /** Write the whole file, reload it through omc and re-read the canvas. */
  saveText(text: string): Promise<void>;
  loadLibrary(lib: string): Promise<void>;
};

const asGraph = (m: StudioModel): Graph => ({
  parts: Object.fromEntries(m.components.map((c) => [c.name, { class: c.class, params: c.params, placement: c.placement }])),
  wires: m.connections.map((c) => ({ a: c.a, b: c.b, ...(c.points ? { points: c.points } : {}) })),
});

export function useStudioStore(omc: OmcClient, status: OmcStatus): { api: StudioApi | null; actions: Actions; error: string; stage: string } {
  const [model, setModel] = useState<StudioModel | null>(null);
  const [dirty, setDirty] = useState(false);
  const [selection, setSelection] = useState<string[]>([]);
  const [notice, setNotice] = useState("");
  const [error, setError] = useState("");
  const [stage, setStage] = useState("loading the Modelica library and opening the model");
  const looks = useRef<IconCache>(new Map());
  const current = useRef<StudioModel | null>(null);
  current.current = model;
  const queue = useRef<Promise<unknown>>(Promise.resolve());

  const say = useCallback((words: string) => setNotice(words), []);

  // One edit at a time: the queue keeps an undo replay and a drag from interleaving.
  const enqueue = useCallback(<T,>(job: () => Promise<T>): Promise<T> => {
    const next = queue.current.then(job, job);
    queue.current = next.catch(() => undefined);
    return next;
  }, []);

  const refresh = useCallback(async () => {
    const m = current.current;
    if (!m) return;
    setModel(await readModel(omc, m.name, m.file, looks.current));
  }, [omc]);

  /** omc's words for a failed call: what it said since the last one. */
  const run = useCallback(
    async (exprs: string[]) => {
      for (const expr of exprs) {
        const answer = await omc.call(expr);
        if (answer === "false" || answer === "") {
          const why = (await omc.call("getErrorString()").catch(() => "")).replace(/^"|"$/g, "").replace(/\\n/g, "\n").trim();
          throw new Error(why || `omc refused ${expr.slice(0, expr.indexOf("("))}`);
        }
      }
      setDirty(true);
      await refresh();
    },
    [omc, refresh],
  );

  const edit = useCallback(
    (exprs: string[]) =>
      enqueue(() => run(exprs)).then(
        () => setNotice(""),
        (e: Error) => {
          setNotice(e.message);
          void enqueue(refresh); // omc may have applied some of it; show what it has
          throw e;
        },
      ),
    [enqueue, run, refresh],
  );

  // Open the server's model once the connection is up.
  useEffect(() => {
    let live = true;
    omc.op<{ file: string; model: string }>("open").then(
      async (opened) => {
        setStage(`reading ${opened.model}: its parts, icons and wires`);
        const m = await readModel(omc, opened.model, opened.file, looks.current);
        if (live) {
          setModel(m);
          setDirty(false);
        }
      },
      (e: Error) => live && setError(e.message),
    );
    return () => {
      live = false;
    };
  }, [omc]);

  // --- undo over the model's parts and wires ---
  const nodes = useMemo<PartNode[]>(
    () => (model?.components ?? []).map((c) => ({ id: c.name, position: { x: c.placement.x, y: c.placement.y }, data: { part: { class: c.class, params: c.params, placement: c.placement } } })),
    [model],
  );
  const edges = useMemo<WireEdge[]>(
    () => (model?.connections ?? []).map((c) => ({ id: `${c.a}|${c.b}`, source: split(c.a).part, target: split(c.b).part, data: { a: c.a, b: c.b, ...(c.points ? { points: c.points } : {}) } })),
    [model],
  );
  const wanted = useRef<{ nodes?: PartNode[]; edges?: WireEdge[] }>({});
  const replay = useCallback(() => {
    queueMicrotask(() => {
      const { nodes: n, edges: e } = wanted.current;
      const m = current.current;
      if (!n || !e || !m) return;
      wanted.current = {};
      const then: Graph = { parts: Object.fromEntries(n.map((p) => [p.id, p.data.part])), wires: e.map((w) => ({ a: w.data!.a, b: w.data!.b, ...(w.data!.points ? { points: w.data!.points } : {}) })) };
      void edit(restoreExprs(m.name, asGraph(m), then)).catch(() => undefined);
    });
  }, [edit]);
  const history = useUndo<PartNode, WireEdge>(
    nodes,
    edges,
    (v) => {
      wanted.current.nodes = v as PartNode[];
      replay();
    },
    (v) => {
      wanted.current.edges = v as WireEdge[];
      replay();
    },
    model?.name,
  );
  // useUndo hands out new functions every render; the actions keep one pair
  const steps = useRef(history);
  steps.current = history;
  const undo = useCallback(() => steps.current.undo(), []);
  const redo = useCallback(() => steps.current.redo(), []);
  const { canUndo, canRedo } = history;

  // --- the StudioApi ---
  const api = useMemo<StudioApi | null>(() => {
    if (!model) return null;
    const name = model.name;
    return {
      model,
      selection,
      status,
      omc,
      select: (names) => setSelection((prev) => (prev.join() === names.join() ? prev : names)),
      async setParam(path, value) {
        return edit(setParamExprs(model, path, value));
      },
      async addComponent(cls, partName, at) {
        await edit([x.addComponent(name, cls, partName, at)]);
      },
      async addVariable(id, value, isPrivate) {
        await edit(addVariableExprs(model, id, value, isPrivate));
      },
      async setPrivate(id, isPrivate) {
        await edit([x.setVariable(name, id, isPrivate)]);
      },
      async renameVariable(from, to) {
        await edit(renameVariableExprs(model, from, to));
      },
      async removeVariable(id) {
        await edit(removeVariableExprs(model, id));
      },
      async connect(a, b) {
        const plan = connectExprs(model, a, b);
        if ("error" in plan) return plan.error;
        try {
          await edit(plan.exprs);
          return null;
        } catch (e) {
          return (e as Error).message;
        }
      },
      async addEntry(kind, target) {
        await edit(entryExprs(model, kind, target));
      },
      async removeEntry(entry) {
        await edit(removeEntryExprs(model, entry));
      },
      async setIcon(icon: IconDef) {
        await enqueue(async () => {
          await omc.op("set_icon", { model: name, icon });
          for (const expr of iconPortExprs(model, icon)) await omc.call(expr);
          await omc.op("save", { model: name });
          await refresh();
        });
      },
      async exportStation(mode: ExportMode, onProgress: (p: ExportProgress) => void, port?: number, offset?: number): Promise<ExportResult> {
        return enqueue(async () => {
          if (!model.file) throw new Error("save the model to a file first");
          return omc.op<ExportResult>("export", { source: model.file, model: name, mode, port, offset, fmu_solver: fmuSolver(loadSolver(name)) }, (stage, fraction) =>
            onProgress({ stage: stage as ExportProgress["stage"], fraction }),
          );
        });
      },
    };
  }, [model, selection, status, omc, edit, enqueue, refresh]);

  const actions = useMemo<Actions>(
    () => ({
      notice,
      say,
      async move(partName, to) {
        const m = current.current;
        const part = m?.components.find((c) => c.name === partName);
        if (!m || !part || same(part.placement, to)) return;
        await edit([x.updateComponent(m.name, part.class, partName, to)]).catch(() => undefined);
      },
      async bend(wire, points) {
        const m = current.current;
        if (!m) return;
        await edit([x.deleteConnection(m.name, wire.a, wire.b), x.addConnection(m.name, wire.a, wire.b, points.map(([px, py]) => [x.snap(px), x.snap(py)]))]).catch(() => undefined);
      },
      async remove(names, wires) {
        const m = current.current;
        if (!m) return;
        await edit(removeExprs(m, names, wires)).catch(() => undefined);
        setSelection([]);
      },
      async place(cls, at) {
        const m = current.current!;
        const partName = freshName(cls, m.components.map((c) => c.name));
        await edit([x.addComponent(m.name, cls, partName, at)]).catch(() => undefined);
        return partName;
      },
      async planSplice(wire, cls) {
        const m = current.current!;
        const look = await classLook(omc, looks.current, cls, "");
        const ports = look.icon?.ports ?? [];
        const wireEnd = endOf(m, wire.a);
        if (!wireEnd) return { why: "that wire has an end that is gone" };
        const through = throughPorts(ports, { ...m.connectors, ...look.connectors }, wireEnd.cls);
        if ("why" in through) return { why: `${cls.slice(cls.lastIndexOf(".") + 1)} can't go in this wire: ${through.why}` };
        const pa = m.components.find((c) => c.name === split(wire.a).part);
        const pb = m.components.find((c) => c.name === split(wire.b).part);
        // causal wires run output -> input, physical ones left to right
        const reversed = wireEnd.kind === "acausal" ? (pa?.placement.x ?? 0) > (pb?.placement.x ?? 0) : wireEnd.kind === "input";
        const mid: [number, number] = pa && pb ? [(pa.placement.x + pb.placement.x) / 2, (pa.placement.y + pb.placement.y) / 2] : [0, 0];
        const partName = freshName(cls, m.components.map((c) => c.name));
        const plan = splicePlan(m.name, wire, cls, partName, through, mid, reversed);
        return { line: plan.line, apply: () => edit(plan.exprs).catch(() => undefined) };
      },
      undo,
      redo,
      canUndo,
      canRedo,
      preview() {
        const m = current.current;
        if (!m?.file) return Promise.reject(new Error("the model has no file yet"));
        // queued behind edits: generating reloads the file, so a half-saved edit must not be in flight
        return enqueue(() => omc.op<{ path: "ode" | "fmu"; reason: string; files?: Record<string, string>; notices?: string[] }>("generate", { source: m.file, model: m.name, dry: true })).then((r) => ({ path: r.path, reason: r.reason, files: r.files ?? {}, notices: r.notices ?? [] }));
      },
      checkModel() {
        const m = current.current;
        if (!m) return Promise.reject(new Error("no model"));
        return enqueue(() => omc.op<{ reply: string; equations: number; unknowns: number; messages: Message[] }>("check_model", { model: m.name }));
      },
      flatModel() {
        const m = current.current;
        if (!m) return Promise.reject(new Error("no model"));
        return enqueue(() => omc.op<{ text: string; messages?: Message[] }>("instantiate_model", { model: m.name })).then((r) => {
          if (!r.text) throw new Error(r.messages?.[0]?.text ?? "omc could not flatten this model");
          return r.text;
        });
      },
      simulate(stop, inputs = {}, solver, onStage) {
        const m = current.current;
        if (!m) return Promise.reject(new Error("no model"));
        return omc.op("simulate", { model: m.name, stop, inputs, ...solver }, onStage && ((stage) => onStage(stage)));
      },
      cancelSimulate: () => omc.cancel(),
      async source() {
        const m = current.current;
        if (!m) return "";
        return String(parseValue(await omc.call(`list(${m.name})`)));
      },
      dirty,
      async save(file?: string) {
        const m = current.current;
        if (!m) return;
        const target = file ?? m.file;
        await enqueue(async () => {
          await omc.op("save", { model: m.name, ...(target ? { file: target } : {}) });
          setDirty(false);
          say(`Saved ${m.name}${target ? ` to ${target}` : ""}`);
        });
      },
      async saveAs(filePath: string) {
        const m = current.current;
        if (!m) return;
        const target = filePath.trim();
        if (!target) return;
        await enqueue(async () => {
          await omc.op("save", { model: m.name, file: target });
          setDirty(false);
          await refresh();
          say(`Saved ${m.name} to ${target}`);
        });
      },
      restore(shown) {
        setModel(shown);
        setDirty(false);
        setSelection([]);
        say(`Opened ${shown.name}`);
      },
      async newModel(name: string, file?: string) {
        const raw = name.trim();
        if (!raw) return;
        const target = file?.trim() || (raw.endsWith(".mo") ? raw : `${raw}.mo`);
        await enqueue(async () => {
          const opened = await omc.op<{ file: string; model: string }>("open", { file: target });
          setStage(`reading ${opened.model}: its parts, icons and wires`);
        const m = await readModel(omc, opened.model, opened.file, looks.current);
          setModel(m);
          setDirty(false);
          setSelection([]);
          say(`Created model ${opened.model}`);
        });
      },
      createClass(spec) {
        return enqueue(() => omc.op<{ file: string; model: string }>("create_class", spec));
      },
      newPackage(parent, name, within) {
        return enqueue(() => omc.op<{ path: string; name: string }>("new_package", { parent, name, within }));
      },
      duplicateClass(spec) {
        return enqueue(() => omc.op<{ file: string; model: string }>("duplicate_class", spec));
      },
      renameClass(spec) {
        return enqueue(() => omc.op<{ file: string; model: string }>("rename_class", spec));
      },
      deleteClass(spec) {
        return enqueue(() => omc.op<void>("delete_class", spec));
      },
      deletePackage(spec) {
        return enqueue(() => omc.op<void>("delete_package", spec));
      },
      moveClass(spec) {
        return enqueue(() => omc.op<{ file: string; model: string }>("move_class", spec));
      },
      fileText() {
        const m = current.current;
        if (!m?.file) return Promise.reject(new Error("the model has no file yet"));
        return enqueue(() => omc.op<{ text: string }>("read_file", { file: m.file }).then((r) => r.text));
      },
      saveText(text) {
        const m = current.current;
        if (!m?.file) return Promise.reject(new Error("the model has no file yet"));
        return enqueue(async () => {
          await omc.op("save", { model: m.name, file: m.file, content: text });
          const opened = await omc.op<{ file: string; model: string }>("open", { file: m.file });
          const next = await readModel(omc, opened.model, opened.file, looks.current);
          setModel(next);
          setDirty(false);
          setSelection([]);
          say(`Saved ${opened.model}`);
        });
      },
      async openFile(file: string, content?: string) {
        const target = file.trim();
        if (!target) return;
        await enqueue(async () => {
          const opened = await omc.op<{ file: string; model: string }>("open", { file: target, ...(content ? { content } : {}) });
          setStage(`reading ${opened.model}: its parts, icons and wires`);
        const m = await readModel(omc, opened.model, opened.file, looks.current);
          setModel(m);
          setDirty(false);
          setSelection([]);
          say(`Opened ${opened.model}`);
        });
      },
      async loadLibrary(lib: string) {
        const raw = lib.trim();
        if (!raw) return;
        await enqueue(async () => {
          let ans = await omc.call(`loadModel(${raw})`).catch(() => "false");
          if (ans !== "true") {
            ans = await omc.call(`loadFile("${raw}")`).catch(() => "false");
          }
          if (ans !== "true") {
            const why = (await omc.call("getErrorString()").catch(() => "")).replace(/^"|"$/g, "").replace(/\\n/g, "\n").trim();
            throw new Error(why || `Could not load library ${raw}`);
          }
          say(`Loaded library ${raw}`);
        });
      },
    }),
    [notice, say, edit, omc, undo, redo, canUndo, canRedo, dirty, enqueue, refresh],
  );

  return { api, actions, error, stage };
}

