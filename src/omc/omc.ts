// Studio's line to omc (#160 contracts §5): squero/0 over the Studio server's WebSocket
// (`paperboat squero` / `squero-om`). One call is one omc expression; the server checks it
// against its allow-list and answers in order. Status pushes drive the top-bar chip.

export type OmcState = 'ready' | 'checking' | 'failed';
/** `mode`: how omc runs, native | podman | docker (#2); "" before it starts. */
export type OmcStatus = {state: OmcState; version: string; mode?: string};

/** One message omc streamed to the Console: the same shape the server's message cards use. */
export type OmcLog = {level: 'error' | 'warning' | 'info'; text: string; file?: string; line?: number; column?: number};

type Reply = {id?: number; ok?: boolean; result?: unknown; error?: string; op?: string; stage?: string; fraction?: number; level?: OmcLog['level']; text?: string; file?: string; line?: number; column?: number} & Partial<OmcStatus>;
type Waiter = {resolve: (result: unknown) => void; reject: (error: Error) => void; progress?: (stage: string, fraction: number) => void};

/** The two calls the store makes; tests give it a fake. */
export type OmcCalls = Pick<OmcClient, 'call' | 'op'>;

export class OmcClient {
  status: OmcStatus = {state: 'checking', version: ''};
  private readonly ws: WebSocket;
  private readonly url: string;
  private readonly waiting = new Map<number, Waiter>();
  private next = 1;
  private closing = false;
  private readonly open: Promise<void>;

  private readonly onStatus: (status: OmcStatus) => void;
  private logListener: ((message: OmcLog) => void) | undefined;
  private logBuffer: OmcLog[] = [];

  constructor(url: string, onStatus: (status: OmcStatus) => void = () => {}) {
    this.onStatus = onStatus;
    this.url = url;
    this.ws = new WebSocket(url);
    this.open = new Promise((resolve, reject) => {
      this.ws.addEventListener('open', () => resolve(), {once: true});
      this.ws.addEventListener('error', () => reject(new Error(`no Studio server at ${url}`)), {once: true});
    });
    this.ws.addEventListener('message', event => this.receive(String(event.data)));
    this.ws.addEventListener('close', () => {
      for (const w of this.waiting.values()) w.reject(new Error('the Studio server went away'));
      this.waiting.clear();
      if (!this.closing) this.setStatus({state: 'failed', version: this.status.version});
    });
  }

  // omc's answer as text (`true`, `"…"`, `{…}`); rejects with omc's or the server's words.
  async call(expr: string): Promise<string> {
    return String(await this.send({op: 'omc', expr}));
  }

  // One of Studio's own squero/0 ops (open, save, icon, set_icon, generate, simulate); `progress`
  // hears the server's {op: "progress"} pushes for this call.
  async op<T = unknown>(op: string, args: Record<string, unknown> = {}, progress?: (stage: string, fraction: number) => void): Promise<T> {
    return (await this.send({op, ...args}, progress)) as T;
  }

  private async send(message: Record<string, unknown>, progress?: Waiter['progress']): Promise<unknown> {
    await this.open;
    const id = this.next++;
    return new Promise((resolve, reject) => {
      this.waiting.set(id, {resolve, reject, progress});
      this.ws.send(JSON.stringify({id, ...message}));
    });
  }

  /**
   * Stop a running simulation (its compile or its executable). It goes on a connection of its
   * own: this one waits for the run's reply, and the server answers one message at a time per connection.
   */
  cancel(): Promise<void> {
    return new Promise((resolve) => {
      const side = new WebSocket(this.url);
      const done = () => (side.close(), resolve());
      side.onopen = () => side.send(JSON.stringify({id: 0, op: 'cancel'}));
      side.onmessage = (event) => (JSON.parse(String(event.data)) as Reply).id === 0 && done();
      side.onerror = done;
      setTimeout(done, 5000);
    });
  }

  /** omc's drained messages, streamed by the server as they happen. */
  onLog(fn: (message: OmcLog) => void): void {
    this.logListener = fn;
    for (const message of this.logBuffer) fn(message);
    this.logBuffer = [];
  }

  close(): void {
    this.closing = true;
    this.ws.close();
  }

  private receive(text: string): void {
    let msg: Reply;
    try {
      msg = JSON.parse(text) as Reply;
    } catch {
      return;
    }
    if (msg.op === 'status' && msg.state) {
      this.setStatus({state: msg.state, version: msg.version ?? '', mode: msg.mode ?? ''});
      return;
    }
    if (msg.op === 'log' && msg.text !== undefined) {
      const log: OmcLog = {
        level: msg.level ?? 'info',
        text: msg.text,
        ...(msg.file ? {file: msg.file} : {}),
        ...(msg.line !== undefined ? {line: msg.line} : {}),
        ...(msg.column !== undefined ? {column: msg.column} : {}),
      };
      if (this.logListener) this.logListener(log);
      else if (this.logBuffer.length < 500) this.logBuffer.push(log); // before the panel subscribes
      return;
    }
    const waiter = msg.id === undefined ? undefined : this.waiting.get(msg.id);
    if (!waiter || msg.id === undefined) return;
    if (msg.op === 'progress') {
      waiter.progress?.(msg.stage ?? '', msg.fraction ?? 0);
      return;
    }
    this.waiting.delete(msg.id);
    if (msg.ok) waiter.resolve(msg.result ?? '');
    else waiter.reject(new Error(msg.error ?? 'omc call failed'));
  }

  private setStatus(status: OmcStatus): void {
    this.status = status;
    this.onStatus(status);
  }
}

// The top bar's chip (contracts §1): `omc 1.27.0 · ready`.
export function chip(status: OmcStatus): string {
  const version = /\d+\.\d+\.\d+/.exec(status.version)?.[0];
  // a container says so: `omc 1.27.1 · ready · podman`
  const where = status.mode && status.mode !== 'native' ? ` · ${status.mode}` : '';
  return `omc${version ? ` ${version}` : ''} · ${status.state}${where}`;
}
