import assert from "node:assert/strict";
import { test } from "node:test";
import {
  checkModelBalance,
  defaultExportFolder,
  findDanglingFlowPorts,
  fixDanglingPort,
  parseBalance,
  runStationExport,
  undrawnNote,
} from "../src/dock/dock.ts";
import type { ExportProgress } from "../src/model/types.ts";
import { createDefaultModel, createFakeStudio, DEFAULT_CONNECTORS } from "./fake.ts";

test("dock: balance parse handles balanced, under-determined, over-determined, and omc error", async () => {
  const api = createFakeStudio();

  // Balanced (default scripted checkModel reply: 14 equation(s) and 14 variable(s))
  const bal = await checkModelBalance(api);
  assert.equal(bal.kind, "balanced");
  assert.equal(bal.equations, 14);
  assert.equal(bal.unknowns, 14);
  assert.match(bal.words, /balanced.*14 \/ 14/);

  // Under-determined
  api.setCheckModelReply("Class RLCircuit has 13 equation(s) and 14 variable(s).");
  const under = await checkModelBalance(api);
  assert.equal(under.kind, "under");
  assert.equal(under.equations, 13);
  assert.equal(under.unknowns, 14);
  assert.equal(under.diff, 1);
  assert.match(under.words, /13 \/ 14.*fewer/);

  // Over-determined
  api.setCheckModelReply("Class RLCircuit has 16 equations and 14 variables.");
  const over = await checkModelBalance(api);
  assert.equal(over.kind, "over");
  assert.equal(over.equations, 16);
  assert.equal(over.unknowns, 14);
  assert.equal(over.diff, 2);
  assert.match(over.words, /16 \/ 14.*more/);

  // A fixed state adds an initial equation the counts leave out: checkModel is the judge.
  const initialised = parseBalance("Check of Basics.Loop completed successfully.\nClass Basics.Loop has 10 equation(s) and 9 variable(s).\n7 of these are trivial equation(s).");
  assert.equal(initialised.kind, "balanced");
  assert.equal(initialised.equations, 10);
  assert.equal(initialised.unknowns, 9);
  assert.match(initialised.words, /10 \/ 9.*checkModel passed/);

  // OMC Error rejection
  api.setCheckModelReply(new Error("omc session timed out"));
  const errReject = await checkModelBalance(api);
  assert.equal(errReject.kind, "error");
  assert.match(errReject.words, /omc session timed out/);

  // Malformed / error string
  const errStr = parseBalance("Error: Class MissingModel not found in scope.");
  assert.equal(errStr.kind, "error");
  assert.match(errStr.words, /MissingModel not found/);
});

test("dock: undrawn declarations are worded, and nothing missing means no note", () => {
  assert.equal(undrawnNote(undefined), null);
  assert.equal(undrawnNote({ total: 0, hidden: 0, arrays: 0, conditionals: 0 }), null);

  const one = undrawnNote({ total: 1, hidden: 1, arrays: 0, conditionals: 0 });
  assert.equal(one?.label, "1 not drawn as declared");
  assert.match(one?.title ?? "", /1 protected part is hidden/);

  const all = undrawnNote({ total: 4, hidden: 2, arrays: 1, conditionals: 1 });
  assert.equal(all?.label, "4 not drawn as declared");
  assert.match(all?.title ?? "", /2 protected parts are hidden/);
  assert.match(all?.title ?? "", /1 array is drawn as one block/);
  assert.match(all?.title ?? "", /1 conditional part is drawn without its condition/);
});

