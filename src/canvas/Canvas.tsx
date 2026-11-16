// The acausal diagram (contracts §8.2): parts on @xyflow/react with the shared Compact plate, icon
// and port rendering. Acausal wires have no direction, causal ones run output to input with an
// arrow; a wire the rules refuse says why in words and is never drawn. Every edit goes to the store.

import {
  Background,
  BaseEdge,
  ConnectionMode,
  MarkerType,
  Position,
  ReactFlow,
  applyNodeChanges,
  useReactFlow,
  type Edge,
  type EdgeProps,
  type Node,
  type NodeChange,
  type NodeProps,
  MiniMap,
} from "@xyflow/react";
import { flipPlacement, netWireType, orthoPoints, rotatePlacement, roundedPolyline, squareUp, useCanvasHotkeys, type BlockDef, type Connectors, type IconDef, type WirePoint } from "@paperboat/shared";
import { CanvasHelp, CompactBlock, SNAP, useWireBends, WireDragContext, type WireDrag } from "@paperboat/shared/components";
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type CSSProperties, type DragEvent } from "react";
import { DRAG_TYPE } from "../library/Library.tsx";
import type { ComponentInst, Connection, StudioApi } from "../model/types.ts";
import { snap } from "../store/exprs.ts";
import { freeSpot } from "../store/plans.ts";
import type { Actions } from "../store/index.tsx";
import { endOf, isConnector, pathOf, split, wireError } from "../store/wires.ts";
import { domainColor, domainOf } from "./domain.ts";
import { centreFor, placedIcon, topLeft } from "./geometry.ts";
import { QuickAdd } from "./QuickAdd.tsx";

type PartData = { part: ComponentInst; icon: IconDef; connectors: Connectors; wires: Record<string, number> };
type PartNode = Node<PartData, "part">;
/** `points`: the bends in drawn order (source end first); `wire`: the connection as the model has it. */
type WireData = { points?: WirePoint[]; wire: Connection; flipped: boolean; kind: string; causal: boolean; missing: boolean; color?: string };
type WireEdge = Edge<WireData, "wire">;


/** The model's own connector seen from inside: its input feeds the parts (a wire's source), its output takes one in. */
function inside(connectors: Connectors, part: ComponentInst): Connectors {
  const cls = part.ports[0].class;
  const c = connectors[cls];
  if (!c || (c.kind !== "input" && c.kind !== "output")) return connectors;
  return { ...connectors, [cls]: { ...c, kind: c.kind === "input" ? "output" : "input" } };
}

function PartView({ id, data, selected }: NodeProps<PartNode>) {
  const { part, icon, connectors, wires } = data;
  const def = useMemo<BlockDef>(() => ({ type: part.class, label: part.name, kind: "function", inputs: [], outputs: [], params: [], view: { compact: true, icon } }), [part.class, part.name, icon]);
  return <CompactBlock id={id} def={def} params={{ ...part.defaults, ...part.params }} selected={selected} connectors={connectors} wires={wires} />;
}

/** Save a wire's dragged bends (in the model's a-to-b order). */
const SetBends = createContext<(wire: Connection, bends: WirePoint[]) => Promise<void>>(() => Promise.resolve());

const sideOf = (p: Position) => (p === Position.Left ? "left" : p === Position.Top ? "top" : p === Position.Bottom ? "bottom" : "right");

function WireView({ id, data, selected, markerEnd, sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition }: EdgeProps<WireEdge>) {
  const setBends = useContext(SetBends);
  const from: WirePoint = [sourceX, sourceY];
  const to: WirePoint = [targetX, targetY];
  const routed = data?.points ? squareUp(from, data.points, to) : orthoPoints(from, sideOf(sourcePosition), to, sideOf(targetPosition));
  const { points, handles } = useWireBends(routed, !!selected && !!data, (bends) => setBends(data!.wire, data!.flipped ? bends.reverse() : bends));
  const path = roundedPolyline(points, 8);
  return (
    <>
      <path d={path} className="wire-casing" />
      <BaseEdge id={id} path={path} markerEnd={markerEnd} interactionWidth={16} style={data?.color ? { stroke: data.color } : undefined} className={`wire-${data?.kind ?? "signal"}${data?.missing ? " wire-missing" : ""}${selected ? " lit" : ""}`} />
      {handles}
    </>
  );
}

const nodeTypes = { part: PartView };
const edgeTypes = { wire: WireView };

type Props = { api: StudioApi; actions: Actions; classes: string[]; onOpen(name: string): void };

