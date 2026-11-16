// The stage's Icon view: the model's own look, drawn on the canvas like OMEdit's icon editor.
// Select and drag to move; pick a tool and drag to draw; the corner handles resize; the inspector
// holds the design tokens, the layers and the connector pins. Saving writes Icon(graphics = …).

import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import { ReactFlowProvider } from "@xyflow/react";
import {
  CORNER,
  DASH,
  FILL,
  FONT,
  STROKE,
  WEIGHT,
  type Corner,
  type Dash,
  type Fill,
  type IconDef,
  type IconPort,
  type Prim,
  type Pt,
  type Stroke,
  type Weight,
} from "@paperboat/shared";
import { CompactBlock, NumberField, Switch } from "@paperboat/shared/components";
import { useStudio } from "../model/types.ts";
import { SideSelect } from "../sheet/SideSelect.tsx";
import {
  applyPlateSize,
  buildLinkedBlockDef,
  CORNER_OPTIONS,
  DASH_OPTIONS,
  FONT_OPTIONS,
  PLATE_SIZES,
  plateSizeKey,
  movePrimBy,
  primBounds,
  primFromDrag,
  removePrimAt,
  resizePrim,
  sanitizeIconDef,
  setPortPlacement,
  SIZE_OPTIONS,
  snapTo5,
  syncPortsWithEntries,
  updatePrimTokens,
  WEIGHT_OPTIONS,
  type PlateSizeKey,
  type PrimStyle,
} from "../sheet/look.ts";
import "../sheet/sheet.css";

type Tool = "select" | "line" | "poly" | "rect" | "ellipse" | "arc" | "text" | "pin";
const TOOLS: { tool: Tool; label: string; hint: string }[] = [
  { tool: "select", label: "select", hint: "Click a shape, drag to move, drag a corner to resize" },
  { tool: "line", label: "line", hint: "Drag a line" },
  { tool: "rect", label: "rect", hint: "Drag a rectangle" },
  { tool: "ellipse", label: "ellipse", hint: "Drag an ellipse" },
  { tool: "poly", label: "polygon", hint: "Drag a triangle, then edit it in the text" },
  { tool: "arc", label: "arc", hint: "Drag an arc" },
  { tool: "text", label: "text", hint: "Click to add text" },
  { tool: "pin", label: "pin", hint: "Click an edge to place the selected connector" },
];

type Drag =
  | { kind: "create"; tool: Exclude<Tool, "select" | "pin">; start: Pt; style: PrimStyle }
  | { kind: "move"; index: number; start: Pt; orig: Prim }
  | { kind: "resize"; index: number; corner: "nw" | "ne" | "sw" | "se"; orig: Prim };

const GRID_STEPS = Array.from({ length: 21 }, (_, i) => i * 5);
const HANDLES: ("nw" | "ne" | "sw" | "se")[] = ["nw", "ne", "sw", "se"];

/** The tool buttons are icons, not words: the canvas is the subject. */
function ToolIcon({ tool }: { tool: Tool }) {
  const common = { viewBox: "0 0 16 16", "aria-hidden": true, fill: "none", stroke: "currentColor", strokeWidth: 1.35, strokeLinecap: "round", strokeLinejoin: "round" } as const;
  switch (tool) {
    case "select":
      return <svg {...common}><path d="M3.5 2.5 8 13l1.4-4.2L13.5 7z" /></svg>;
    case "line":
      return <svg {...common}><path d="M3 13 13 3" /></svg>;
    case "rect":
      return <svg {...common}><rect x="2.5" y="3.5" width="11" height="9" rx="1" /></svg>;
    case "ellipse":
      return <svg {...common}><ellipse cx="8" cy="8" rx="5.5" ry="4.5" /></svg>;
    case "poly":
      return <svg {...common}><path d="M8 2.5 14 13.5H2z" /></svg>;
    case "arc":
      return <svg {...common}><path d="M2.5 11a5.5 5.5 0 0 1 11 0" /><path d="M2.5 13h11" opacity=".45" /></svg>;
    case "text":
      return <svg {...common}><path d="M3 3h10M8 3v10M5.5 13h5" /></svg>;
    case "pin":
      return <svg {...common}><circle cx="5" cy="5" r="2.2" /><path d="m6.6 6.6 6 6M10.5 9.5l-1 2.5 2.5-1" /></svg>;
  }
}

