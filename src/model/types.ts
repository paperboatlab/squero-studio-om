// The seam between Lane A and Lane B in this app (contracts §8). Frozen: change only through
// the orchestrator. Lane A implements StudioApi and provides it; Lane B only consumes it.
import { createContext, useContext } from "react";
import type { Connectors, IconDef, IconPort, ParamDef } from "@paperboat/shared";
import type { OmcClient, OmcStatus } from "../omc/omc.ts";

/** Diagram-layer placement, on the 20px canvas grid. */
export type Placement = { x: number; y: number; rotation: 0 | 90 | 180 | 270; mirror?: boolean };

export type ComponentInst = {
  name: string; // "R1"
  class: string; // "Modelica.Electrical.Analog.Basic.Resistor"
  params: Record<string, string>; // modifiers as Modelica text: { R: "10" }
  defaults?: Record<string, string>; // the class's numeric parameters when unset: { C: "1" }
  placement: Placement;
  icon?: IconDef; // from the Wave 2 importer
  ports: IconPort[];
};

/** An undirected physical connection or a directed causal one: "R1.n" to "L1.p". */
export type Connection = { a: string; b: string; points?: [number, number][] };

/** A top-level parameter (a variable). Public ones are the generated object's Station props; private
 * ones are `protected` in the .mo: shared values inside the model, not exported. */
export type Param = ParamDef & { private?: boolean; modelica?: string };

/** A variable the model declares at its top level: a state, an input/output, or a parameter. */
export type ModelVar = {
  name: string;
  /** Short class name: Real, Boolean, SI.Pressure, … */
  type: string;
  /** parameter | constant | continuous | discrete | "" */
  variability: string;
  /** input | output | "" */
  causality: string;
  visibility: "public" | "protected";
  value?: string;
  comment?: string;
};

/** A top-level causal entry point (+ Input on a port, + Probe on a variable). */
export type EntryPoint = { name: string; kind: "input" | "output"; target: string; unit?: string };

/** Declarations the canvas doesn't draw as written: `protected` parts are hidden, arrays are drawn
 * as one block, and a conditional part is drawn without its condition. */
export type Undrawn = { total: number; hidden: number; arrays: number; conditionals: number };

export type StudioModel = {
  name: string; // qualified class name being edited
  file?: string; // the .mo on disk
  components: ComponentInst[];
  connections: Connection[];
  params: Param[]; // top-level parameters: the public ones are the generated object's Station props
  /** Every top-level variable, for the Variables tree. Parameters also appear in `params`. */
  vars?: ModelVar[];
  entries: EntryPoint[];
  icon?: IconDef; // the model's own look (Look tab), the generated linked block's view.icon
  connectors: Connectors;
  undrawn?: Undrawn; // counted from what omc says; absent when everything is drawn as written
};

export type ExportProgress = { stage: "check" | "flatten" | "generate" | "build"; fraction: number };
export type ExportMode = "pst" | "serve" | "status" | "stop";
export type ExportResult = {
  path?: "ode" | "fmu";
  reason?: string;
  file?: string;
  pst?: string;
  name?: string;
  port?: number;
  state?: "building" | "running" | "exited" | "stopped";
  /** The station log: new text since `offset` (a build streams its output here). */
  log?: string;
  tail?: string;
  offset?: number;
  /** While building, what `paperboat status` says (pb#332): the stage, seconds since the start, a failure's one line. */
  stage?: "prepare" | "configure" | "compile" | "waiting";
  elapsed?: number;
  error?: string;
  failed?: boolean;
};

export interface StudioApi {
  model: StudioModel;
  selection: string[]; // component names or "R1.n" port paths
  status: OmcStatus;
  omc: OmcClient; // raw squero/0 calls (checkModel for the balance count)
  select(names: string[]): void;
  /** "R1.R" sets a component modifier; "J" (no dot) sets a top-level parameter's default. */
  setParam(path: string, value: string): Promise<void>;
  addComponent(cls: string, name: string, at: Placement): Promise<void>;
  /** A new top-level parameter `name = value`, public (a Station prop) or private (protected). */
  addVariable(name: string, value: string, isPrivate: boolean): Promise<void>;
  setPrivate(name: string, isPrivate: boolean): Promise<void>;
  /** Renames a top-level parameter and every modifier and binding that reads it. */
  renameVariable(from: string, to: string): Promise<void>;
  /** Refused while a modifier or binding reads it. */
  removeVariable(name: string): Promise<void>;
  /** null when connected, else the words why not (contracts §4 rules). */
  connect(a: string, b: string): Promise<string | null>;
  /** + Input on a causal input port, + Probe on a variable or output port. */
  addEntry(kind: "input" | "output", target: string): Promise<void>;
  removeEntry(name: string): Promise<void>;
  /** Writes the model's Icon annotation (Look tab). */
  setIcon(icon: IconDef): Promise<void>;
  exportStation(mode: ExportMode, onProgress: (p: ExportProgress) => void, port?: number, offset?: number): Promise<ExportResult>;
}

export const StudioContext = createContext<StudioApi | null>(null);

export function useStudio(): StudioApi {
  const api = useContext(StudioContext);
  if (!api) throw new Error("useStudio outside StudioContext");
  return api;
}
