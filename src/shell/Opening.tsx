// The wait screen: says what Studio is really waiting for, in order: the OpenModelica runtime
// starting, then the Modelica library loading with the model, then reading the model. The screen
// itself (timer, wave, hint) is the shared WaitScreen; only Studio's words are here.
import { WaitScreen } from "@paperboat/shared/components";
import type { OmcStatus } from "../omc/omc.ts";

export function Opening({ status, stage, error }: { status: OmcStatus; stage: string; error: string }) {
  const failed = !!error || status.state === "failed";
  const words = error || (status.state === "failed" ? "No Studio server: start one with squero-om" : status.state === "checking" ? "starting the OpenModelica runtime" : stage);
  const hint = status.state === "checking"
    ? "OpenModelica starts a fresh process: this is the slow part of a cold start."
    : "The first open loads the whole Modelica Standard Library, which takes a few seconds.";
  return <WaitScreen words={words} failed={failed} hint={hint} />;
}
