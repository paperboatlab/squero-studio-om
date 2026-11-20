// Opens a station in Paperboat's web app and screenshots the Flow page in light and dark.
// usage: node flow-shots.mjs <web url> <station ws> <out dir>
import { chromium, CHROME } from "./lib.mjs";
const [web, station, out] = process.argv.slice(2);
const browser = await chromium.launch({ executablePath: CHROME });
for (const theme of ["light", "dark"]) {
  const page = await browser.newPage({ viewport: { width: 1500, height: 900 } });
  await page.addInitScript((t) => localStorage.setItem("paperboat.theme", t), theme);
  await page.goto(`${web}/?station=${encodeURIComponent(station)}`);
  await page.waitForSelector(".react-flow__node", { timeout: 20000 });
  await page.waitForTimeout(2500);
  await page.screenshot({ path: `${out}/flow-${theme}.png` });
  console.log(theme, await page.locator(".react-flow__node").evaluateAll((n) => n.map((x) => x.dataset.id)));
}
await browser.close();
