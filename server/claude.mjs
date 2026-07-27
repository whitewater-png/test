// Two ways to give the character a brain:
//
//   1. The Claude Code CLI in headless mode (`claude -p --output-format stream-json`).
//      This is the interesting one — the character inherits Claude Code's whole
//      toolbelt, so "調べて" really does a web search, and it can read/edit files
//      in the project directory.
//   2. The Anthropic Messages API, as a fallback for machines with no CLI installed.
//
// Both expose the same `respond()` shape so the bridge doesn't care which is live.
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

const MODEL = 'claude-opus-5';

// ---------------------------------------------------------------- Claude Code

const CLI_BIN = process.env.MASCOT_CLAUDE_BIN ?? 'claude';
const CLI_ALLOWED_TOOLS =
  process.env.MASCOT_ALLOWED_TOOLS ?? 'WebSearch,WebFetch,Read,Glob,Grep';

/** Does the `claude` binary exist and run? Cached after the first probe. */
let cliProbe = null;
export function probeClaudeCli() {
  cliProbe ??= new Promise((resolve) => {
    const child = spawn(CLI_BIN, ['--version'], { stdio: 'ignore' });
    child.on('error', () => resolve(false));
    child.on('close', (code) => resolve(code === 0));
  });
  return cliProbe;
}

function runCli({ message, systemPrompt, sessionId, cwd, onDelta, signal }) {
  const args = [
    '-p',
    message,
    '--output-format',
    'stream-json',
    '--verbose',
    '--append-system-prompt',
    systemPrompt,
  ];
  if (CLI_ALLOWED_TOOLS) args.push('--allowedTools', CLI_ALLOWED_TOOLS);
  if (sessionId) args.push('--resume', sessionId);

  return new Promise((resolve, reject) => {
    const child = spawn(CLI_BIN, args, {
      cwd,
      signal,
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    let text = '';
    let nextSessionId = sessionId ?? null;
    let stderr = '';

    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString();
    });

    const lines = createInterface({ input: child.stdout });
    lines.on('line', (line) => {
      if (!line.trim()) return;
      let event;
      try {
        event = JSON.parse(line);
      } catch {
        return; // stream-json is one JSON object per line; ignore anything else
      }

      if (event.session_id) nextSessionId = event.session_id;

      if (event.type === 'assistant') {
        for (const block of event.message?.content ?? []) {
          if (block.type === 'text' && block.text) {
            text += block.text;
            onDelta?.(block.text);
          }
        }
      } else if (event.type === 'result') {
        // `result` carries the final answer; prefer it when we saw no stream.
        if (!text && typeof event.result === 'string') {
          text = event.result;
          onDelta?.(event.result);
        }
        if (event.is_error) {
          reject(new Error(event.result || 'Claude Code returned an error'));
        }
      }
    });

    child.on('error', reject);
    child.on('close', (code) => {
      if (code !== 0 && !text) {
        reject(new Error(`claude exited with code ${code}${stderr ? `: ${stderr.trim()}` : ''}`));
        return;
      }
      resolve({ text: text.trim(), sessionId: nextSessionId, backend: 'claude-code' });
    });
  });
}

// -------------------------------------------------------------- Messages API

const apiHistories = new Map(); // sessionId -> message[]
let sdkClient;

async function getSdkClient() {
  if (sdkClient) return sdkClient;
  const { default: Anthropic } = await import('@anthropic-ai/sdk');
  // Zero-arg constructor: resolves ANTHROPIC_API_KEY, ANTHROPIC_AUTH_TOKEN, or
  // an `ant auth login` profile, in that order.
  sdkClient = new Anthropic();
  return sdkClient;
}

async function runApi({ message, systemPrompt, sessionId, onDelta }) {
  const client = await getSdkClient();
  const id = sessionId ?? `api-${Date.now().toString(36)}`;
  const history = apiHistories.get(id) ?? [];
  history.push({ role: 'user', content: message });

  const stream = client.messages.stream({
    model: MODEL,
    max_tokens: 4096,
    // The mascot is a chat surface — favour latency over depth.
    output_config: { effort: 'low' },
    system: [{ type: 'text', text: systemPrompt, cache_control: { type: 'ephemeral' } }],
    messages: history,
  });

  stream.on('text', (delta) => onDelta?.(delta));
  const final = await stream.finalMessage();

  if (final.stop_reason === 'refusal') {
    history.pop();
    throw new Error('Claude declined to answer that one.');
  }

  const text = final.content
    .filter((block) => block.type === 'text')
    .map((block) => block.text)
    .join('')
    .trim();

  history.push({ role: 'assistant', content: final.content });
  // Keep the rolling window bounded; the mascot doesn't need yesterday's chat.
  apiHistories.set(id, history.slice(-40));

  return { text, sessionId: id, backend: 'messages-api' };
}

// ------------------------------------------------------------------ dispatch

/**
 * @param {object} opts
 * @param {'auto'|'cli'|'api'} [opts.backend]
 * @param {string} opts.message
 * @param {string} opts.systemPrompt
 * @param {string|null} [opts.sessionId]
 * @param {string} [opts.cwd]           project dir Claude Code should work in
 * @param {(delta: string) => void} [opts.onDelta]
 * @param {AbortSignal} [opts.signal]
 */
export async function respond(opts) {
  const backend = opts.backend ?? process.env.MASCOT_BACKEND ?? 'auto';

  if (backend === 'api') return runApi(opts);
  if (backend === 'cli') return runCli(opts);

  if (await probeClaudeCli()) {
    try {
      return await runCli(opts);
    } catch (err) {
      if (opts.signal?.aborted) throw err;
      console.warn(`[claude] CLI failed (${err.message}); falling back to the Messages API`);
    }
  }
  return runApi(opts);
}
