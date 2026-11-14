// The top bar: squero [om], the menus and the model title on the left; on the right
// only three quiet controls: the side-pane toggle, the OMC status pill and a theme cycle (both
// from @paperboat/shared, the same as Paperboat's top bar).
// The source view and Export live in the side pane, the footer and the menus.

import type { ReactNode } from "react";
import { MenuBar, StatusPill, ThemeToggle, type Theme } from "@paperboat/shared/components";
import type { Command } from "@paperboat/shared";
import { chip, type OmcState, type OmcStatus } from "../omc/omc.ts";
import { SqueroLogo } from "@paperboat/shared/squero";

export function Wordmark({ running = false }: { running?: boolean }) {
  return (
    <span className="wordmark" aria-label="Squero Studio: OpenModelica">
      <SqueroLogo product="OM" height={20} running={running} />
    </span>
  );
}

export function ProjectLabel({ name, dirty }: { name: string; dirty?: boolean }) {
  return (
    <span className="proj" title={dirty ? `${name}: unsaved changes (Ctrl+S saves)` : name}>
      <span className="proj-name">{name}</span>
      {dirty && <i className="dirty" role="img" aria-label="Unsaved changes" />}
    </span>
  );
}

export type TopBarProps = {
  status: OmcStatus;
  model?: string;
  dirty?: boolean;
  commands: Command[];
  side: boolean;
  onToggleSide: () => void;
  theme: Theme;
  onTheme: (t: Theme) => void;
  onStatusClick?: () => void;
  running?: boolean;
  focus?: ReactNode;
  showFocus?: boolean;
  errors?: number;
  consoleShown?: boolean;
  onConsole?: () => void;
  aboutShown?: boolean;
  onAbout?: () => void;
};

const STATES: OmcState[] = ["checking", "ready", "failed"];
const TONE = { ready: "ok", checking: "busy", failed: "bad" } as const;

export function TopBar({
  status,
  model,
  dirty = false,
  commands,
  side,
  onToggleSide,
  theme,
  onTheme,
  onStatusClick,
  running = false,
  focus,
  showFocus = true,
  errors = 0,
  consoleShown = false,
  onConsole,
  aboutShown = false,
  onAbout,
}: TopBarProps) {
  return (
    <header className="bar">
      <Wordmark running={running} />
      <div className="menubar-group" data-own-keys>
        <MenuBar commands={commands} />
        <button
          type="button"
          className="menu-title"
          aria-expanded={aboutShown}
          title="About Squero Studio"
          onClick={onAbout}
        >
          About
        </button>
      </div>
      {model && <ProjectLabel name={model} dirty={dirty} />}
      <span className="spacer" />
      <button type="button" className={`key small${errors ? " err" : ""}${consoleShown ? " on" : ""}`} aria-pressed={consoleShown} title="Open the Console" onClick={onConsole}>
        {errors ? `${errors} ${errors === 1 ? "error" : "errors"}` : "Console"}
      </button>
      <button
        type="button"
        className={`key small icon-key${side ? " on" : ""}`}
        aria-pressed={side}
        aria-label="Source & Simulate pane"
        title="Source & Simulate pane"
        onClick={onToggleSide}
      >
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <rect x="1.5" y="2.5" width="13" height="11" rx="1.5" />
          <path d="M10 2.5v11" />
          {side && <rect x="10" y="2.5" width="4.5" height="11" className="fill" />}
        </svg>
      </button>
      <span className={`bar-focus${showFocus ? "" : " off"}`} inert={!showFocus}>
        {focus}
      </span>
      <StatusPill
        label={chip(status)}
        labels={STATES.map((state) => chip({ ...status, state }))}
        tone={TONE[status.state]}
        title="OpenModelica: status and libraries"
        onClick={onStatusClick}
      />
      <ThemeToggle theme={theme} onTheme={onTheme} />
    </header>
  );
}
