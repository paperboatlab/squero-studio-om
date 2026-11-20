// Dock: Export station. Writes next to the server's working dir (reset.sh uses $WORK).
import { open } from "./lib.mjs";
const { browser, page } = await open();
await page.waitForTimeout(2500);
const dock = page.getByLabel("Model balance and export");
console.log(await dock.getByLabel("Equation and unknown balance").innerText());
await dock.getByLabel("Export station folder").fill(process.argv[2] ?? "dc-drive-cpp");
await dock.getByRole("button", { name: "Export station" }).click();
await page.waitForTimeout(15000);
console.log(await dock.innerText());
await page.screenshot({ path: "/tmp/export.png" });
await browser.close();
