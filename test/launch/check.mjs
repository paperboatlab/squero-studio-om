// A running squero-om, as `paperboat squero start squero-studio-om` leaves it: the page is served, and
// squero/0 reaches omc (its version, then the Modelica Standard Library loads). CI's launch job runs
// it; Node 22's own WebSocket, so no dependency.
//   node test/launch/check.mjs [port]      (59201 by default)
const port = Number(process.argv[2] ?? 59201);
const fail = (why) => { console.error(`FAIL ${why}`); process.exit(1); };
setTimeout(() => fail("timed out"), 600000).unref();

const page = await fetch(`http://127.0.0.1:${port}/`).then((r) => r.text()).catch((e) => fail(`no page: ${e.message}`));
if (!page.includes('id="root"')) fail("the page is not the app");
console.log("page served");

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
const pending = new Map();
ws.addEventListener("message", (event) => {
  const m = JSON.parse(String(event.data));
  if (m.id !== undefined && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
});
ws.addEventListener("close", () => fail("the server closed the socket"));
await new Promise((resolve, reject) => { ws.addEventListener("open", resolve); ws.addEventListener("error", () => reject(new Error("no socket"))); })
  .catch((e) => fail(e.message));
const ask = (id, body) => new Promise((resolve) => { pending.set(id, resolve); ws.send(JSON.stringify({ id, ...body })); });

const version = await ask(1, { op: "omc", expr: "getVersion()" });
if (!version.ok || !String(version.result).includes("OpenModelica")) fail(`getVersion: ${JSON.stringify(version)}`);
console.log(`omc: ${version.result}`);
const msl = await ask(2, { op: "omc", expr: "loadModel(Modelica)" });
if (!msl.ok || msl.result !== "true") fail(`loadModel(Modelica): ${JSON.stringify(msl)}`);
console.log("Modelica Standard Library loaded");
process.exit(0);
