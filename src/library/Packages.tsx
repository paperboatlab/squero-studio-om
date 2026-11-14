// The Modelica libraries, in the shared package panel (@paperboat/shared/packages) that Squero MX
// shows Julia in and Paperboat its toolkits: OpenModelica is the runtime, the installed libraries
// the environment, the OpenModelica package index the registry. The server's `packages` op runs
// OpenModelica's own package manager (installPackage, upgradeInstalledPackages) through omc.
import { useEffect, useState } from "react";
import { PackagePanel, type RegistryItem } from "@paperboat/shared/packages";
import type { OmcCalls } from "../omc/omc.ts";

type Status = { version: string; path: string; libraries: { name: string; versions: string[] }[] };
type Index = { libraries: { name: string; repo: string; versions: string[] }[] };

const message = (failure: unknown) => String((failure as Error).message ?? failure);

export function Packages({ omc, onInstalled }: { omc: OmcCalls; onInstalled?: (name: string) => void }) {
  const [status, setStatus] = useState<Status | null>(null);
  const [busy, setBusy] = useState("");
  const [error, setError] = useState("");
  const [index, setIndex] = useState<RegistryItem[]>([]);
  const [indexState, setIndexState] = useState({ loading: "", error: "", loaded: false });

  const run = async (label: string, job: () => Promise<Status>) => {
    setBusy(label);
    setError("");
    try {
      setStatus(await job());
    } catch (failure) {
      setError(message(failure));
    } finally {
      setBusy("");
    }
  };

  useEffect(() => {
    void run("reading the libraries", () => omc.op<Status>("packages", { action: "status" }));
  }, [omc]);

  // The index downloads once per open of the panel (updatePackageIndex); retry reads it again.
  useEffect(() => {
    if (indexState.loaded) return;
    let live = true;
    setIndexState((s) => ({ ...s, loading: "updating the package index", error: "" }));
    omc
      .op<Index>("packages", { action: "index" })
      .then((found) => {
        if (!live) return;
        setIndex(found.libraries.map((l) => ({ name: l.name, latest: l.versions[0], versions: l.versions, repo: l.repo, note: l.repo.replace(/^https?:\/\/(www\.)?github\.com\//, "").replace(/\.git$/, "") })));
        setIndexState({ loading: "", error: "", loaded: true });
      })
      .catch((failure) => live && setIndexState({ loading: "", error: message(failure), loaded: false }));
    return () => {
      live = false;
    };
  }, [omc, indexState.loaded]);

  return (
    <PackagePanel
      noun="library"
      busy={busy}
      error={error}
      loading={busy || undefined}
      runtime={status && { title: "openmodelica", tag: status.version.replace(/^OpenModelica\s*/, "") }}
      environment={
        status && {
          title: "libraries",
          path: status.path,
          packages: status.libraries.map((l) => ({ name: l.name, version: l.versions.join(", ") })),
          empty: "No libraries installed. Add one from the index.",
          actions: [
            { label: "Reload", run: () => void run("reading the libraries", () => omc.op<Status>("packages", { action: "status" })) },
            { label: "Upgrade all", title: "Install the newest version of every installed library", run: () => void run("upgrading the libraries", () => omc.op<Status>("packages", { action: "upgrade" })) },
          ],
        }
      }
      registry={{
        title: "OpenModelica index",
        items: index,
        itemsLabel: `${index.length} libraries`,
        loading: indexState.loading,
        error: indexState.error,
        retry: () => setIndexState((s) => ({ ...s, loaded: false })),
        pickVersion: true,
        add: (name, version) =>
          void run(`installing ${name}${version ? ` ${version}` : ""}`, async () => {
            const next = await omc.op<Status>("packages", { action: "install", name, version: version ?? "" });
            onInstalled?.(name);
            return next;
          }),
      }}
    />
  );
}
