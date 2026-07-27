#!/usr/bin/env node
// Local bridge between the browser/Electron renderer and Claude.
//
//   GET  /                     the viewer
//   GET  /api/manifest         which .vrm and .vrma files are on disk
//   GET  /assets/models/<f>    a .vrm, streamed as-is
//   GET  /assets/motions/<f>   a .vrma, auto-repaired on the way out
//   POST /api/chat             {message, sessionId?} -> SSE: delta / done / error
import { createServer } from 'node:http';
import { readdir, readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { repairVrma } from './vrma.mjs';
import { respond, probeClaudeCli } from './claude.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const WEB = join(root, 'web');
const MODELS = join(root, 'models');
const MOTIONS = join(root, 'motions');
const PORT = Number(process.env.MASCOT_PORT ?? 4747);

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.vrm': 'model/gltf-binary',
  '.vrma': 'model/gltf-binary',
  '.glb': 'model/gltf-binary',
};

// VRM and VRMA files *are* .glb containers, and plenty of exporters emit them
// with the generic extension. Accept both rather than making people rename.
const MODEL_EXT = ['.vrm', '.glb'];
const MOTION_EXT = ['.vrma', '.glb'];

// The character can express these; they map onto VRM 1.0 expression presets.
export const EMOTIONS = ['neutral', 'happy', 'angry', 'sad', 'relaxed', 'surprised'];

async function listFiles(dir, extensions) {
  try {
    const entries = await readdir(dir);
    return entries.filter((f) => extensions.includes(extname(f).toLowerCase())).sort();
  } catch {
    return [];
  }
}

/** A motion file named `pokemon-dance.vrma` is offered to Claude as `pokemon-dance`. */
const motionName = (file) => basename(file, extname(file));

/**
 * Optional `motions/motions.json` — `{ "<motion name>": "<what it looks like>" }`.
 *
 * File names alone are a poor brief: `VRMA_03` tells Claude nothing, and even
 * `v-sign` doesn't say it's a 12-second pose routine. Descriptions go straight
 * into the system prompt so she can pick something that fits what was asked.
 */
async function loadDescriptions() {
  try {
    const raw = await readFile(join(MOTIONS, 'motions.json'), 'utf8');
    const parsed = JSON.parse(raw);
    return typeof parsed === 'object' && parsed !== null ? parsed : {};
  } catch (err) {
    if (err.code !== 'ENOENT') console.warn(`[motions] motions.json ignored: ${err.message}`);
    return {};
  }
}

function buildSystemPrompt(motions, descriptions = {}) {
  const list = motions.length
    ? motions
        .map((file) => {
          const name = motionName(file);
          const note = descriptions[name];
          return note ? `- ${name} … ${note}` : `- ${name}`;
        })
        .join('\n')
    : '- (motions フォルダが空です。踊れません)';

  return [
    'あなたはユーザーのデスクトップに住んでいる3Dキャラクターです。',
    'Blenderで作られたVRMアバターとして画面に立っていて、話しかけられたら声で返事をします。',
    '',
    '## 話し方',
    '- 短く、親しみやすく。基本は1〜3文。読み上げられる前提なので記号や箇条書きは避ける。',
    '- ユーザーの言語に合わせる（日本語で話しかけられたら日本語で返す）。',
    '- 調べ物や作業を頼まれたら、使えるツールを使って実際にやってから結果を話す。',
    '',
    '## 体を動かす',
    '返事の最後に、次のタグを必要な分だけ付けてください。タグは読み上げられず、キャラクターの制御に使われます。',
    '',
    '- `[[emotion:NAME]]` … 表情。NAME は次から選ぶ: ' + EMOTIONS.join(', '),
    '- `[[motion:NAME]]` … モーション再生。NAME は次から選ぶ:',
    list,
    '',
    '踊ってと言われたらモーションタグを付ける。普段の返事では表情タグだけで十分です。',
    '存在しないモーション名は絶対に使わないこと。',
  ].join('\n');
}

/** Pull the control tags out of a reply and return the speakable remainder. */
export function parseTags(text) {
  const motions = [];
  let emotion = null;
  const speech = text
    .replace(/\[\[\s*motion\s*:\s*([^\]]+?)\s*\]\]/gi, (_, name) => {
      motions.push(name);
      return '';
    })
    .replace(/\[\[\s*emotion\s*:\s*([^\]]+?)\s*\]\]/gi, (_, name) => {
      emotion = name.toLowerCase();
      return '';
    })
    .replace(/[ \t]+\n/g, '\n')
    .trim();

  return {
    speech,
    motion: motions[0] ?? null,
    motions,
    emotion: EMOTIONS.includes(emotion) ? emotion : null,
  };
}

function sendJson(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(payload),
  });
  res.end(payload);
}

/** Resolve a request path inside `dir`, refusing anything that escapes it. */
function safeJoin(dir, requested) {
  const decoded = decodeURIComponent(requested);
  const full = normalize(join(dir, decoded));
  return full.startsWith(dir + '/') || full === dir ? full : null;
}

