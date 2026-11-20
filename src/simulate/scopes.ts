// The Simulate pane's scopes (#7) on Paperboat's board grid: each scope is an instrument of
// the shared layout engine, its channels kept in the instrument's settings; labels, separators,
// sections and tabs are the engine's decorations and tabs. Pure, so a test can drive it.

import { arrange, type Arranged, type BoardLayout, type PlacedLayout } from "@paperboat/shared";

/** The side pane is narrow: two columns, a scope is a half (S), a row (M) or a square (L). */
export const COLUMNS = 2;

export const START: BoardLayout = { columns: COLUMNS, instruments: [{ block: "Scope 1", size: "large" }] };

/** The layout laid out: a scope is a monitor, in the order it was added. */
export function arrangeScopes(layout: BoardLayout): Arranged {
  const monitors = Object.fromEntries((layout.instruments ?? []).map((i, n) => [i.block, { kind: "scope", x: n, y: 0 }]));
  return arrange(monitors, layout, layout.columns ?? COLUMNS);
}

/** A new scope, at the end of `tab` ("" with no tabs), named Scope N with N free. */
export function addScope(layout: BoardLayout, tab: string): BoardLayout {
  const taken = new Set((layout.instruments ?? []).map((i) => i.block));
  let n = 1;
  while (taken.has(`Scope ${n}`)) n++;
  return { ...layout, instruments: [...(layout.instruments ?? []), { block: `Scope ${n}`, size: "normal", ...(tab ? { tab } : {}) }] };
}

export function removeScope(layout: BoardLayout, block: string): BoardLayout {
  return { ...layout, instruments: (layout.instruments ?? []).filter((i) => i.block !== block) };
}

/** The channels a scope plots. */
export const picksOf = (p: Pick<PlacedLayout, "settings">): string[] => (Array.isArray(p.settings?.picks) ? (p.settings.picks as string[]) : []);
