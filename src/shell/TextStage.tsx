// The stage's Text view: the model's whole .mo, editable. It reads the file (not omc's list(),
// which is the class body) so `within` and annotations are exactly what Save writes back.
// It is the shared Studio editor (@paperboat/shared/editor) with Modelica, always editable. A
// message selects its line. The App routes Save and document switches through the
// registered handle.

import { useCallback, useEffect, useRef, useState } from "react";
import { EditorSelection } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { CodeEditor, EditorSettingsKey, useEditorSettings, type ConfigStore } from "@paperboat/shared/editor";
import type { StudioModel } from "../model/types.ts";
import { modelica } from "../split/modelica.ts";
import type { Actions } from "../store/index.tsx";

export type TextEditorHandle = { dirty: boolean; save: () => Promise<void> };

export function TextStage({
  model,
  actions,
  line,
  theme,
  config,
  base,
  onEditor,
}: {
  model: StudioModel;
  actions: Actions;
  line?: number;
  /** The app's shown theme, for the editor's "ui" setting. */
  theme: "light" | "dark";
  /** The user's config file, where the editor settings live. */
  config?: ConfigStore;
  /** HEAD's copy of the file for the git gutter; null when untracked or outside a repository. */
  base?: string | null;
  onEditor?: (handle: TextEditorHandle | null) => void;
}) {
  const [text, setText] = useState<string | null>(null);
  const [saved, setSaved] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [view, setView] = useState<EditorView | null>(null);
  const [settings, setSettings, configFile] = useEditorSettings(config, "om.editor");
  const read = useRef(actions.fileText);
  read.current = actions.fileText;
  const write = useRef(actions.saveText);
  write.current = actions.saveText;
  const editorCb = useRef(onEditor);
  editorCb.current = onEditor;

  useEffect(() => {
    let live = true;
    setText(null);
    setError("");
    read.current().then(
      (value) => {
        if (!live) return;
        setText(value);
        setSaved(value);
      },
      (e: Error) => live && setError(e.message),
    );
    return () => {
      live = false;
    };
  }, [model.file]);

  const dirty = text !== null && text !== saved;
  const save = useCallback(async () => {
    if (text === null || text === saved || busy) return;
    setBusy(true);
    setError("");
    try {
      await write.current(text);
      setSaved(text);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }, [text, saved, busy]);
  const saveRef = useRef(save);
  saveRef.current = save;

  useEffect(() => {
    editorCb.current?.({ dirty, save: () => saveRef.current() });
  }, [dirty]);
  useEffect(
    () => () => {
      editorCb.current?.(null);
    },
    [],
  );

  // Ctrl+S saves the buffer, not the stale omc model: capture it before the app's shortcut.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") {
        e.preventDefault();
        e.stopPropagation();
        void saveRef.current();
      }
    };
    window.addEventListener("keydown", onKey, true);
    return () => window.removeEventListener("keydown", onKey, true);
  }, []);

  // A message's line: select it and bring it to the middle.
  useEffect(() => {
    if (!view || !line) return;
    const doc = view.state.doc;
    const at = doc.line(Math.min(Math.max(line, 1), doc.lines));
    view.dispatch({ selection: EditorSelection.range(at.from, at.to), effects: EditorView.scrollIntoView(at.from, { y: "center" }) });
    view.focus();
  }, [line, view]);

  const name = model.file ? model.file.split(/[\\/]/).pop() : `${model.name}.mo`;
  return (
    <section className="stage-text" aria-label={`${model.name} text`}>
      <header className="code-head">
        <b>{name}</b>
        <span className="lbl">· {model.name}</span>
        <span className="spacer" />
        {dirty && <span className="lbl">unsaved</span>}
        <EditorSettingsKey settings={settings} onSettings={setSettings} file={configFile} />
        {text !== null && (
          <button type="button" className="key small" onClick={() => void navigator.clipboard?.writeText(text)}>
            Copy
          </button>
        )}
        <button type="button" className="key small ink" disabled={!dirty || busy} onClick={() => void save()}>
          {busy ? "saving…" : "Save"}
        </button>
      </header>
      <div className="code-editor">
        {text === null ? (
          <p className="code-reading">{error || "reading…"}</p>
        ) : (
          <CodeEditor
            key={model.file}
            value={text}
            onChange={setText}
            settings={settings}
            appTheme={theme}
            ariaLabel="Modelica source editor"
            language={modelica}
            base={base}
            onView={setView}
          />
        )}
      </div>
      {error && (
        <p className="studio-var-error code-error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}
