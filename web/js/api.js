const base = location.origin;

export async function fetchManifest() {
  const res = await fetch(`${base}/api/manifest`);
  if (!res.ok) throw new Error(`manifest failed: ${res.status}`);
  return res.json();
}

/**
 * POST a message and consume the SSE reply.
 *
 * EventSource can only do GET, so we read the response body ourselves. The
 * frames are plain `event:`/`data:` pairs — same wire format, hand-parsed.
 *
 * @param {string} message
 * @param {object} opts
 * @param {string|null} [opts.sessionId]  keeps the Claude Code session alive
 * @param {(text: string) => void} [opts.onDelta]
 * @param {AbortSignal} [opts.signal]
 * @returns {Promise<{speech: string, motion: string|null, emotion: string|null, sessionId: string|null}>}
 */
export async function chat(message, { sessionId = null, onDelta, signal } = {}) {
  const res = await fetch(`${base}/api/chat`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ message, sessionId }),
    signal,
  });

  if (!res.ok || !res.body) {
    const detail = await res.text().catch(() => '');
    throw new Error(`chat failed: ${res.status} ${detail}`);
  }

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffer = '';
  let result = null;
  let failure = null;

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value, { stream: true });

    // Frames are separated by a blank line.
    let split;
    while ((split = buffer.indexOf('\n\n')) !== -1) {
      const frame = buffer.slice(0, split);
      buffer = buffer.slice(split + 2);

      let event = 'message';
      let data = '';
      for (const line of frame.split('\n')) {
        if (line.startsWith('event:')) event = line.slice(6).trim();
        else if (line.startsWith('data:')) data += line.slice(5).trim();
      }
      if (!data) continue;

      const payload = JSON.parse(data);
      if (event === 'delta') onDelta?.(payload.text);
      else if (event === 'done') result = payload;
      else if (event === 'error') failure = payload.message;
    }
  }

  if (failure) throw new Error(failure);
  if (!result) throw new Error('the bridge closed the stream without a reply');
  return result;
}
