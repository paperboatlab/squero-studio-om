// OM's side of the shared Help window: a few guide pages, the canvas gestures, and every
// Modelica class omc has loaded, with its documentation annotation (Documentation(info=…)).
import { CANVAS_GESTURES } from "@paperboat/shared/components";
import { matches, type HelpItem, type HelpPage, type HelpProvider } from "@paperboat/shared";
import { atom, list, parseValue } from "../omc/parse.ts";
import type { OmcCalls } from "../omc/omc.ts";
import { flat, search } from "../library/classes.ts";

const GUIDES: (HelpItem & { text: string })[] = [
  {
    id: "guide:start", title: "Getting started", kind: "guide", summary: "models, the library, check and simulate",
    text: `Squero Studio OM edits Modelica models with OpenModelica (omc) behind it.

- Open a \`.mo\` file from the files rail, or File › New Model.
- Drag parts from the library onto the canvas and wire their ports; the source follows every edit.
- The dock under the canvas counts equations and unknowns; Check asks omc to check the model.
- The side pane shows the source and the scopes; Run › Simulate runs the model and plots it.`,
  },
  {
    id: "guide:canvas", title: "On the canvas", kind: "guide", summary: "gestures and keys",
    text: CANVAS_GESTURES.map(([what, how]) => `- ${what}: ${how}`).join("\n"),
  },
  {
    id: "guide:docs", title: "Class documentation", kind: "guide", summary: "F1 on a part, or type a class name",
    text: `Select a part on the canvas and press \`F1\` to read its class's documentation.

Type a class name (\`Resistor\`) or a full path (\`Modelica.Blocks.Continuous.PID\`) to look one up. The pages are the library's own Documentation annotations.`,
  },
];

const CLASS = "class:";
const NAME = /^[A-Za-z_][\w]*(\.[A-Za-z_][\w]*)*$/;

/** The Help provider: guides first, then loaded classes found by short name or path. */
export function omHelp(omc: OmcCalls): HelpProvider {
  let names: Promise<string[]> | null = null;
  const loadNames = () => {
    names ??= (async () => {
      const tops = list(parseValue(await omc.call("getClassNames()"))).map(atom);
      return (await Promise.all(tops.map((t) => flat(omc, t).catch(() => [] as string[])))).flat();
    })().catch(() => { names = null; return []; });
    return names;
  };
  return {
    async items(query) {
      const guides = GUIDES.filter((g) => matches(g, query));
      if (!query) return guides;
      const hits = search(await loadNames(), query, 200);
      const items: HelpItem[] = hits.map((full) => ({ id: CLASS + full, title: full.slice(full.lastIndexOf(".") + 1), kind: "class", summary: full }));
      if (NAME.test(query) && query.includes(".") && !hits.includes(query)) items.unshift({ id: CLASS + query, title: query, kind: "look up" });
      return [...guides, ...items];
    },
    // modelica://Modelica.Blocks.Types.Init (a class, maybe with a #section) opens that class here.
    link: (href) => /^modelica:\/\/([\w.]+)/i.exec(href)?.[1] ?? null,
    async page(id): Promise<HelpPage | null> {
      const guide = GUIDES.find((g) => g.id === id);
      if (guide) return { title: guide.title, kind: "guide", text: guide.text };
      const cls = id.slice(CLASS.length);
      if (!NAME.test(cls)) return null;
      const [comment, docs, restriction] = await Promise.all([
        omc.call(`getClassComment(${cls})`).then((r) => atom(parseValue(r))).catch(() => ""),
        omc.call(`getDocumentationAnnotation(${cls})`).then((r) => list(parseValue(r)).map(atom)).catch(() => [] as string[]),
        omc.call(`getClassRestriction(${cls})`).then((r) => atom(parseValue(r))).catch(() => ""),
      ]);
      const [info = "", revisions = ""] = docs;
      if (!comment && !info && !restriction) return null;
      const html = [comment && `<p><em>${comment.replace(/&/g, "&amp;").replace(/</g, "&lt;")}</em></p>`, info, revisions && `<h3>Revisions</h3>${revisions}`].filter(Boolean).join("\n");
      return { title: cls, kind: restriction || "class", ...(html ? { html } : { text: "No documentation in this class." }) };
    },
  };
}
