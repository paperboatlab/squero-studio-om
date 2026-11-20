// The split view (contracts §8.2): the generated artifacts beside the canvas. `flat` is
// instantiateModel's text, `causal ODE c++` the generated station from a dry-run generate; both
// read-only. The model's own .mo is the stage's Text view, not a pane here.

import { useEffect, useRef, useState } from "react";
import type { StudioModel } from "../model/types.ts";
import type { Actions } from "../store/index.tsx";
import { CodeView } from "./CodeView.tsx";
import type { EditorSettings } from "@paperboat/shared/editor";

export type View = "flat" | "cpp";
const OPTIONS: { value: View; label: string }[] = [
  { value: "flat", label: "flat" },
  { value: "cpp", label: "causal ODE c++" },
];

type Preview = Awaited<ReturnType<Actions["preview"]>>;

/** Runs `load` after the model settles; the text it returns, "…" while it runs, the words it failed with. */
function useLater<T>(model: StudioModel, load: () => Promise<T>, on: boolean): { value: T | null; error: string; busy: boolean } {
  const [state, setState] = useState<{ value: T | null; error: string; busy: boolean }>({ value: null, error: "", busy: false });
  const latest = useRef(load);
  latest.current = load;
  useEffect(() => {
    if (!on) return;
    let live = true;
    setState((s) => ({ ...s, busy: true }));
    const timer = window.setTimeout(() => {
      latest.current().then(
        (value) => live && setState({ value, error: "", busy: false }),
        (e: Error) => live && setState({ value: null, error: e.message, busy: false }),
      );
    }, 350);
    return () => {
      live = false;
      window.clearTimeout(timer);
    };
  }, [model, on]);
  return state;
}

export function Split({
  model,
  actions,
  view,
  onView,
  settings,
  appTheme,
}: {
  model: StudioModel;
  actions: Actions;
  view: View;
  onView: (v: View) => void;
  /** The editor settings, so the read-only views read like the text editor. */
  settings: EditorSettings;
  appTheme: "light" | "dark";
}) {
  const flat = useLater<string>(model, actions.flatModel, view === "flat");
  const cpp = useLater<Preview>(model, actions.preview, view === "cpp");
  const [file, setFile] = useState("");
  const names = Object.keys(cpp.value?.files ?? {});
  const shown = names.includes(file) ? file : (names.find((n) => n.endsWith(".cpp") && n.startsWith("domain/")) ?? names[0] ?? "");

  return (
    <section className="split" aria-label="Source">
      <header>
        <div className="switch" role="radiogroup" aria-label="Source view">
          {OPTIONS.map((o) => (
            <button key={o.value} role="radio" aria-checked={o.value === view} className={`opt${o.value === view ? " on" : ""}`} onClick={() => onView(o.value)}>
              {o.label}
            </button>
          ))}
        </div>
      </header>
      {view === "flat" ? (
        <>
          <p className="code-about" role="status">
            <b>flat model</b> · instantiateModel({model.name}) · read-only
            {flat.value !== null && (
              <button
                type="button"
                className="key small"
                style={{ marginLeft: "auto" }}
                onClick={() => void navigator.clipboard?.writeText(flat.value ?? "")}
              >
                Copy
              </button>
            )}
          </p>
          <CodeView code={flat.value ?? (flat.error || "flattening…")} lang="mo" ariaLabel="Flat Modelica" settings={settings} appTheme={appTheme} />
        </>
      ) : (
        <>
          <p className="code-about" role="status">
            {cpp.error ? cpp.error : cpp.value ? <><b>{cpp.value.path}</b> · {cpp.value.reason}</> : "checking…"}
          </p>
          {names.length > 1 && (
            <div className="files" role="tablist" aria-label="Generated files">
              {names.map((n) => (
                <button key={n} role="tab" aria-selected={n === shown} className={n === shown ? "on" : undefined} onClick={() => setFile(n)}>
                  {n}
                </button>
              ))}
            </div>
          )}
          <CodeView
            code={shown ? (cpp.value?.files[shown] ?? "") : cpp.value && !names.length ? (cpp.value.notices[0] ?? "") : ""}
            lang="cpp"
            ariaLabel="Generated C++"
            settings={settings}
            appTheme={appTheme}
          />
        </>
      )}
    </section>
  );
}