export function Canvas({ api, actions, classes, onOpen }: Props) {
  const { model, selection } = api;
  const flow = useReactFlow();
  const root = useRef<HTMLDivElement>(null);
  const [wire, setWire] = useState<Connection | null>(null);
  const [quick, setQuick] = useState(false);
  const [anchor, setAnchor] = useState<string | null>(null);

  // --- what the model draws ---
  const counts = useMemo(() => {
    const out: Record<string, Record<string, number>> = {};
    for (const c of model.connections)
      for (const path of [c.a, c.b]) {
        const { part, port } = split(path);
        (out[part] ??= {})[port] = (out[part][port] ?? 0) + 1;
      }
    return out;
  }, [model.connections]);

  // fit the view once, to a model that opens with parts; after that the view is the user's
  useEffect(() => {
    const t = window.setTimeout(() => model.components.length && flow.fitView({ padding: 0.25, maxZoom: 1 }), 200);
    return () => window.clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [model.name]);

  const [nodes, setNodes] = useState<PartNode[]>([]);
  useEffect(() => {
    setNodes(
      model.components.map((part) => {
        const [x, y] = topLeft(part);
        return { id: part.name, type: "part", position: { x, y }, style: { "--ink": domainColor(domainOf(part.class)) } as CSSProperties, selected: selection.includes(part.name), data: { part, icon: placedIcon(part), connectors: isConnector(part) ? inside(model.connectors, part) : model.connectors, wires: counts[part.name] ?? {} } };
      }),
    );
  }, [model, counts, selection]);

  const edges = useMemo<WireEdge[]>(() => {
    const out: WireEdge[] = [];
    for (const c of model.connections) {
      let [a, b] = [c.a, c.b];
      const ea = endOf(model, a);
      const eb = endOf(model, b);
      const flipped = ea?.kind === "input" && eb?.kind === "output";
      if (flipped) [a, b] = [b, a]; // a causal wire is drawn from its output
      const pa = split(a);
      const pb = split(b);
      const kind = ea?.kind === "acausal" ? netWireType(model.connectors[ea.cls]) : "signal";
      const owner = (path: string) => model.components.find((p) => p.name === split(path).part)?.class ?? "";
      const color = kind === "signal" ? domainColor("signal") : domainColor(domainOf(owner(a)) ?? domainOf(owner(b)));
      out.push({
        id: `${c.a}|${c.b}`,
        type: "wire",
        source: pa.part,
        sourceHandle: pa.port,
        target: pb.part,
        targetHandle: pb.port,
        selected: wire?.a === c.a && wire.b === c.b,
        markerEnd: kind === "signal" ? { type: MarkerType.ArrowClosed, width: 14, height: 14 } : undefined,
        data: { ...(c.points ? { points: flipped ? [...c.points].reverse() : c.points } : {}), wire: c, flipped, kind, causal: kind === "signal", missing: !ea || !eb, ...(color ? { color } : {}) },
      });
    }
    return out;
  }, [model, wire]);

  // --- interaction ---
  const onNodesChange = useCallback((changes: NodeChange<PartNode>[]) => setNodes((n) => applyNodeChanges(changes, n)), []);
  const centre = () => {
    const rect = root.current?.getBoundingClientRect();
    const p = flow.screenToFlowPosition({ x: (rect?.left ?? 0) + (rect?.width ?? 0) / 2, y: (rect?.top ?? 0) + (rect?.height ?? 0) / 2 });
    return { x: snap(p.x), y: snap(p.y), rotation: 0 as const };
  };
  const place = (cls: string, at = freeSpot(model, centre())) => void actions.place(cls, at).then((name) => api.select([name]));

  const onDrop = (e: DragEvent) => {
    const cls = e.dataTransfer.getData(DRAG_TYPE);
    if (!cls) return;
    e.preventDefault();
    const p = flow.screenToFlowPosition({ x: e.clientX, y: e.clientY });
    place(cls, { x: snap(p.x), y: snap(p.y), rotation: 0 });
  };

  const pathFor = (nodeId: string, handle: string | null) => {
    const part = model.components.find((c) => c.name === nodeId);
    return part ? pathOf(part, handle ?? "") : `${nodeId}.${handle ?? ""}`;
  };
  const drag = useMemo<WireDrag | null>(
    () =>
      anchor === null
        ? null
        : {
            target: null,
            accepts: (nodeId, handle) => wireError(model, anchor, pathFor(nodeId, handle)) === null,
            why: (nodeId, handle) => wireError(model, anchor, pathFor(nodeId, handle)),
          },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [anchor, model],
  );

  const rotateSelected = useCallback(async (cw: boolean) => {
    if (!selection.length) return;
    for (const part of model.components.filter((c) => selection.includes(c.name))) {
      const next = rotatePlacement(part.placement, cw);
      await actions.move(part.name, { ...part.placement, ...next });
    }
  }, [selection, model.components, actions]);

  const flipSelected = useCallback(async (axis: "x" | "y") => {
    if (!selection.length) return;
    for (const part of model.components.filter((c) => selection.includes(c.name))) {
      const next = flipPlacement(part.placement, axis);
      await actions.move(part.name, { ...part.placement, ...next });
    }
  }, [selection, model.components, actions]);

  const duplicateSelected = useCallback(async () => {
    if (!selection.length) return;
    const newNames: string[] = [];
    for (const name of selection) {
      const part = model.components.find((c) => c.name === name);
      if (part) {
        const spot = freeSpot(model, { x: part.placement.x + 40, y: part.placement.y + 40 });
        const placedName = await actions.place(part.class, {
          ...spot,
          rotation: part.placement.rotation,
          mirror: part.placement.mirror,
        });
        for (const [k, v] of Object.entries(part.params)) {
          await api.setParam(`${placedName}.${k}`, v);
        }
        newNames.push(placedName);
      }
    }
    if (newNames.length) api.select(newNames);
  }, [selection, model, actions, api]);

  const deleteSelected = useCallback(() => {
    if (wire || selection.length) {
      void actions.remove(selection.filter((s) => model.components.some((c) => c.name === s)), wire ? [wire] : []);
      setWire(null);
    }
  }, [wire, selection, model.components, actions]);

  useCanvasHotkeys({
    hasSelection: Boolean(selection.length || wire),
    onInsert: () => !quick && setQuick(true),
    onRotateCW: () => void rotateSelected(true),
    onRotateCCW: () => void rotateSelected(false),
    onFlipH: () => void flipSelected("x"),
    onFlipV: () => void flipSelected("y"),
    onDuplicate: () => void duplicateSelected(),
    onDelete: deleteSelected,
    onEscape: () => {
      setWire(null);
      api.select([]);
    },
  });

  const selectedWire = wire ? `${wire.a} - ${wire.b}` : undefined;
  const latest = useRef(actions);
  latest.current = actions;
  const wireKey = wire ? `${wire.a}|${wire.b}` : "";
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const plan = useCallback((cls: string) => latest.current.planSplice(wire!, cls), [wireKey]);

  return (
    <div ref={root} className="canvas" tabIndex={0} onDragOver={(e) => e.dataTransfer.types.includes(DRAG_TYPE) && e.preventDefault()} onDrop={onDrop} data-testid="canvas">
      <SetBends.Provider value={actions.bend}>
      <WireDragContext.Provider value={drag}>
        <ReactFlow<PartNode, WireEdge>
          nodes={nodes}
          edges={edges}
          nodeTypes={nodeTypes}
          edgeTypes={edgeTypes}
          onNodesChange={onNodesChange}
          connectionMode={ConnectionMode.Loose}
          connectionRadius={SNAP.connect}
          deleteKeyCode={null}
          zoomOnDoubleClick={false}
          nodesConnectable
          proOptions={{ hideAttribution: true }}
          onConnectStart={(_, from) => from.nodeId && setAnchor(pathFor(from.nodeId, from.handleId))}
          onConnectEnd={() => setAnchor(null)}
          onConnect={(c) => {
            const a = pathFor(c.source, c.sourceHandle);
            const b = pathFor(c.target, c.targetHandle);
            void api.connect(a, b).then((why) => actions.say(why ?? ""));
          }}
          onNodeClick={(e, n) => {
            // the store is the one source of what is selected; a click says what changes
            setWire(null);
            api.select(e.ctrlKey || e.metaKey ? (selection.includes(n.id) ? selection.filter((s) => s !== n.id) : [...selection, n.id]) : [n.id]);
            onOpen(n.id);
          }}
          onEdgeClick={(_, e) => {
            const c = model.connections.find((w) => `${w.a}|${w.b}` === e.id);
            setWire(c ?? null);
            api.select([]);
          }}
          onPaneClick={() => {
            setWire(null);
            api.select([]);
            actions.say("");
          }}
          onNodeDragStop={(_, n) => {
            const part = model.components.find((c) => c.name === n.id);
            if (!part) return;
            const [x, y] = centreFor(part, n.position);
            void actions.move(part.name, { ...part.placement, x: snap(x), y: snap(y) });
          }}
        >
          <Background />
          <MiniMap
            pannable
            zoomable
            ariaLabel="Overview of the diagram: click or drag to move the view"
            nodeColor={(n) => (n.selected ? "var(--text)" : "var(--line-strong)")}
            nodeStrokeColor="var(--line-strong)"
            nodeStrokeWidth={3}
            nodeBorderRadius={2}
            maskColor="var(--surface-float)"
            maskStrokeColor="var(--line-ink)"
            maskStrokeWidth={1}
          />
        </ReactFlow>
        <CanvasHelp />
      </WireDragContext.Provider>
      </SetBends.Provider>
      {actions.notice && (
        <p className="notice" role="status">
          {actions.notice}
        </p>
      )}
      {quick && <QuickAdd names={classes} wire={selectedWire} plan={plan} onPlace={(cls) => place(cls)} onClose={() => (setQuick(false), root.current?.focus())} />}
    </div>
  );
}

