import { useEffect, useState } from "react";
import { AboutSquero } from "@paperboat/shared/squero";
import type { OmcClient, OmcStatus } from "../omc/omc.ts";
import { STUDIO_VERSION } from "../version.ts";

/**
 * About Squero Studio OM: the shared dialog with the Studio, Paperboat and OpenModelica versions.
 * The server's `about` op runs `paperboat --version`, so Paperboat is "Not installed" when it isn't there.
 */
export function AboutModal({ omc, status, onClose }: { omc: OmcClient; status: OmcStatus; onClose: () => void }) {
  // undefined: still asking the server; null: paperboat isn't installed.
  const [paperboat, setPaperboat] = useState<string | null | undefined>(undefined);

  useEffect(() => {
    let live = true;
    omc.op<{ paperboat?: string | null }>("about").then(
      (reply) => live && setPaperboat(reply.paperboat ?? null),
      () => live && setPaperboat(null),
    );
    return () => {
      live = false;
    };
  }, [omc]);

  const omcVersion = /\d+\.\d+\.\d+/.exec(status.version)?.[0] ?? "";
  return (
    <AboutSquero
      product="OM"
      runtime="OpenModelica"
      facts={[
        { label: "Version", value: STUDIO_VERSION },
        { label: "Paperboat", value: paperboat === undefined ? "checking…" : paperboat ?? "Not installed" },
        { label: "OpenModelica", value: omcVersion || (status.state === "failed" ? "Not running" : "checking…") },
        { label: "License", value: "GPL-3.0-only" },
      ]}
      credits={<>Built on <a href="https://openmodelica.org" target="_blank" rel="noreferrer">OpenModelica</a>{omcVersion ? ` ${omcVersion}` : ""}.</>}
      onClose={onClose}
    />
  );
}
