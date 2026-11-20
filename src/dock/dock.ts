import type { ConnectorDef, IconPort } from "@paperboat/shared";
import type {
  ComponentInst,
  ExportMode,
  ExportProgress,
  ExportResult,
  Placement,
  StudioApi,
  StudioModel,
} from "../model/types.ts";

export type BalanceResult =
  | { kind: "balanced"; equations: number; unknowns: number; ratio: string; words: string }
  | { kind: "under"; equations: number; unknowns: number; diff: number; ratio: string; words: string }
  | { kind: "over"; equations: number; unknowns: number; diff: number; ratio: string; words: string }
  | { kind: "error"; equations: null; unknowns: null; ratio: string; words: string };

const CHECK_RE = /(\d+)\s+equations?(?:\(s\))?\s+and\s+(\d+)\s+(?:variables?|unknowns?)(?:\(s\))?/i;
/** A fixed state adds an initial equation the counts above do not include. checkModel is the judge. */
const PASSED_RE = /completed successfully/i;

export function parseBalance(reply: string | Error): BalanceResult {
  if (reply instanceof Error) {
    return {
      kind: "error",
      equations: null,
      unknowns: null,
      ratio: "– / –",
      words: reply.message || "omc checkModel failed",
    };
  }
  const text = String(reply ?? "").trim().replace(/^"(.*)"$/s, "$1").trim();
  const match = CHECK_RE.exec(text);
  if (!match) {
    return {
      kind: "error",
      equations: null,
      unknowns: null,
      ratio: "– / –",
      words: text || "omc checkModel returned no equation count",
    };
  }
  const equations = Number(match[1]);
  const unknowns = Number(match[2]);
  const ratio = `${equations} / ${unknowns}`;
  if (equations === unknowns) {
    return {
      kind: "balanced",
      equations,
      unknowns,
      ratio,
      words: `balanced · ${equations} / ${unknowns} equations / unknowns`,
    };
  }
  // A model with a fixed state can report one more equation than unknowns (the initial equation)
  // and still pass; the counts alone would call that over-determined.
  if (PASSED_RE.test(text)) {
    return {
      kind: "balanced",
      equations,
      unknowns,
      ratio,
      words: `balanced · ${equations} / ${unknowns} equations / unknowns · checkModel passed`,
    };
  }
  if (equations < unknowns) {
    const diff = unknowns - equations;
    return {
      kind: "under",
      equations,
      unknowns,
      diff,
      ratio,
      words: `unbalanced · ${equations} / ${unknowns} (${diff} fewer equation${diff === 1 ? "" : "s"} than unknowns)`,
    };
  }
  const diff = equations - unknowns;
  return {
    kind: "over",
    equations,
    unknowns,
    diff,
    ratio,
    words: `unbalanced · ${equations} / ${unknowns} (${diff} more equation${diff === 1 ? "" : "s"} than unknowns)`,
  };
}

/** What the canvas can't show as written, in the dock's words; null when nothing is missing. */
export function undrawnNote(undrawn: StudioModel["undrawn"]): { label: string; title: string } | null {
  if (!undrawn || undrawn.total < 1) return null;
  const parts: string[] = [];
  if (undrawn.hidden) parts.push(`${undrawn.hidden} protected part${undrawn.hidden === 1 ? " is" : "s are"} hidden`);
  if (undrawn.arrays) parts.push(`${undrawn.arrays} array${undrawn.arrays === 1 ? " is" : "s are"} drawn as one block`);
  if (undrawn.conditionals)
    parts.push(
      `${undrawn.conditionals} conditional part${undrawn.conditionals === 1 ? " is" : "s are"} drawn without its condition`,
    );
  return {
    label: `${undrawn.total} not drawn as declared`,
    title: `${parts.join("; ")}. Edit the .mo in Text to see the declaration.`,
  };
}

/** omc's error, in the words the footer shows: the common ones say what to do. */
export function explainOmcError(why: string): string {
  const wire = /connectors in connect\(([^,]+),\s*([^)]+)\) are not type compatible/.exec(why);
  const local = (path: string) => path.replace(/^m\./, ""); // Simulate's harness holds the model as m
  if (wire) return `${local(wire[1])} and ${local(wire[2])} can't be wired: they are different kinds of port (say a pin and a signal). Delete that wire; to read a pin as a signal, put a sensor such as VoltageSensor in between.`;
  return why;
}