const COLOUR_LABELS: Record<string, string> = {
  ink: "ink",
  muted: "muted",
  signal: "signal",
  electrical: "blue",
  translational: "green",
  thermal: "red",
  fluid: "cyan",
  magnetic: "orange",
  rotational: "grey",
  none: "none",
  raised: "raised",
  sunken: "sunken",
};
const STROKE_SWATCHES = Object.entries(STROKE).map(([token, color]) => ({ token, color, label: COLOUR_LABELS[token] ?? token }));
const FILL_SWATCHES = Object.entries(FILL).map(([token, color]) => ({ token, color, label: COLOUR_LABELS[token] ?? token }));

function ColorRow({
  label,
  value,
  swatches,
  onChange,
}: {
  label: "stroke" | "fill";
  value: string;
  swatches: { token: string; color: string; label: string }[];
  onChange: (token: string) => void;
}) {
  return (
    <div className="icon-colours" role="radiogroup" aria-label={label}>
      <span className="lbl">{label}</span>
      <span className="icon-swatches">
        {swatches.map((swatch) => (
          <button
            key={swatch.token}
            type="button"
            role="radio"
            aria-checked={swatch.token === value}
            aria-label={`${label}: ${swatch.label}`}
            title={`${label}: ${swatch.label}`}
            className={`icon-swatch${swatch.token === "none" ? " none" : ""}${swatch.token === value ? " on" : ""}`}
            style={swatch.token === "none" ? undefined : { background: swatch.color }}
            onClick={() => onChange(swatch.token)}
          />
        ))}
      </span>
    </div>
  );
}