async function serveStatic(res, file) {
  try {
    const info = await stat(file);
    if (info.isDirectory()) return false;
    const body = await readFile(file);
    res.writeHead(200, {
      'content-type': MIME[extname(file).toLowerCase()] ?? 'application/octet-stream',
      'content-length': body.byteLength,
      'cache-control': 'no-cache',
    });
    res.end(body);
    return true;
  } catch {
    return false;
  }
}

async function serveMotion(res, file) {
  let raw;
  try {
    raw = new Uint8Array(await readFile(file));
  } catch {
    return false;
  }
  let body = raw;
  try {
    const { buffer, fixes } = repairVrma(raw);
    body = buffer;
    if (fixes.length) console.log(`[vrma] repaired ${basename(file)}: ${fixes.join('; ')}`);
  } catch (err) {
    console.warn(`[vrma] could not inspect ${basename(file)}: ${err.message}`);
  }
  res.writeHead(200, {
    'content-type': 'model/gltf-binary',
    'content-length': body.byteLength,
    'cache-control': 'no-cache',
  });
  res.end(Buffer.from(body));
  return true;
}

function readBody(req, limit = 1_000_000) {
  return new Promise((resolveBody, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > limit) {
        reject(new Error('request body too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolveBody(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

async function handleChat(req, res) {
  let payload;
  try {
    payload = JSON.parse((await readBody(req)) || '{}');
  } catch (err) {
    sendJson(res, 400, { error: err.message });
    return;
  }

  const message = String(payload.message ?? '').trim();
  if (!message) {
    sendJson(res, 400, { error: 'message is required' });
    return;
  }

  res.writeHead(200, {
    'content-type': 'text/event-stream; charset=utf-8',
    'cache-control': 'no-cache',
    connection: 'keep-alive',
  });
  const send = (event, data) => res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);

  const controller = new AbortController();
  req.on('close', () => controller.abort());

  const [motions, descriptions] = await Promise.all([
    listFiles(MOTIONS, MOTION_EXT),
    loadDescriptions(),
  ]);

  try {
    const result = await respond({
      message,
      systemPrompt: buildSystemPrompt(motions, descriptions),
      sessionId: payload.sessionId ?? null,
      cwd: process.env.MASCOT_PROJECT_DIR ?? root,
      signal: controller.signal,
      onDelta: (delta) => send('delta', { text: delta }),
    });

    const parsed = parseTags(result.text);
    // Guard against Claude inventing a motion that isn't on disk.
    const known = new Set(motions.map(motionName));
    send('done', {
      ...parsed,
      motion: parsed.motion && known.has(parsed.motion) ? parsed.motion : null,
      sessionId: result.sessionId,
      backend: result.backend,
    });
  } catch (err) {
    if (!controller.signal.aborted) send('error', { message: err.message });
  } finally {
    res.end();
  }
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const path = url.pathname;

  if (req.method === 'POST' && path === '/api/chat') return handleChat(req, res);

  if (req.method === 'GET' && path === '/api/manifest') {
    const [models, motions, descriptions] = await Promise.all([
      listFiles(MODELS, MODEL_EXT),
      listFiles(MOTIONS, MOTION_EXT),
      loadDescriptions(),
    ]);
    return sendJson(res, 200, {
      models: models.map((f) => ({
        name: motionName(f),
        url: `/assets/models/${encodeURIComponent(f)}`,
      })),
      motions: motions.map((f) => ({
        name: motionName(f),
        description: descriptions[motionName(f)] ?? null,
        url: `/assets/motions/${encodeURIComponent(f)}`,
      })),
      emotions: EMOTIONS,
      backend: (await probeClaudeCli()) ? 'claude-code' : 'messages-api',
    });
  }

  if (req.method !== 'GET' && req.method !== 'HEAD') {
    return sendJson(res, 405, { error: 'method not allowed' });
  }

  if (path.startsWith('/assets/motions/')) {
    const file = safeJoin(MOTIONS, path.slice('/assets/motions/'.length));
    if (file && (await serveMotion(res, file))) return;
    return sendJson(res, 404, { error: 'motion not found' });
  }

  if (path.startsWith('/assets/models/')) {
    const file = safeJoin(MODELS, path.slice('/assets/models/'.length));
    if (file && (await serveStatic(res, file))) return;
    return sendJson(res, 404, { error: 'model not found' });
  }

  const file = safeJoin(WEB, path === '/' ? 'index.html' : path);
  if (file && (await serveStatic(res, file))) return;
  return sendJson(res, 404, { error: 'not found' });
});

if (import.meta.url === `file://${process.argv[1]}`) {
  server.listen(PORT, '127.0.0.1', async () => {
    const backend = (await probeClaudeCli()) ? 'Claude Code CLI' : 'Anthropic Messages API';
    console.log(`mascot bridge  → http://127.0.0.1:${PORT}`);
    console.log(`brain          → ${backend}`);
  });
}

export { server, PORT, buildSystemPrompt };
