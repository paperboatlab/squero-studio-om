// The Variables sheet (Alt+V): OMEdit's variable browser, plus a Paperboat tab that turns the
// model's own top-level parameters into Station properties the runtime can set. Icon and port
// drawing lives on the canvas (View ▸ Icon), not here.

import { useEffect, useState } from "react";
import { Switch } from "@paperboat/shared/components";
import { useStudio } from "../model/types.ts";
import { defaultExportFolder } from "../dock/dock.ts";
import { PaperboatBlock } from "./PaperboatBlock.tsx";
import { VariablesTree } from "./VariablesTree.tsx";
import "./sheet.css";

export type InterfaceSheetProps = { open: boolean; onOpen: () => void; onClose: () => void; canExport: boolean };

type SheetTab = "interface" | "paperboat";

function initialTabFromUrl(): SheetTab {
  if (typeof window !== "undefined") {
    const q = new URLSearchParams(window.location.search).get("sheetTab");
    if (q === "paperboat") return "paperboat";
  }
  return "interface";
}

function RemoveGlyph() {
  return (
    <svg viewBox="0 0 10 10" aria-hidden="true">
      <path d="M2 2l6 6M8 2L2 8" />
    </svg>
  );
}

export function InterfaceSheet({ open, onOpen, onClose, canExport }: InterfaceSheetProps) {
  const api = useStudio();
  const [tab, setTab] = useState<SheetTab>(initialTabFromUrl);
  const inputs = api.model.entries.filter((e) => e.kind === "input");
  const outputs = api.model.entries.filter((e) => e.kind === "output");
  const shortModel = api.model.name.split(".").pop() ?? api.model.name;
  const folderHint = `${defaultExportFolder(shortModel)}/`;

  useEffect(() => {
    if (!canExport && tab === "paperboat") setTab("interface");
  }, [canExport, tab]);

  return (
    <aside className={`studio-sheet${open ? " open" : ""}`} aria-label="Variables and Paperboat" data-own-keys>
      {/* Folded: a standing tab at the right edge, as Paperboat's Variables sheet. */}
      <button type="button" className="studio-sheet-tab" hidden={open} onClick={onOpen} title="Open the Variables (Alt+V)" aria-expanded={false}>
        <svg viewBox="0 0 14 14" aria-hidden="true">
          <path d="M9 3L5 7l4 4" />
        </svg>
        <span className="studio-sheet-glyph" aria-hidden="true">=</span>
        <span className="studio-sheet-tab-name">Variables</span>
        <span className="lbl">{api.model.vars?.length ?? api.model.params.length}</span>
      </button>
      <header className="studio-sheet-head" hidden={!open}>
        <h2 className="studio-sheet-title">{shortModel}</h2>
        <Switch<SheetTab>
          label="Tab"
          value={tab}
          onChange={setTab}
          options={[
            { value: "interface", label: "variables", hint: "Every top-level variable, then each component's parameters" },
            ...(canExport ? [{ value: "paperboat" as const, label: "paperboat", hint: "Parameters passed to the model at run time, and the linked block" }] : []),
          ]}
        />
        <button type="button" className="studio-look-key studio-sheet-fold" title="Fold (Alt+V or Esc)" aria-label="Fold the sheet" aria-expanded onClick={onClose}>
          <svg viewBox="0 0 14 14" aria-hidden="true">
            <path d="M5 3l4 4-4 4" />
          </svg>
        </button>
      </header>

      <div className="studio-sheet-body" hidden={!open}>
        {tab === "interface" ? (
          <>
            <VariablesTree model={api.model} mode="edit" />

            <section className="studio-sheet-section" aria-label="Entry points">
              <div className="studio-sheet-subhead">Entry points</div>
              <div className="studio-entries-grid">
                <div className="studio-entries-col">
                  <span className="studio-entries-col-head">Inputs</span>
                  {inputs.length === 0 ? (
                    <span className="studio-prop-unit">none</span>
                  ) : (
                    inputs.map((entry) => (
                      <div key={entry.name} className="studio-entry-pill">
                        <span className="studio-entry-tag">in</span>
                        <span className="studio-entry-name" title={entry.target}>
                          {entry.name}
                        </span>
                        {entry.unit && <span className="studio-prop-unit">{entry.unit}</span>}
                        <button type="button" className="studio-remove" title={`Remove ${entry.name}`} onClick={() => void api.removeEntry(entry.name)}>
                          <RemoveGlyph />
                        </button>
                      </div>
                    ))
                  )}
                </div>
                <div className="studio-entries-col">
                  <span className="studio-entries-col-head">Outputs</span>
                  {outputs.length === 0 ? (
                    <span className="studio-prop-unit">none</span>
                  ) : (
                    outputs.map((entry) => (
                      <div key={entry.name} className="studio-entry-pill">
                        <span className="studio-entry-tag">out</span>
                        <span className="studio-entry-name" title={entry.target}>
                          {entry.name}
                        </span>
                        {entry.unit && <span className="studio-prop-unit">{entry.unit}</span>}
                        <button type="button" className="studio-remove" title={`Remove ${entry.name}`} onClick={() => void api.removeEntry(entry.name)}>
                          <RemoveGlyph />
                        </button>
                      </div>
                    ))
                  )}
                </div>
              </div>
            </section>
          </>
        ) : (
          <>
            <section className="studio-sheet-section" aria-label="Parameter passthrough">
              <div className="studio-sheet-subhead">Parameters passed to Paperboat</div>
              <VariablesTree model={api.model} mode="passthrough" />
              <p className="studio-handoff-note">
                A <b>passthrough</b> parameter becomes a Station property; Paperboat sets it when the model starts. Everything else stays
                inside the model. The icon and the connector pins are drawn on the canvas: <b>View ▸ Icon</b>.
              </p>
            </section>

            <section className="studio-sheet-section" aria-label="Paperboat block">
              <div className="studio-sheet-subhead">Paperboat block</div>
              <PaperboatBlock />
              <p className="studio-handoff-note">
                Paperboat opens <code>{folderHint}</code> via File › Station… The Modelica icon is drawn in <b>View ▸ Icon</b>.
              </p>
            </section>
          </>
        )}
      </div>
    </aside>
  );
}
