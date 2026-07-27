// Speech synthesis through the operating system.
//
// Electron's Chromium is built without Google's speech services, so the
// renderer's `speechSynthesis` is silent inside the app even though it works
// fine in a normal browser. macOS ships `say`, which is better for us anyway:
// it is offline, it has real Japanese voices, and because we get a WAV back we
// can drive the mouth from the actual waveform instead of faking an envelope.
import { spawn } from 'node:child_process';
import { readFile, writeFile, rm, mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const MAX_CHARS = 800;

/** @returns {Promise<{ code: number, stdout: string, stderr: string }>} */
function run(command, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args);
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => (stdout += chunk));
    child.stderr.on('data', (chunk) => (stderr += chunk));
    child.on('error', reject);
    child.on('close', (code) => resolve({ code: code ?? 1, stdout, stderr }));
  });
}

/**
 * `say -v '?'` prints one voice per line:
 *   Kyoko               ja_JP    # こんにちは。私の名前は Kyoko です。
 */
async function listVoices() {
  const { code, stdout } = await run('say', ['-v', '?']);
  if (code !== 0) return [];
  return stdout
    .split('\n')
    .map((line) => line.match(/^(.+?)\s{2,}([a-z]{2}_[A-Z]{2})/))
    .filter(Boolean)
    .map(([, name, locale]) => ({ name: name.trim(), locale }));
}

let probe = null;

/** Cached capability probe: is `say` here, and which voice should we use? */
export function ttsStatus() {
  probe ??= (async () => {
    if (process.platform !== 'darwin') {
      return { available: false, reason: 'macOS 以外では say コマンドがありません', voices: [] };
    }
    let voices;
    try {
      voices = await listVoices();
    } catch {
      return { available: false, reason: 'say コマンドが見つかりません', voices: [] };
    }
    if (voices.length === 0) {
      return { available: false, reason: 'say が音声を返しませんでした', voices: [] };
    }

    // Prefer an explicitly configured voice, then any Japanese one, then whatever
    // the system default is.
    const wanted = process.env.MASCOT_VOICE;
    const japanese = voices.find((v) => v.locale.startsWith('ja'));
    const voice = wanted ?? japanese?.name ?? null;

    return { available: true, voice, voices: voices.map((v) => `${v.name} (${v.locale})`) };
  })();
  return probe;
}

/**
 * Synthesise `text` to a WAV buffer.
 * @param {string} text
 * @param {string} [voice] overrides the auto-detected voice
 * @returns {Promise<Buffer>}
 */
export async function synthesize(text, voice) {
  const status = await ttsStatus();
  if (!status.available) throw new Error(status.reason);

  const dir = await mkdtemp(join(tmpdir(), 'mascot-tts-'));
  const input = join(dir, 'text.txt');
  const output = join(dir, 'speech.wav');

  try {
    // Feed the text through a file rather than argv. A reply that happens to
    // start with a hyphen would otherwise be read as an option, and there is no
    // quoting to get wrong this way.
    await writeFile(input, text.slice(0, MAX_CHARS), 'utf8');

    const args = [];
    const chosen = voice ?? status.voice;
    if (chosen) args.push('-v', chosen);
    args.push('-f', input, '--file-format=WAVE', '--data-format=LEI16@22050', '-o', output);

    const { code, stderr } = await run('say', args);
    if (code !== 0) throw new Error(stderr.trim() || `say exited with ${code}`);

    return await readFile(output);
  } finally {
    await rm(dir, { recursive: true, force: true }).catch(() => {});
  }
}