export function IconView() {
  const api = useStudio();
  const [draft, setDraft] = useState<IconDef>(() => syncPortsWithEntries(api.model.icon, api.model.entries));
  const [selected, setSelected] = useState(-1);
  const [selectedPort, setSelectedPort] = useState<string | null>(null);
  const [tool, setTool] = useState<Tool>("select");
  const [grid, setGrid] = useState(true);
  const [snap, setSnap] = useState(true);
  const [preview, setPreview] = useState<Prim | null>(null);
  const [dirty, setDirty] = useState(false);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState("");
  const [connName, setConnName] = useState("");
  const [connClass, setConnClass] = useState("Modelica.Blocks.Interfaces.RealInput");
  const [connBusy, setConnBusy] = useState(false);
  const drag = useRef<Drag | null>(null);
  const dragPort = useRef<string | null>(null);
  const previewRef = useRef<Prim | null>(null);
  const svgRef = useRef<SVGSVGElement>(null);

  useEffect(() => {
    const next = syncPortsWithEntries(api.model.icon, api.model.entries);
    setDraft(next);
    setSelected((prev) => (next.g.length ? Math.min(Math.max(prev, 0), next.g.length - 1) : -1));
    setDirty(false);
    setSaved(false);
  }, [api.model.icon, api.model.entries]);

  const change = (fn: (icon: IconDef) => IconDef) => {
    setDraft(fn);
    setDirty(true);
  };

  const selectedPrim = draft.g[selected];
  const shortModel = api.model.name.split(".").pop() ?? api.model.name;
  const ports = draft.ports ?? [];
  const linkedBlockDef = buildLinkedBlockDef(api.model, draft);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return;
      if ((e.key === "Delete" || e.key === "Backspace") && selected >= 0) {
        e.preventDefault();
        change((icon) => removePrimAt(icon, selected));
        setSelected((i) => Math.max(0, i - 1));
      } else if (e.key === "Escape") {
        setTool("select");
        setSelected(-1);
        previewRef.current = null;
        setPreview(null);
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "d" && selected >= 0) {
        e.preventDefault();
        const prim = draft.g[selected];
        if (!prim) return;
        change((icon) => ({ ...icon, g: [...icon.g, movePrimBy(prim, 5, 5)] }));
        setSelected(draft.g.length);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [draft.g, selected]);

  const point = (e: ReactPointerEvent<SVGElement>): Pt => {
    const svg = svgRef.current;
    if (!svg) return [50, 50];
    const rect = svg.getBoundingClientRect();
    const x = Math.max(0, Math.min(100, ((e.clientX - rect.left) / Math.max(1, rect.width)) * 100));
    const y = Math.max(0, Math.min(100, ((e.clientY - rect.top) / Math.max(1, rect.height)) * 100));
    return snap ? [snapTo5(x), snapTo5(y)] : [x, y];
  };

  const styleOf = (prim: Prim | undefined): PrimStyle => ({
    stroke: prim?.stroke ?? "ink",
    weight: prim?.weight ?? "normal",
    dash: prim?.dash ?? "solid",
    fill: prim?.fill ?? "none",
    ...(prim?.t === "rect" ? { corner: prim.corner ?? "0" } : {}),
    ...(prim?.t === "text" ? { font: prim.font ?? "sans", size: prim.size ?? 12 } : {}),
  });

  const edgeOf = (at: Pt): { side: IconPort["side"]; along: number } => {
    const [x, y] = at;
    const edge = Math.min(x, 100 - x, y, 100 - y);
    const side: IconPort["side"] = edge === x ? "left" : edge === 100 - x ? "right" : edge === y ? "top" : "bottom";
    return { side, along: side === "left" || side === "right" ? y : x };
  };

  const placePort = (at: Pt) => {
    const id = selectedPort ?? ports[0]?.id;
    if (!id) return;
    const { side, along } = edgeOf(at);
    change((icon) => setPortPlacement(icon, api.model.entries, id, side, along));
    setSelectedPort(id);
  };

  const onCanvasDown = (e: ReactPointerEvent<SVGSVGElement>) => {
    const at = point(e);
    if (tool === "select") {
      setSelected(-1);
      return;
    }
    if (tool === "pin") {
      placePort(at);
      return;
    }
    e.currentTarget.setPointerCapture?.(e.pointerId);
    drag.current = { kind: "create", tool, start: at, style: styleOf(selectedPrim) };
    const first = primFromDrag(tool, at, at, styleOf(selectedPrim));
    previewRef.current = first;
    setPreview(first);
  };

  const onShapeDown = (e: ReactPointerEvent<SVGElement>, index: number) => {
    if (tool !== "select") return; // let the canvas start the chosen tool
    e.stopPropagation();
    setSelected(index);
    const prim = draft.g[index];
    if (!prim) return;
    svgRef.current?.setPointerCapture?.(e.pointerId);
    drag.current = { kind: "move", index, start: point(e), orig: structuredClone(prim) };
  };

  const onHandleDown = (e: ReactPointerEvent<SVGElement>, index: number, corner: "nw" | "ne" | "sw" | "se") => {
    e.stopPropagation();
    setSelected(index);
    const prim = draft.g[index];
    if (!prim) return;
    svgRef.current?.setPointerCapture?.(e.pointerId);
    drag.current = { kind: "resize", index, corner, orig: structuredClone(prim) };
  };

  const onMove = (e: ReactPointerEvent<SVGSVGElement>) => {
    const d = drag.current;
    if (!d) return;
    const at = point(e);
    if (dragPort.current) {
      const { side, along } = edgeOf(at);
      const id = dragPort.current;
      change((icon) => setPortPlacement(icon, api.model.entries, id, side, along));
      return;
    }
    if (d.kind === "create") {
      const next = primFromDrag(d.tool, d.start, at, d.style);
      previewRef.current = next;
      setPreview(next);
      return;
    }
    if (d.kind === "move") {
      const dx = at[0] - d.start[0];
      const dy = at[1] - d.start[1];
      change((icon) => ({ ...icon, g: icon.g.map((p, i) => (i === d.index ? movePrimBy(d.orig, dx, dy) : p)) }));
      return;
    }
    change((icon) => ({ ...icon, g: icon.g.map((p, i) => (i === d.index ? resizePrim(d.orig, d.corner, at) : p)) }));
  };

  const onUp = () => {
    const d = drag.current;
    drag.current = null;
    dragPort.current = null;
    if (d?.kind === "create") {
      const made = previewRef.current;
      previewRef.current = null;
      setPreview(null);
      if (made) {
        change((icon) => ({ ...icon, g: [...icon.g, made] }));
        setSelected(draft.g.length);
      }
    }
  };

  const duplicate = () => {
    if (!selectedPrim) return;
    change((icon) => ({ ...icon, g: [...icon.g, movePrimBy(selectedPrim, 5, 5)] }));
    setSelected(draft.g.length);
  };

  const bring = (to: "front" | "back") => {
    if (selected < 0) return;
    change((icon) => {
      const g = [...icon.g];
      const [one] = g.splice(selected, 1);
      if (!one) return icon;
      if (to === "front") g.push(one);
      else g.unshift(one);
      return { ...icon, g };
    });
    setSelected(to === "front" ? draft.g.length - 1 : 0);
  };

  const patchSelected = (patch: Parameters<typeof updatePrimTokens>[1]) => {
    if (!selectedPrim) return;
    change((icon) => ({ ...icon, g: icon.g.map((p, i) => (i === selected ? updatePrimTokens(p, patch) : p)) }));
  };

  const addConnector = async () => {
    const name = connName.trim() || `port${ports.length + 1}`;
    const cls = connClass.trim();
    if (!/^[A-Za-z_]\w*$/.test(name)) {
      setError("a connector name starts with a letter or _ and holds letters, digits and _");
      return;
    }
    if (!/^[A-Za-z_][\w.]*$/.test(cls)) {
      setError("a connector class is a qualified Modelica name");
      return;
    }
    setError("");
    setConnBusy(true);
    try {
      await api.addComponent(cls, name, { x: 200, y: 160, rotation: 0 });
      setSelectedPort(name);
      setTool("pin");
      setConnName("");
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setConnBusy(false);
    }
  };

  const save = async () => {
    setError("");
    const clean = sanitizeIconDef(draft, api.model.entries);
    try {
      await api.setIcon(clean);
      setDraft(clean);
      setDirty(false);
      setSaved(true);
      setTimeout(() => setSaved(false), 1200);
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const drawPrim = (p: Prim, idx: number, ghost = false) => {
    const stroke = STROKE[p.stroke ?? "ink"];
    const strokeWidth = WEIGHT[p.weight ?? "normal"];
    const common = {
      stroke,
      strokeWidth,
      strokeDasharray: DASH[p.dash ?? "solid"],
      strokeLinecap: "round" as const,
      strokeLinejoin: "round" as const,
      onPointerDown: (e: ReactPointerEvent<SVGElement>) => onShapeDown(e, idx),
      style: { cursor: tool === "select" ? "move" : "crosshair", pointerEvents: ghost ? ("none" as const) : undefined },
      opacity: ghost ? 0.55 : 1,
    };
    const fill = FILL[p.fill ?? "none"];
    switch (p.t) {
      case "line":
        return <polyline key={idx} {...common} fill="none" points={p.p.map(([x, y]) => `${x},${y}`).join(" ")} />;
      case "poly":
        return <polygon key={idx} {...common} fill={fill} points={p.p.map(([x, y]) => `${x},${y}`).join(" ")} />;
      case "rect":
        return <rect key={idx} {...common} fill={fill} x={p.x} y={p.y} width={p.w} height={p.h} rx={CORNER[p.corner ?? "0"]} />;
      case "ellipse":
        return <ellipse key={idx} {...common} fill={fill} cx={p.x + p.w / 2} cy={p.y + p.h / 2} rx={p.w / 2} ry={p.h / 2} />;
      case "arc": {
        const [cx, cy, rx, ry] = [p.x + p.w / 2, p.y + p.h / 2, p.w / 2, p.h / 2];
        const at = (a: number) => `${+(cx + rx * Math.cos((a * Math.PI) / 180)).toFixed(2)},${+(cy - ry * Math.sin((a * Math.PI) / 180)).toFixed(2)}`;
        const sweep = (((p.a1 - p.a0) % 360) + 360) % 360;
        return <path key={idx} {...common} fill={fill} d={`M${at(p.a0)}A${rx},${ry} 0 ${sweep > 180 ? 1 : 0} 0 ${at(p.a1)}`} />;
      }
      case "text":
        return (
          <text key={idx} x={p.x} y={p.y} fill={stroke} textAnchor="middle" dominantBaseline="central" fontFamily={FONT[p.font ?? "sans"]} fontSize={p.size ?? 12} style={{ cursor: tool === "select" ? "move" : "crosshair", opacity: ghost ? 0.55 : 1, pointerEvents: ghost ? "none" : undefined }} onPointerDown={(e) => onShapeDown(e, idx)}>
            {p.s}
          </text>
        );
    }
  };

  const box = selectedPrim ? primBounds(selectedPrim) : null;
  const portAt = (p: IconPort): Pt => (p.side === "left" ? [0, p.at] : p.side === "right" ? [100, p.at] : p.side === "top" ? [p.at, 0] : [p.at, 100]);

  return (
    <section className="icon-view" aria-label={`${shortModel} icon`}>
      <header className="icon-view-bar">
        <div className="icon-tools" role="toolbar" aria-label="Drawing tools">
          {TOOLS.map((t) => (
            <button
              key={t.tool}
              type="button"
              className={`studio-look-key${tool === t.tool ? " on" : ""}`}
              title={`${t.label} — ${t.hint}`}
              aria-label={t.label}
              aria-pressed={tool === t.tool}
              disabled={t.tool === "pin" && ports.length === 0}
              onClick={() => setTool(t.tool)}
            >
              <ToolIcon tool={t.tool} />
            </button>
          ))}
        </div>
        <span className="icon-bar-sep" />
        <button type="button" className={`studio-look-key${grid ? " on" : ""}`} aria-pressed={grid} onClick={() => setGrid((g) => !g)}>
          grid
        </button>
        <button type="button" className={`studio-look-key${snap ? " on" : ""}`} aria-pressed={snap} onClick={() => setSnap((s) => !s)}>
          snap
        </button>
        <span className="icon-bar-sep" />
        <button type="button" className="studio-look-key" disabled={!selectedPrim} onClick={duplicate} title="Duplicate (Ctrl+D)">
          duplicate
        </button>
        <button type="button" className="studio-look-key" disabled={!selectedPrim} onClick={() => bring("front")}>
          front
        </button>
        <button type="button" className="studio-look-key" disabled={!selectedPrim} onClick={() => bring("back")}>
          back
        </button>
        <button type="button" className="studio-look-key" disabled={!selectedPrim} onClick={() => selected >= 0 && (change((icon) => removePrimAt(icon, selected)), setSelected(Math.max(0, selected - 1)))}>
          delete
        </button>
        <span className="spacer" />
        <span className="icon-count">
          {draft.g.length} shapes · {plateSizeKey(draft)}
        </span>
        <button type="button" className="studio-save-key" disabled={!dirty} onClick={() => void save()}>
          {saved ? "Saved" : "Save"}
        </button>
      </header>
      <div className="icon-view-body">
        <div className={`icon-canvas${tool === "select" ? " select" : ""}`}>
          <svg
            ref={svgRef}
            className="icon-stage"
            viewBox="0 0 100 100"
            aria-label="Icon canvas, 0 to 100"
            onPointerDown={onCanvasDown}
            onPointerMove={onMove}
            onPointerUp={onUp}
            onPointerCancel={onUp}
          >
            {grid &&
              GRID_STEPS.flatMap((gx) =>
                GRID_STEPS.map((gy) => (
                  <circle key={`${gx}-${gy}`} className="studio-look-grid-dot" cx={gx} cy={gy} r={gx % 25 === 0 && gy % 25 === 0 ? 0.9 : 0.45} />
                )),
              )}
            {draft.g.map((p, idx) => drawPrim(p, idx))}
            {preview && drawPrim(preview, -1, true)}
            {ports.map((port) => {
              const [x, y] = portAt(port);
              return (
                <rect
                  key={port.id}
                  className={`icon-port${selectedPort === port.id ? " on" : ""}`}
                  x={x - 2.4}
                  y={y - 2.4}
                  width={4.8}
                  height={4.8}
                  onPointerDown={(e) => {
                    e.stopPropagation();
                    setSelectedPort(port.id);
                    dragPort.current = port.id;
                    svgRef.current?.setPointerCapture?.(e.pointerId);
                  }}
                >
                  <title>{port.id} · {port.side} {port.at}</title>
                </rect>
              );
            })}
            {box && <rect className="studio-look-sel-box" x={box.x} y={box.y} width={box.w} height={box.h} />}
            {box &&
              selectedPrim &&
              (selectedPrim.t === "rect" || selectedPrim.t === "ellipse" || selectedPrim.t === "arc") &&
              HANDLES.map((corner) => {
                const hx = corner.includes("w") ? box.x : box.x + box.w;
                const hy = corner.includes("n") ? box.y : box.y + box.h;
                return <rect key={corner} className="icon-handle" x={hx - 1.8} y={hy - 1.8} width={3.6} height={3.6} onPointerDown={(e) => onHandleDown(e, selected, corner)} />;
              })}
          </svg>
          <span className="icon-hint">
            {tool === "pin"
              ? ports.length
                ? `${selectedPort ?? ports[0]?.id}: click an edge`
                : "Add an input or output in Variables first"
              : tool === "select"
                ? "click to select · drag to move · corner handles resize"
                : "drag on the canvas to draw"}
          </span>
        </div>

        <aside className="icon-inspector" aria-label="Icon inspector">
          <section className="icon-section">
            <h3>shape</h3>
            {selectedPrim ? (
              <div className="studio-look-choices">
                <ColorRow label="stroke" value={selectedPrim.stroke ?? "ink"} swatches={STROKE_SWATCHES} onChange={(stroke) => patchSelected({ stroke: stroke as Stroke })} />
                <Switch<Weight> label="Weight" value={selectedPrim.weight ?? "normal"} onChange={(weight) => patchSelected({ weight })} options={WEIGHT_OPTIONS} />
                <ColorRow label="fill" value={selectedPrim.fill ?? "none"} swatches={FILL_SWATCHES} onChange={(fill) => patchSelected({ fill: fill as Fill })} />
                <Switch<Dash> label="Dash" value={selectedPrim.dash ?? "solid"} onChange={(dash) => patchSelected({ dash })} options={DASH_OPTIONS} />
                {selectedPrim.t === "rect" && (
                  <Switch<Corner> label="Corner" value={selectedPrim.corner ?? "0"} onChange={(corner) => patchSelected({ corner })} options={CORNER_OPTIONS} />
                )}
                {selectedPrim.t === "text" && (
                  <>
                    <Switch<"sans" | "mono"> label="Font" value={selectedPrim.font ?? "sans"} onChange={(font) => patchSelected({ font })} options={FONT_OPTIONS} />
                    <Switch<"12" | "12.5" | "13" | "15" | "30">
                      label="Size"
                      value={String(selectedPrim.size ?? 12) as "12" | "12.5" | "13" | "15" | "30"}
                      onChange={(sz) => patchSelected({ size: Number(sz) as 12 | 12.5 | 13 | 15 | 30 })}
                      options={SIZE_OPTIONS}
                    />
                    <input className="studio-prop-input" value={selectedPrim.s} aria-label="Text" onChange={(e) => patchSelected({ s: e.target.value })} />
                  </>
                )}
              </div>
            ) : (
              <p className="studio-handoff-note">Pick a shape to change its stroke, fill or text.</p>
            )}
          </section>

          {selectedPrim && "x" in selectedPrim && (
            <section className="icon-section">
              <h3>geometry</h3>
              <div className="icon-geometry">
                <label>x <NumberField value={selectedPrim.x} onChange={(x) => patchSelected({ x })} /></label>
                <label>y <NumberField value={selectedPrim.y} onChange={(y) => patchSelected({ y })} /></label>
                {"w" in selectedPrim && (
                  <>
                    <label>w <NumberField value={selectedPrim.w} onChange={(w) => patchSelected({ w })} /></label>
                    <label>h <NumberField value={selectedPrim.h} onChange={(h) => patchSelected({ h })} /></label>
                  </>
                )}
              </div>
            </section>
          )}

          <section className="icon-section">
            <h3>plate</h3>
            <Switch<PlateSizeKey> label="Size" value={plateSizeKey(draft)} onChange={(sz) => change((icon) => applyPlateSize(icon, sz))} options={PLATE_SIZES.map((s) => ({ value: s, label: s }))} />
          </section>

          <section className="icon-section">
            <h3>layers</h3>
            <div className="icon-layers">
              {draft.g.length === 0 && <p className="studio-handoff-note">Nothing drawn yet.</p>}
              {draft.g
                .map((p, i) => ({ p, i }))
                .reverse()
                .map(({ p, i }) => (
                  <button key={i} type="button" className={`icon-layer${selected === i ? " on" : ""}`} onClick={() => setSelected(i)}>
                    <span className="icon-layer-kind">{p.t}</span>
                    <span className="icon-layer-name">{p.t === "text" ? p.s : `${p.t} ${i + 1}`}</span>
                  </button>
                ))}
            </div>
          </section>

          <section className="icon-section">
            <h3>connectors</h3>
            {ports.length === 0 ? (
              <p className="studio-handoff-note">No connectors yet. Add one below, or an input/output in Variables.</p>
            ) : (
              ports.map((port) => (
                <div key={port.id} className={`studio-look-port-row${selectedPort === port.id ? " on" : ""}`}>
                  <button type="button" className="icon-port-pick" onClick={() => setSelectedPort(port.id)} title="Place this connector with the pin tool">
                    {port.id}
                  </button>
                  <SideSelect label={`${port.id} side`} value={port.side} onChange={(side) => change((icon) => setPortPlacement(icon, api.model.entries, port.id, side, port.at))} />
                  <NumberField className="studio-look-at-input" aria-label={`${port.id} at`} value={port.at} onChange={(at) => change((icon) => setPortPlacement(icon, api.model.entries, port.id, port.side, at))} />
                </div>
              ))
            )}
            <div className="icon-add-connector">
              <input value={connName} placeholder="name" aria-label="New connector name" onChange={(e) => setConnName(e.target.value)} />
              <input value={connClass} placeholder="class" aria-label="New connector class" onChange={(e) => setConnClass(e.target.value)} />
              <button type="button" className="studio-look-key" disabled={connBusy} onClick={() => void addConnector()} title="Add the connector to the model, then place its pin">
                {connBusy ? "…" : "+"}
              </button>
            </div>
          </section>

          <section className="icon-section">
            <h3>preview</h3>
            <div className="studio-preview-pair">
              <div className="studio-preview-tile" data-theme="light">
                <ReactFlowProvider>
                  <CompactBlock id={`${shortModel}-light`} def={linkedBlockDef} params={{}} selected={false} connectors={api.model.connectors} />
                </ReactFlowProvider>
                <span className="studio-preview-caption">light</span>
              </div>
              <div className="studio-preview-tile" data-theme="dark">
                <ReactFlowProvider>
                  <CompactBlock id={`${shortModel}-dark`} def={linkedBlockDef} params={{}} selected={false} connectors={api.model.connectors} />
                </ReactFlowProvider>
                <span className="studio-preview-caption">dark</span>
              </div>
            </div>
          </section>
          {error && <p className="studio-var-error" role="alert">{error}</p>}
        </aside>
      </div>
    </section>
  );
}
