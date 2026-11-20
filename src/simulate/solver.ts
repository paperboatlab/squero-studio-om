// Studio's solver settings for a Simulate run (omc's `simulate` method and options), kept per
// model in localStorage. An export carries the choice into the station as far as the FMU allows:
// its solver is CVODE, or Euler when Euler was picked.

import type { SolverInfo, SolverOptionInfo } from "@paperboat/shared/components";

export type SolverChoice = { method: string; tolerance?: number; intervals?: number; start?: number };

export const SOLVERS: SolverInfo[] = [
  { id: "dassl", label: "DASSL", family: "implicit", blurb: "omc's default: BDF for stiff DAEs, robust", options: ["tolerance", "intervals", "start"], recommended: true },
  { id: "ida", label: "IDA (SUNDIALS)", family: "implicit", blurb: "BDF for DAEs, sparse-friendly; large models", options: ["tolerance", "intervals", "start"] },
  { id: "cvode", label: "CVODE (SUNDIALS)", family: "implicit", blurb: "BDF for stiff ODEs; what an exported FMU runs", options: ["tolerance", "intervals", "start"] },
  { id: "gbode", label: "GBODE", family: "adaptive", blurb: "Adaptive Runge–Kutta family, error-controlled", options: ["tolerance", "intervals", "start"] },
  { id: "rungekutta", label: "Runge–Kutta 4", family: "explicit", blurb: "Classic RK4, one step per interval", options: ["intervals", "start"] },
  { id: "heun", label: "Heun (RK2)", family: "explicit", blurb: "Second order, one step per interval", options: ["intervals", "start"] },
  { id: "euler", label: "Euler", family: "explicit", blurb: "First order; for checking, not accuracy", options: ["intervals", "start"] },
  { id: "impeuler", label: "Implicit Euler", family: "implicit", blurb: "First order, stable on stiff systems", options: ["intervals", "start"] },
  { id: "trapezoid", label: "Trapezoid", family: "implicit", blurb: "Second order implicit, no damping of oscillations", options: ["intervals", "start"] },
];

export const OPTIONS: SolverOptionInfo[] = [
  { key: "tolerance", label: "Tolerance", hint: "Relative error per step; smaller is slower and closer.", group: "Accuracy", valid: (v) => v > 0 && v < 1 },
  { key: "intervals", label: "Output intervals", hint: "Samples in the result; fixed-step solvers take one step each.", group: "Steps", whole: true, valid: (v) => v >= 1 && v <= 1_000_000 },
  { key: "start", label: "Start time", unit: "s", hint: "Where the run begins; stop is set in the run dock.", group: "Time", valid: (v) => Number.isFinite(v) },
];

export const DEFAULTS: Record<string, number> = { tolerance: 1e-6, intervals: 500, start: 0 };

const key = (model: string) => `squero-solver:${model}`;

export function loadSolver(model: string): SolverChoice {
  try {
    const v = JSON.parse(localStorage.getItem(key(model)) ?? "null") as SolverChoice | null;
    if (v && SOLVERS.some((s) => s.id === v.method)) return v;
  } catch {
    /* a bad entry is no entry */
  }
  return { method: "dassl" };
}

export function saveSolver(model: string, v: SolverChoice): void {
  localStorage.setItem(key(model), JSON.stringify(v));
}

/** The FMU's solver for an export under `v`: OpenModelica's CS FMUs have CVODE and Euler. */
export const fmuSolver = (v: SolverChoice) => (v.method === "euler" ? "euler" : "cvode");

/** The dock's one-line summary: "DASSL · 1e-6". */
export function summary(v: SolverChoice): string {
  const s = SOLVERS.find((x) => x.id === v.method);
  const t = v.tolerance ?? DEFAULTS.tolerance;
  const tol = s?.options.includes("tolerance") ? ` · ${t < 1e-3 ? t.toExponential() : t}` : "";
  return `${s?.label.split(" ")[0] ?? v.method}${tol}`;
}