export async function checkModelBalance(api: { omc: Pick<StudioApi["omc"], "call">; model: { name: string } }): Promise<BalanceResult> {
  try {
    await api.omc.call("getErrorString()").catch(() => ""); // drop what other calls left, so the error read below is checkModel's
    const reply = await api.omc.call(`checkModel(${api.model.name})`);
    const res = parseBalance(reply);
    if (res.kind !== "error") return res;
    // an empty reply means omc refused the model: its first error says why
    const why = /Error: ([^\n"]*)/.exec(await api.omc.call("getErrorString()").catch(() => ""))?.[1];
    return why ? { ...res, words: explainOmcError(why) } : res;
  } catch (err) {
    return parseBalance(err instanceof Error ? err : new Error(String(err)));
  }
}

export type DanglingDomain = "electrical" | "translational" | "rotational" | "other";

export type DanglingFix = {
  className:
    | "Modelica.Electrical.Analog.Basic.Ground"
    | "Modelica.Mechanics.Translational.Components.Fixed"
    | "Modelica.Mechanics.Rotational.Components.Fixed";
  shortName: "Ground" | "Fixed";
  buttonLabel: "Fix: Ground" | "Fix: Fixed";
  prefix: "G" | "fixed";
  port: "p" | "flange";
};

export type DanglingPort = {
  portPath: string;
  component: string;
  port: string;
  portClass: string;
  side: IconPort["side"];
  domain: DanglingDomain;
  label: string;
  fix: DanglingFix | null;
};

export function detectDomainAndFix(
  comp: ComponentInst,
  port: IconPort,
  connDef: ConnectorDef | undefined,
): { domain: DanglingDomain; fix: DanglingFix | null } {
  const isElectrical =
    /(^|\.)(Positive|Negative)?Pin$/i.test(port.class) ||
    comp.class.startsWith("Modelica.Electrical.") ||
    Boolean(connDef?.flow?.some((f) => f.unit === "A" || f.id === "i"));

  if (isElectrical) {
    return {
      domain: "electrical",
      fix: {
        className: "Modelica.Electrical.Analog.Basic.Ground",
        shortName: "Ground",
        buttonLabel: "Fix: Ground",
        prefix: "G",
        port: "p",
      },
    };
  }

  const isTranslational =
    comp.class.includes("Translational") ||
    port.class.includes("Translational") ||
    (/^Flange(_[ab])?$/i.test(port.class) &&
      Boolean(
        connDef?.flow?.some((f) => f.unit === "N" || f.id === "f") ||
          connDef?.potential?.some((p) => p.unit === "m" || p.id === "s"),
      ));

  if (isTranslational) {
    return {
      domain: "translational",
      fix: {
        className: "Modelica.Mechanics.Translational.Components.Fixed",
        shortName: "Fixed",
        buttonLabel: "Fix: Fixed",
        prefix: "fixed",
        port: "flange",
      },
    };
  }

  const isRotational =
    comp.class.includes("Rotational") ||
    port.class.includes("Rotational") ||
    /^Flange(_[ab])?$/i.test(port.class) ||
    Boolean(
      connDef?.flow?.some((f) => f.unit === "N.m" || f.id === "tau") ||
        connDef?.potential?.some((p) => p.unit === "rad" || p.id === "phi"),
    );

  if (isRotational) {
    return {
      domain: "rotational",
      fix: {
        className: "Modelica.Mechanics.Rotational.Components.Fixed",
        shortName: "Fixed",
        buttonLabel: "Fix: Fixed",
        prefix: "fixed",
        port: "flange",
      },
    };
  }

  return { domain: "other", fix: null };
}

function truthyModifier(val: string | undefined): boolean {
  return val !== undefined && val !== "" && val !== "false" && val !== "0";
}

export function findDanglingFlowPorts(model: StudioModel): DanglingPort[] {
  const connected = new Set<string>();
  for (const conn of model.connections) {
    connected.add(conn.a);
    connected.add(conn.b);
    connected.add(conn.a.replace(/\[.*\]$/, ""));
    connected.add(conn.b.replace(/\[.*\]$/, ""));
  }

  const out: DanglingPort[] = [];
  for (const comp of model.components) {
    for (const port of comp.ports) {
      if (port.if !== undefined && !truthyModifier(comp.params[port.if])) continue;
      const connDef = model.connectors[port.class];
      if (!connDef || !Array.isArray(connDef.flow) || connDef.flow.length === 0) continue;
      const portPath = `${comp.name}.${port.id}`;
      if (connected.has(portPath)) continue;
      const { domain, fix } = detectDomainAndFix(comp, port, connDef);
      out.push({
        portPath,
        component: comp.name,
        port: port.id,
        portClass: port.class,
        side: port.side,
        domain,
        label: `${portPath} unconnected`,
        fix,
      });
    }
  }
  return out;
}

const snap20 = (n: number) => Math.round(n / 20) * 20;

export function nextFixComponentName(model: StudioModel, prefix: string): string {
  const used = new Set(model.components.map((c) => c.name));
  let idx = 1;
  while (used.has(`${prefix}${idx}`)) idx++;
  return `${prefix}${idx}`;
}

export function placementNearPort(comp: ComponentInst | undefined, side: IconPort["side"]): Placement {
  const baseX = comp?.placement.x ?? 100;
  const baseY = comp?.placement.y ?? 100;
  switch (side) {
    case "left":
      return { x: snap20(baseX - 60), y: snap20(baseY), rotation: 0 };
    case "right":
      return { x: snap20(baseX + 60), y: snap20(baseY), rotation: 0 };
    case "top":
      return { x: snap20(baseX), y: snap20(baseY - 60), rotation: 0 };
    case "bottom":
      return { x: snap20(baseX), y: snap20(baseY + 60), rotation: 0 };
  }
}

export async function fixDanglingPort(api: StudioApi, item: DanglingPort): Promise<string | null> {
  if (!item.fix) return "no automatic fix for this domain";
  const comp = api.model.components.find((c) => c.name === item.component);
  const newName = nextFixComponentName(api.model, item.fix.prefix);
  const at = placementNearPort(comp, item.side);
  await api.addComponent(item.fix.className, newName, at);
  return await api.connect(item.portPath, `${newName}.${item.fix.port}`);
}

export function defaultExportFolder(modelName: string): string {
  // the generator's station_name (server/src/generate.cpp): DCMotor -> dc-motor
  const short = (modelName.split(".").pop() ?? modelName)
    .replace(/([a-z0-9])([A-Z])|([A-Z])([A-Z][a-z])/g, (_, a, b, c, d) => (a ? `${a}-${b}` : `${c}-${d}`))
    .replace(/[^A-Za-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .toLowerCase();
  return `${short || "model"}-cpp`;
}

export type ExportOutcome =
  | { ok: true; result: ExportResult; line: string }
  | { ok: false; result?: ExportResult; line: string };

function saveBase64(file: string, b64: string) {
  const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type: "application/zip" }));
  a.download = file;
  document.body.append(a);
  a.click();
  setTimeout(() => {
    a.remove();
    URL.revokeObjectURL(a.href);
  }, 1000);
}

export function serveLine(mode: ExportMode, r: ExportResult, port?: number): string {
  if (mode === "stop") return `stopped station on port ${port}`;
  if (mode === "pst") return `${r.file} downloaded · ${r.reason}`;
  if (r.state === "running") return `${r.name ?? "station"} running on port ${r.port}`;
  if (r.state === "exited") return `station on port ${r.port} exited:\n${r.tail ?? r.log ?? ""}`;
  return `building ${r.name ?? "station"} for port ${r.port}… (the first build compiles the engine: minutes)`;
}

export async function runStationExport(
  api: StudioApi,
  mode: ExportMode,
  onProgress: (p: ExportProgress) => void,
  port?: number,
  offset?: number,
): Promise<ExportOutcome> {
  try {
    const result = await api.exportStation(mode, onProgress, port, offset);
    if (result.pst && result.file) saveBase64(result.file, result.pst);
    const line = serveLine(mode, result, port);
    return result.state === "exited" ? { ok: false, result, line } : { ok: true, result, line };
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return { ok: false, line: explainOmcError(msg) || "export failed" };
  }
}
