import assert from "node:assert/strict";
import test from "node:test";
import { addDecoration, addTab, moveInstrument, moveToTab, resizeInstrument, setSettings } from "@paperboat/shared";
import { addScope, arrangeScopes, picksOf, removeScope, START } from "../src/simulate/scopes.ts";

test("scopes: added in order on a two-column grid, resized, moved, removed", () => {
  let l = addScope(addScope(START, ""), ""); // Scope 1 (L), Scope 2, Scope 3
  let a = arrangeScopes(l);
  assert.deepEqual(a.instruments.map((p) => [p.block, p.col, p.row, p.w, p.h]), [
    ["Scope 1", 0, 0, 2, 2],
    ["Scope 2", 0, 2, 2, 1],
    ["Scope 3", 0, 3, 2, 1],
  ]);
  l = resizeInstrument(a, "Scope 2", "compact");
  l = moveInstrument(arrangeScopes(l), "Scope 3", 1, 2);
  a = arrangeScopes(l);
  // what it lands on makes way: Scope 3 (M) takes row 2, the S that was there goes below
  assert.deepEqual(a.instruments.find((p) => p.block === "Scope 3"), { block: "Scope 3", size: "normal", col: 0, row: 2, w: 2, h: 1, tab: "" });
  assert.deepEqual(a.instruments.find((p) => p.block === "Scope 2")!.row, 3);
  assert.deepEqual(arrangeScopes(removeScope(l, "Scope 2")).instruments.map((p) => p.block).sort(), ["Scope 1", "Scope 3"]);
  assert.equal(addScope(removeScope(l, "Scope 2"), "").instruments!.at(-1)!.block, "Scope 2"); // the free name
});

test("scopes: channels live in the instrument's settings; tabs and labels are the board's", () => {
  let a = arrangeScopes(START);
  a = arrangeScopes(setSettings(a, "Scope 1", { picks: ["x", "y"] }));
  assert.deepEqual(picksOf(a.instruments[0]), ["x", "y"]);
  a = arrangeScopes(addTab(a, "Currents"));
  assert.deepEqual(a.tabs, ["Main", "Currents"]);
  a = arrangeScopes(moveToTab(a, "Scope 1", "Currents"));
  a = arrangeScopes(addDecoration(a, "label", "Main"));
  assert.equal(a.instruments[0].tab, "Currents");
  assert.deepEqual(a.decorations.map((d) => [d.kind, d.tab]), [["label", "Main"]]);
  assert.deepEqual(picksOf(a.instruments[0]), ["x", "y"]); // the settings travel with it
});
