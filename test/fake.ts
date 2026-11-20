import type { Connectors, IconDef } from "@paperboat/shared";
import type {
  ComponentInst,
  Connection,
  EntryPoint,
  ExportMode,
  ExportProgress,
  ExportResult,
  Placement,
  StudioApi,
  StudioModel,
} from "../src/model/types.ts";
import type { OmcClient, OmcStatus } from "../src/omc/omc.ts";

export type RecordedCall = {
  method:
    | "select"
    | "setParam"
    | "addComponent"
    | "addVariable"
    | "setPrivate"
    | "renameVariable"
    | "removeVariable"
    | "connect"
    | "addEntry"
    | "removeEntry"
    | "setIcon"
    | "exportStation"
    | "omc.call";
  args: unknown[];
};

export const DEFAULT_CONNECTORS: Connectors = {
  Pin: {
    kind: "acausal",
    handle: "square",
    size: 8,
    weight: 1.2,
    potential: [{ id: "v", unit: "V" }],
    flow: [{ id: "i", unit: "A" }],
  },
  PositivePin: {
    kind: "acausal",
    handle: "square",
    size: 8,
    weight: 1.2,
    alt: "filled",
    potential: [{ id: "v", unit: "V" }],
    flow: [{ id: "i", unit: "A" }],
  },
  NegativePin: {
    kind: "acausal",
    handle: "square",
    size: 8,
    weight: 1.2,
    potential: [{ id: "v", unit: "V" }],
    flow: [{ id: "i", unit: "A" }],
  },
  Flange_a: {
    kind: "acausal",
    handle: "square",
    size: 10,
    weight: 1.8,
    alt: "filled",
    potential: [{ id: "phi", unit: "rad" }],
    flow: [{ id: "tau", unit: "N.m" }],
  },
  Flange_b: {
    kind: "acausal",
    handle: "square",
    size: 10,
    weight: 1.8,
    potential: [{ id: "phi", unit: "rad" }],
    flow: [{ id: "tau", unit: "N.m" }],
  },
  HeatPort_a: {
    kind: "acausal",
    handle: "diamond",
    size: 8,
    weight: 1.2,
    dash: "dashed",
    potential: [{ id: "T", unit: "K" }],
    flow: [{ id: "Q_flow", unit: "W" }],
  },
  HeatPort_b: {
    kind: "acausal",
    handle: "diamond",
    size: 8,
    weight: 1.2,
    dash: "dashed",
    potential: [{ id: "T", unit: "K" }],
    flow: [{ id: "Q_flow", unit: "W" }],
  },
  FluidPort_a: {
    kind: "acausal",
    handle: "ring2",
    size: 12,
    weight: 2,
    potential: [{ id: "p", unit: "Pa" }],
    flow: [{ id: "m_flow", unit: "kg/s" }],
    stream: [{ id: "h_outflow", unit: "J/kg" }],
  },
  FluidPort_b: {
    kind: "acausal",
    handle: "ring2",
    size: 12,
    weight: 2,
    potential: [{ id: "p", unit: "Pa" }],
    flow: [{ id: "m_flow", unit: "kg/s" }],
    stream: [{ id: "h_outflow", unit: "J/kg" }],
  },
  RealInput: {
    kind: "input",
    handle: "triangle",
    size: 8,
    weight: 1.2,
  },
  IntegerInput: {
    kind: "input",
    handle: "triangle",
    size: 8,
    weight: 1.2,
  },
  BooleanInput: {
    kind: "input",
    handle: "triangle",
    size: 8,
    weight: 1.2,
  },
  RealOutput: {
    kind: "output",
    handle: "circle",
    size: 8,
    weight: 1.2,
    alt: "filled",
  },
  IntegerOutput: {
    kind: "output",
    handle: "circle",
    size: 8,
    weight: 1.2,
    alt: "filled",
  },
  BooleanOutput: {
    kind: "output",
    handle: "circle",
    size: 8,
    weight: 1.2,
    alt: "filled",
  },
};

