import { useEffect, useRef, useState } from "react";
import { useStudio, type ExportMode, type ExportProgress } from "../model/types.ts";
import {
  checkModelBalance,
  findDanglingFlowPorts,
  fixDanglingPort,
  runStationExport,
  undrawnNote,
  type BalanceResult,
  type DanglingPort,
  type ExportOutcome,
} from "./dock.ts";
import "./dock.css";

export function StudioDock({
  canExport = true,
  why = "",
  beforeExport,
  onLog,
}: {
  canExport?: boolean;
  why?: string;
  beforeExport?: () => Promise<void>;
  onLog?: (text: string, level?: "info" | "warn" | "error") => void;
}) {
  const api = useStudio();
  const [balance, setBalance] = useState<BalanceResult>({
    kind: "balanced",
    equations: 0,
    unknowns: 0,
    ratio: "… / …",
    words: "checking balance…",
  });
  const [running, setRunning] = useState<{ port: number; up: boolean; ready?: boolean; line?: string; stage?: string; elapsed?: number; error?: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState<ExportProgress | null>(null);
  const [outcome, setOutcome] = useState<ExportOutcome | null>(null);
  const [fixError, setFixError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  // The station build writes a log; follow it from this byte and send each new line to the console.
  const logOffset = useRef(0);
  const logPending = useRef("");

  const modelSignature = JSON.stringify({
    name: api.model.name,
    components: api.model.components,
    connections: api.model.connections,
    params: api.model.params,
    entries: api.model.entries,
    revision,
  });

  // Keyed on the model and omc only: api itself is rebuilt on every status push, and
  // checkModel causes one, so depending on api re-checks forever.
  const omc = api.omc;
  const modelName = api.model.name;
  useEffect(() => {
    let cancelled = false;
    const timer = setTimeout(() => {
      void checkModelBalance({ omc, model: { name: modelName } }).then((res) => {
        if (!cancelled) setBalance(res);
      });
    }, 300);
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [omc, modelName, modelSignature]);

  const dangling = findDanglingFlowPorts(api.model);
  const undrawn = undrawnNote(api.model.undrawn);
  const isBalanced = balance.kind === "balanced";

  const handleFix = async (item: DanglingPort) => {
    setFixError(null);
    const err = await fixDanglingPort(api, item);
    if (err) setFixError(err);
    setRevision((r) => r + 1);
  };

  const handleExport = async (mode: ExportMode) => {
    if (!isBalanced || busy || !canExport) return;
    setBusy(true);
    await beforeExport?.();
    setOutcome(null);
    logOffset.current = 0;
    logPending.current = "";
    onLog?.(mode === "pst" ? "packing the station…" : "starting the station: building…", "info");
    const again = mode === "serve" ? running?.port : undefined; // a restart keeps its port
    if (again) await runStationExport(api, "stop", () => {}, again);
    setRunning(null);
    setProgress({ stage: "check", fraction: 0 });
    const res = await runStationExport(api, mode, (p) => {
      setProgress({ ...p });
    }, again);
    setProgress(null);
    if (!(mode === "serve" && res.ok)) setOutcome(res); // a started station reports below as it comes up
    if (mode === "serve" && res.ok && res.result.port) {
      logOffset.current = 0;
      setRunning({ port: res.result.port, up: false });
    }
    setBusy(false);
  };

  // A started station builds first (minutes, cold): poll until it listens or exits.
  const pollPort = running && !running.up ? running.port : null;
  const apiRef = useRef(api);
  apiRef.current = api; // api is rebuilt on every status push: keep the timer off it
  useEffect(() => {
    if (!pollPort) return;
    const timer = setInterval(() => {
      void runStationExport(apiRef.current, "status", () => {}, pollPort, logOffset.current).then((res) => {
        // Stream the log: the poll returns only what was written since the last one.
        const chunk = res.result?.log ?? "";
        if (chunk) {
          logPending.current += chunk;
          for (let at; (at = logPending.current.indexOf("\n")) >= 0;) {
            const line = logPending.current.slice(0, at).trimEnd();
            logPending.current = logPending.current.slice(at + 1);
            if (line) onLog?.(line, "info");
          }
          if (res.result?.offset !== undefined) logOffset.current = res.result.offset;
        }
        if (res.ok && res.result.state === "building") {
          const line = (logPending.current || chunk).trim().split("\n").pop();
          const { stage, elapsed, error } = res.result;
          return setRunning((r) => (r && r.port === pollPort ? { ...r, line, stage, elapsed, error: res.result.failed ? error : undefined } : r));
        }
        if (!res.ok) {
          if (res.result?.tail && !chunk.includes(res.result.tail)) onLog?.(res.result.tail.trimEnd(), "error");
          setOutcome(res); // it exited: the log says why
        }
        setRunning(res.ok ? { port: pollPort, up: true } : null);
      });
    }, 1500);
    return () => clearInterval(timer);
  }, [pollPort]);

  // Listening is not ready: wait for the station's own hello over its WebSocket.
  const probePort = running?.up && !running.ready ? running.port : null;
  useEffect(() => {
    if (!probePort) return;
    let ws: WebSocket | undefined;
    let timer: ReturnType<typeof setTimeout>;
    let done = false;
    const tryOnce = () => {
      ws = new WebSocket(`ws://localhost:${probePort}`);
      ws.onmessage = () => {
        done = true;
        ws?.close();
        setRunning((r) => (r && r.port === probePort ? { ...r, ready: true } : r));
      };
      ws.onclose = () => {
        if (!done) timer = setTimeout(tryOnce, 1000);
      };
    };
    tryOnce();
    return () => {
      done = true;
      clearTimeout(timer);
      ws?.close();
    };
  }, [probePort]);
  // While it builds, paperboat's own stage and clock (pb#332), else the log's last line.
  const STAGE: Record<string, string> = { prepare: "preparing", configure: "configuring", compile: "compiling", waiting: "starting" };
  const stationWords = !running
    ? null
    : running.error
      ? `the station failed: ${running.error}`
      : !running.up && running.stage
        ? `${STAGE[running.stage] ?? "building"} the station for :${running.port}…${running.elapsed !== undefined ? ` ${Math.floor(running.elapsed)}s` : ""}`
        : !running.up
      ? `building for :${running.port}${running.line ? `: ${running.line.slice(0, 90)}` : "…"}`
      : !running.ready
        ? `started on :${running.port}, waiting for it to answer…`
        : `ready: connect Paperboat to ws://localhost:${running.port}`;

  const handleStop = async () => {
    if (!running || busy) return;
    setBusy(true);
    const res = await runStationExport(api, "stop", () => {}, running.port);
    setOutcome(res);
    if (res.ok) setRunning(null);
    setBusy(false);
  };

  return (
    <div className="studio-dock" role="region" aria-label="Model balance and export" data-own-keys>
      {progress && (
        <div className="studio-dock-progress-track" aria-hidden="true">
          <div
            className="studio-dock-progress-bar"
            style={{ width: `${Math.max(0, Math.min(100, Math.round(progress.fraction * 100)))}%` }}
          />
        </div>
      )}

      <div className="studio-dock-row">
        <output
          className={`studio-dock-balance${isBalanced ? "" : " unbalanced"}`}
          aria-label="Equation and unknown balance"
        >
          <span className="studio-dock-dot" aria-hidden="true" />
          <span title={balance.words}>{balance.words}</span>
        </output>

        {undrawn && (
          <output
            className="studio-dock-undrawn"
            aria-label="Constructs not drawn as declared"
            title={undrawn.title}
          >
            {undrawn.label}
          </output>
        )}

        {dangling.length > 0 && (
          <div className="studio-dock-dangling" aria-label="Dangling flow connectors">
            {dangling.map((item) => (
              <span key={item.portPath} className="studio-dock-dangling-item">
                <span>{item.label}</span>
                {item.fix && (
                  <button
                    type="button"
                    className="studio-dock-fix-key"
                    onClick={() => void handleFix(item)}
                  >
                    [ {item.fix.buttonLabel} ]
                  </button>
                )}
              </span>
            ))}
          </div>
        )}

        <div className="studio-dock-export">
          <button
            type="button"
            className="studio-dock-export-key"
            disabled={!isBalanced || busy || !canExport}
            title={!canExport ? why : isBalanced ? "Download the station as a .pst package" : balance.words}
            onClick={() => void handleExport("pst")}
          >
            Download .pst
          </button>
          <button
            type="button"
            className="studio-dock-export-key"
            disabled={!isBalanced || busy || !canExport}
            title={!canExport ? why : isBalanced ? "Start the station as ssom_<model> on a free port" : balance.words}
            onClick={() => void handleExport("serve")}
          >
            {running ? "Restart" : "Start"} station
          </button>
          {running && (
            <button type="button" className="studio-dock-export-key" disabled={busy} onClick={() => void handleStop()}>
              Stop :{running.port}
            </button>
          )}
        </div>
      </div>

      {(progress || outcome || fixError || stationWords) && (
        <div className="studio-dock-row">
          {progress && <span className="studio-dock-stage">{progress.stage}</span>}
          {stationWords && !progress && (
            <span className={`studio-dock-stage station-${running?.error ? "failed" : running?.ready ? "ready" : "wait"}`}>
              {running && !running.up && !running.error && <span className="wave" aria-hidden="true"><i /><i /><i /><i /><i /></span>}
              {stationWords}
            </span>
          )}
          {outcome && (
            <span className={`studio-dock-result${outcome.ok ? "" : " error"}`}>
              {outcome.line}
            </span>
          )}
          {fixError && <span className="studio-dock-result error">{fixError}</span>}
        </div>
      )}
    </div>
  );
}
