// The Paperboat block, edited as a block: one plate, its name, and the input/output pins. The
// Modelica icon is a separate thing (View ▸ Icon); this is what Paperboat shows when it links the
// model as a block.

import { useEffect, useMemo, useState } from "react";
import { ReactFlowProvider } from "@xyflow/react";
import { CompactBlock, NumberField } from "@paperboat/shared/components";
import type { IconPort } from "@paperboat/shared";
import { useStudio } from "../model/types.ts";
import { buildPaperboatBlockDef, snapTo5, syncPortsWithEntries } from "./look.ts";
import { SideSelect } from "./SideSelect.tsx";

function portsOf(model: ReturnType<typeof useStudio>["model"]): IconPort[] {
  const ids = new Set(model.entries.map((e) => e.name));
  return (syncPortsWithEntries(model.icon, model.entries).ports ?? []).filter((p) => ids.has(p.id));
}

export function PaperboatBlock() {
  const api = useStudio();
  const [ports, setPorts] = useState<IconPort[]>(() => portsOf(api.model));
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    setPorts(portsOf(api.model));
    setSaved(false);
  }, [api.model.icon, api.model.entries]);

  const original = useMemo(() => portsOf(api.model), [api.model.icon, api.model.entries]);
  const dirty = JSON.stringify(ports) !== JSON.stringify(original);
  const def = buildPaperboatBlockDef(api.model, ports);
  const update = (id: string, side: IconPort["side"], at: number) =>
    setPorts((prev) => prev.map((p) => (p.id === id ? { ...p, side, at: snapTo5(at) } : p)));

  const save = async () => {
    setError("");
    try {
      await api.setIcon({ ...(api.model.icon ?? { w: 1, h: 1, g: [] }), ports });
      setSaved(true);
      setTimeout(() => setSaved(false), 1200);
    } catch (e) {
      setError((e as Error).message);
    }
  };

  return (
    <div className="paperboat-block">
      <div className="paperboat-block-preview">
        <ReactFlowProvider>
          <CompactBlock id="paperboat-block" def={def} params={{}} selected={false} connectors={api.model.connectors} />
        </ReactFlowProvider>
      </div>
      <div className="paperboat-block-ports">
        {ports.length === 0 ? (
          <p className="studio-handoff-note">No pins yet. Add an input or output in the Variables tab; then place it here.</p>
        ) : (
          ports.map((port) => {
            const entry = api.model.entries.find((e) => e.name === port.id);
            return (
              <div key={port.id} className="paperboat-block-port">
                <span className="paperboat-block-name">{port.id}</span>
                <span className="var-tag">{entry?.kind ?? "pin"}</span>
                <SideSelect label={`${port.id} side`} value={port.side} onChange={(side) => update(port.id, side, port.at)} />
                <NumberField className="studio-look-at-input" aria-label={`${port.id} at`} value={port.at} onChange={(at) => update(port.id, port.side, at)} />
              </div>
            );
          })
        )}
      </div>
      <div className="paperboat-block-foot">
        <span className="studio-prop-unit">{saved ? "Saved pin placement" : dirty ? "Unsaved pin placement" : "Pins match the model"}</span>
        <button type="button" className="studio-save-key" disabled={!dirty} onClick={() => void save()}>
          Save pins
        </button>
      </div>
      {error && <p className="studio-var-error" role="alert">{error}</p>}
    </div>
  );
}