export const RESISTOR_ICON: IconDef = {
  w: 1,
  h: 1,
  g: [
    { t: "line", p: [[0, 50], [20, 50]] },
    { t: "rect", x: 20, y: 30, w: 60, h: 40, fill: "raised", corner: "sm" },
    { t: "line", p: [[80, 50], [100, 50]] },
    { t: "text", x: 50, y: 50, s: "{R}", font: "mono", size: 12, stroke: "ink" },
  ],
  ports: [
    { id: "p", class: "PositivePin", side: "left", at: 50, variant: "filled" },
    { id: "n", class: "NegativePin", side: "right", at: 50, variant: "hollow" },
    { id: "u", class: "RealInput", side: "top", at: 50 },
  ],
};

export const INDUCTOR_ICON: IconDef = {
  w: 1,
  h: 1,
  g: [
    { t: "line", p: [[0, 50], [15, 50]] },
    { t: "arc", x: 15, y: 35, w: 25, h: 30, a0: 0, a1: 180 },
    { t: "arc", x: 40, y: 35, w: 20, h: 30, a0: 0, a1: 180 },
    { t: "arc", x: 60, y: 35, w: 25, h: 30, a0: 0, a1: 180 },
    { t: "line", p: [[85, 50], [100, 50]] },
  ],
  ports: [
    { id: "p", class: "PositivePin", side: "left", at: 50, variant: "filled" },
    { id: "n", class: "NegativePin", side: "right", at: 50, variant: "hollow" },
  ],
};

export const REAL_INPUT_ICON: IconDef = {
  w: 1,
  h: 1,
  g: [
    { t: "poly", p: [[15, 20], [85, 50], [15, 80]], fill: "raised", stroke: "ink" },
  ],
  ports: [
    { id: "y", class: "RealOutput", side: "right", at: 50, variant: "filled" },
  ],
};

export const DEFAULT_MODEL_ICON: IconDef = {
  w: 2,
  h: 1,
  g: [
    { t: "rect", x: 15, y: 20, w: 70, h: 60, fill: "raised", corner: "md" },
    { t: "line", p: [[0, 50], [15, 50]], weight: "wire" },
    { t: "line", p: [[85, 50], [100, 50]], weight: "wire" },
    { t: "ellipse", x: 35, y: 30, w: 30, h: 40, fill: "sunken" },
    { t: "text", x: 50, y: 50, s: "RL", font: "mono", size: 12, stroke: "ink" },
  ],
  ports: [
    { id: "u", class: "RealInput", side: "left", at: 50 },
    { id: "i_L", class: "RealOutput", side: "right", at: 50, variant: "filled" },
  ],
};

/**
 * Default in-memory StudioModel: a Resistor (R1), an Inductor (L1), no Ground, and one
 * RealInput (u_in). R1.p and R1.n->L1.p are wired so L1.n is the single dangling flow port.
 */
export function createDefaultModel(): StudioModel {
  return {
    name: "RLCircuit",
    file: "RLCircuit.mo",
    components: [
      {
        name: "R1",
        class: "Modelica.Electrical.Analog.Basic.Resistor",
        params: { R: "10", T_ref: "300.15" },
        placement: { x: 120, y: 120, rotation: 0 },
        icon: structuredClone(RESISTOR_ICON),
        ports: structuredClone(RESISTOR_ICON.ports ?? []),
      },
      {
        name: "L1",
        class: "Modelica.Electrical.Analog.Basic.Inductor",
        params: { L: "0.05" },
        placement: { x: 240, y: 120, rotation: 0 },
        icon: structuredClone(INDUCTOR_ICON),
        ports: structuredClone(INDUCTOR_ICON.ports ?? []),
      },
      {
        name: "u_in",
        class: "Modelica.Blocks.Interfaces.RealInput",
        params: {},
        placement: { x: 40, y: 120, rotation: 0 },
        icon: structuredClone(REAL_INPUT_ICON),
        ports: structuredClone(REAL_INPUT_ICON.ports ?? []),
      },
    ],
    connections: [
      { a: "u_in.y", b: "R1.p" },
      { a: "R1.n", b: "L1.p" },
    ],
    params: [
      { id: "R_nom", type: "number", default: 10, unit: "Ohm" },
      { id: "L_nom", type: "number", default: 0.05, unit: "H" },
    ],
    entries: [
      { name: "u", kind: "input", target: "R1.u", unit: "V" },
      { name: "i_L", kind: "output", target: "L1.n", unit: "A" },
    ],
    icon: structuredClone(DEFAULT_MODEL_ICON),
    connectors: structuredClone(DEFAULT_CONNECTORS),
  };
}