test("dock: dangling flow connector detection and fix choice per domain", async () => {
  // 1. Default model: R1.p and R1.n->L1.p are wired; L1.n is the only unconnected flow port (electrical)
  const api = createFakeStudio();
  const initialDangling = findDanglingFlowPorts(api.model);
  assert.equal(initialDangling.length, 1);
  assert.equal(initialDangling[0].portPath, "L1.n");
  assert.equal(initialDangling[0].label, "L1.n unconnected");
  assert.equal(initialDangling[0].domain, "electrical");
  assert.equal(initialDangling[0].fix?.className, "Modelica.Electrical.Analog.Basic.Ground");
  assert.equal(initialDangling[0].fix?.buttonLabel, "Fix: Ground");

  // Fixing L1.n adds Ground G1 and connects L1.n -> G1.p
  const fixErr = await fixDanglingPort(api, initialDangling[0]);
  assert.equal(fixErr, null);
  assert.deepEqual(api.calls.slice(-2).map((c) => [c.method, c.args[0], c.args[1]]), [
    ["addComponent", "Modelica.Electrical.Analog.Basic.Ground", "G1"],
    ["connect", "L1.n", "G1.p"],
  ]);
  assert.equal(findDanglingFlowPorts(api.model).length, 0, "no dangling ports after Fix: Ground");

  // 2. Translational, Rotational, and Thermal/Fluid domains
  const multiModel = createDefaultModel();
  multiModel.connections = [
    { a: "u_in.y", b: "R1.p" },
    { a: "R1.n", b: "L1.p" },
    { a: "L1.n", b: "R1.p" },
  ];
  multiModel.connectors = {
    ...DEFAULT_CONNECTORS,
    TranslationalFlange: {
      kind: "acausal",
      handle: "square",
      size: 10,
      weight: 1.8,
      potential: [{ id: "s", unit: "m" }],
      flow: [{ id: "f", unit: "N" }],
    },
  };
  multiModel.components.push(
    {
      name: "M1",
      class: "Modelica.Mechanics.Translational.Components.Mass",
      params: { m: "1" },
      placement: { x: 100, y: 200, rotation: 0 },
      ports: [{ id: "flange_a", class: "TranslationalFlange", side: "left", at: 50 }],
    },
    {
      name: "J1",
      class: "Modelica.Mechanics.Rotational.Components.Inertia",
      params: { J: "0.1" },
      placement: { x: 200, y: 200, rotation: 0 },
      ports: [{ id: "flange_b", class: "Flange_b", side: "right", at: 50 }],
    },
    {
      name: "H1",
      class: "Modelica.Thermal.HeatTransfer.Components.HeatCapacitor",
      params: { C: "100" },
      placement: { x: 300, y: 200, rotation: 0 },
      ports: [{ id: "port", class: "HeatPort_a", side: "bottom", at: 50 }],
    },
  );

  const api2 = createFakeStudio({ model: multiModel });
  const list = findDanglingFlowPorts(api2.model);
  assert.equal(list.length, 3);

  const trans = list.find((d) => d.component === "M1")!;
  assert.equal(trans.domain, "translational");
  assert.equal(trans.fix?.className, "Modelica.Mechanics.Translational.Components.Fixed");
  assert.equal(trans.fix?.buttonLabel, "Fix: Fixed");
  await fixDanglingPort(api2, trans);
  assert.deepEqual(api2.calls.slice(-2).map((c) => [c.method, c.args[0], c.args[1]]), [
    ["addComponent", "Modelica.Mechanics.Translational.Components.Fixed", "fixed1"],
    ["connect", "M1.flange_a", "fixed1.flange"],
  ]);

  const rot = list.find((d) => d.component === "J1")!;
  assert.equal(rot.domain, "rotational");
  assert.equal(rot.fix?.className, "Modelica.Mechanics.Rotational.Components.Fixed");
  assert.equal(rot.fix?.buttonLabel, "Fix: Fixed");
  await fixDanglingPort(api2, rot);
  assert.deepEqual(api2.calls.slice(-2).map((c) => [c.method, c.args[0], c.args[1]]), [
    ["addComponent", "Modelica.Mechanics.Rotational.Components.Fixed", "fixed2"],
    ["connect", "J1.flange_b", "fixed2.flange"],
  ]);

  const thermal = list.find((d) => d.component === "H1")!;
  assert.equal(thermal.domain, "other");
  assert.equal(thermal.fix, null, "other domains show no fix button");
});

test("dock: exportStation streams progress sequence and formats result or error", async () => {
  const api = createFakeStudio();
  assert.equal(defaultExportFolder(api.model.name), "rl-circuit-cpp");

  const seen: ExportProgress[] = [];
  const outcome = await runStationExport(api, "pst", (p) => seen.push(p));

  assert.equal(outcome.ok, true);
  assert.deepEqual(
    seen.map((s) => [s.stage, s.fraction]),
    [
      ["check", 0.25],
      ["flatten", 0.5],
      ["generate", 0.75],
      ["build", 1.0],
    ],
  );
  assert.match(outcome.line, /downloaded · no events/);

  // Error path
  api.setExportScript([{ stage: "check", fraction: 0.2 }], new Error("DAE has structural singularity"));
  const failed = await runStationExport(api, "serve", () => {});
  assert.equal(failed.ok, false);
  assert.equal(failed.line, "DAE has structural singularity");

  // Start: building until the port opens; a station that exits is an error carrying its log
  api.setExportScript([], { name: "ssom_RLCircuit", port: 4100, state: "building" });
  assert.match((await runStationExport(api, "serve", () => {})).line, /^building ssom_RLCircuit for port 4100/);
  api.setExportScript([], { port: 4100, state: "running" });
  assert.match((await runStationExport(api, "status", () => {}, 4100)).line, /running on port 4100/);
  api.setExportScript([], { port: 4100, state: "exited", log: "cmake build failed\n" });
  const exited = await runStationExport(api, "status", () => {}, 4100);
  assert.equal(exited.ok, false);
  assert.match(exited.line, /exited:\ncmake build failed/);
});

test("export folder follows the generator's station_name", () => {
  assert.equal(defaultExportFolder("DCMotor"), "dc-motor-cpp");
  assert.equal(defaultExportFolder("Lib.Pkg.CartPole"), "cart-pole-cpp");
  assert.equal(defaultExportFolder("my_model2"), "my-model2-cpp");
});

test("omc's type-incompatible connect is explained in plain words", async () => {
  const { explainOmcError } = await import("../src/dock/dock.ts");
  assert.match(explainOmcError("The connectors in connect(R1.n, n) are not type compatible."), /^R1\.n and n can't be wired: .*sensor/);
  assert.match(explainOmcError("simulation failed: The connectors in connect(m.R1.n, m.n) are not type compatible."), /^R1\.n and n can't/);
  assert.equal(explainOmcError("something else"), "something else");
});