export type FakeStudioOptions = {
  model?: StudioModel;
  selection?: string[];
  status?: OmcStatus;
  checkModelReply?: string | Error | ((expr: string) => string | Promise<string>);
  exportSteps?: ExportProgress[];
  exportResult?: ExportResult | Error;
  connectReply?: string | null;
  onMutate?: (model: StudioModel) => void;
};

export interface FakeStudio extends StudioApi {
  readonly calls: RecordedCall[];
  setCheckModelReply(reply: string | Error | ((expr: string) => string | Promise<string>)): void;
  setExportScript(steps: ExportProgress[], result?: ExportResult | Error): void;
}

const DEFAULT_CHECK_REPLY =
  "Check of RLCircuit completed successfully.\nClass RLCircuit has 14 equation(s) and 14 variable(s).\n2 of these are trivial equation(s).";

const DEFAULT_EXPORT_STEPS: ExportProgress[] = [
  { stage: "check", fraction: 0.25 },
  { stage: "flatten", fraction: 0.5 },
  { stage: "generate", fraction: 0.75 },
  { stage: "build", fraction: 1.0 },
];

export function createFakeStudio(options: FakeStudioOptions = {}): FakeStudio {
  const calls: RecordedCall[] = [];
  const model: StudioModel = options.model ?? createDefaultModel();
  let checkReply: string | Error | ((expr: string) => string | Promise<string>) =
    options.checkModelReply ?? DEFAULT_CHECK_REPLY;
  let exportSteps: ExportProgress[] = options.exportSteps ?? DEFAULT_EXPORT_STEPS;
  let exportResult: ExportResult | Error = options.exportResult ?? {
    file: "ssom_rl_circuit.pst",
    path: "ode",
    reason: "no events, no nonlinear torn systems, no external tables",
  };

  const notify = () => {
    options.onMutate?.(model);
  };

  const omc = {
    status: options.status ?? { state: "ready", version: "1.27.0" },
    async call(expr: string): Promise<string> {
      calls.push({ method: "omc.call", args: [expr] });
      if (typeof checkReply === "function") return await checkReply(expr);
      if (checkReply instanceof Error) throw checkReply;
      return checkReply;
    },
    close() {},
  } as unknown as OmcClient;

  const api: FakeStudio = {
    model,
    selection: options.selection ?? ["R1"],
    status: options.status ?? { state: "ready", version: "1.27.0" },
    omc,
    calls,

    setCheckModelReply(reply) {
      checkReply = reply;
    },

    setExportScript(steps, result) {
      exportSteps = steps;
      if (result !== undefined) exportResult = result;
    },

    select(names: string[]) {
      calls.push({ method: "select", args: [names] });
      api.selection = [...names];
      notify();
    },

    async setParam(path: string, value: string): Promise<void> {
      calls.push({ method: "setParam", args: [path, value] });
      const dot = path.indexOf(".");
      if (dot >= 0) {
        const compName = path.slice(0, dot);
        const paramName = path.slice(dot + 1);
        const comp = model.components.find((c) => c.name === compName);
        if (comp) {
          comp.params = { ...comp.params, [paramName]: value };
        }
      } else {
        const num = Number(value);
        const existing = model.params.find((p) => p.id === path);
        if (existing) {
          if (existing.type === "number" && Number.isFinite(num)) {
            existing.default = num;
          } else if (existing.type === "boolean") {
            existing.default = value === "true" || value === "1";
          } else if (existing.type !== "number") {
            existing.default = value;
          }
        } else {
          model.params = [
            ...model.params,
            Number.isFinite(num) && value.trim() !== ""
              ? { id: path, type: "number", default: num }
              : { id: path, type: "text", default: value },
          ];
        }
      }
      notify();
    },

    async addVariable(name: string, value: string, isPrivate: boolean): Promise<void> {
      calls.push({ method: "addVariable", args: [name, value, isPrivate] });
      const num = Number(value);
      model.params = [...model.params, { id: name, type: "number", default: Number.isFinite(num) ? num : 0, ...(isPrivate ? { private: true } : {}) }];
      notify();
    },

    async setPrivate(name: string, isPrivate: boolean): Promise<void> {
      calls.push({ method: "setPrivate", args: [name, isPrivate] });
      model.params = model.params.map((p) => (p.id === name ? { ...p, private: isPrivate } : p));
      notify();
    },

    async renameVariable(from: string, to: string): Promise<void> {
      calls.push({ method: "renameVariable", args: [from, to] });
      model.params = model.params.map((p) => (p.id === from ? { ...p, id: to } : p));
      for (const c of model.components) for (const [k, v] of Object.entries(c.params)) if (v === from) c.params = { ...c.params, [k]: to };
      notify();
    },

    async removeVariable(name: string): Promise<void> {
      calls.push({ method: "removeVariable", args: [name] });
      model.params = model.params.filter((p) => p.id !== name);
      notify();
    },

    async addComponent(cls: string, name: string, at: Placement): Promise<void> {
      calls.push({ method: "addComponent", args: [cls, name, at] });
      const defaultPorts: ComponentInst["ports"] = cls.endsWith(".Ground")
        ? [{ id: "p", class: "Pin", side: "top", at: 50 }]
        : cls.endsWith(".Fixed")
          ? [{ id: "flange", class: "Flange_b", side: "top", at: 50 }]
          : [];
      model.components = [
        ...model.components,
        {
          name,
          class: cls,
          params: {},
          placement: { ...at },
          ports: defaultPorts,
        },
      ];
      notify();
    },

    async connect(a: string, b: string): Promise<string | null> {
      calls.push({ method: "connect", args: [a, b] });
      if (options.connectReply !== undefined && options.connectReply !== null) {
        return options.connectReply;
      }
      const next: Connection = { a, b };
      model.connections = [...model.connections, next];
      notify();
      return null;
    },

    async addEntry(kind: "input" | "output", target: string): Promise<void> {
      calls.push({ method: "addEntry", args: [kind, target] });
      const base = target.replace(".", "_");
      let name = base;
      let idx = 2;
      while (model.entries.some((e) => e.name === name)) {
        name = `${base}_${idx++}`;
      }
      const entry: EntryPoint = { name, kind, target };
      model.entries = [...model.entries, entry];
      notify();
    },

    async removeEntry(name: string): Promise<void> {
      calls.push({ method: "removeEntry", args: [name] });
      model.entries = model.entries.filter((e) => e.name !== name);
      notify();
    },

    async setIcon(icon: IconDef): Promise<void> {
      calls.push({ method: "setIcon", args: [icon] });
      model.icon = structuredClone(icon);
      notify();
    },

    async exportStation(mode: ExportMode, onProgress: (p: ExportProgress) => void, port?: number): Promise<ExportResult> {
      calls.push({ method: "exportStation", args: [mode, port] });
      for (const step of exportSteps) {
        onProgress(step);
      }
      if (exportResult instanceof Error) {
        throw exportResult;
      }
      return exportResult;
    },
  };

  return api;
}
