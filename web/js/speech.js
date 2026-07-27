// Voice in and voice out.
//
// Output has three backends, tried in this order:
//
//   1. VOICEVOX at http://127.0.0.1:50021, if it's running
//   2. the bridge's /api/tts, which shells out to the OS (macOS `say`)
//   3. the renderer's own speechSynthesis
//
// The first two hand back real audio, so the mouth can be driven from the
// waveform. The third is a fallback that only exists for plain browsers —
// **inside Electron it is silent**, because Electron's Chromium ships without
// Google's speech services. That is exactly why backend 2 exists.

const VOICEVOX = 'http://127.0.0.1:50021';

let audioCtx = null;
const context = () => (audioCtx ??= new (window.AudioContext ?? window.webkitAudioContext)());

/**
 * Play decoded audio, reporting a 0…1 loudness every frame so the caller can
 * open and close the character's mouth in time with it.
 * @param {ArrayBuffer} data
 * @param {(level: number) => void} [onLevel]
 */
async function playWithLipSync(data, onLevel) {
  const ctx = context();
  if (ctx.state === 'suspended') await ctx.resume();

  const buffer = await ctx.decodeAudioData(data);
  const source = ctx.createBufferSource();
  source.buffer = buffer;

  const analyser = ctx.createAnalyser();
  analyser.fftSize = 512;
  source.connect(analyser);
  analyser.connect(ctx.destination);

  const samples = new Uint8Array(analyser.frequencyBinCount);
  let running = true;
  const pump = () => {
    if (!running) return;
    analyser.getByteTimeDomainData(samples);
    let peak = 0;
    for (const s of samples) peak = Math.max(peak, Math.abs(s - 128) / 128);
    onLevel?.(Math.min(1, peak * 2.6));
    requestAnimationFrame(pump);
  };

  return new Promise((resolve) => {
    source.onended = () => {
      running = false;
      onLevel?.(0);
      resolve();
    };
    source.start();
    pump();
  });
}

async function probeVoicevox() {
  try {
    const res = await fetch(`${VOICEVOX}/version`, { signal: AbortSignal.timeout(600) });
    return res.ok;
  } catch {
    return false;
  }
}

async function probeServerTts() {
  try {
    const res = await fetch('/api/tts/status', { signal: AbortSignal.timeout(4000) });
    return res.ok ? res.json() : { available: false, reason: `HTTP ${res.status}` };
  } catch (err) {
    return { available: false, reason: err.message };
  }
}

export function createVoice({ speaker = 1 } = {}) {
  let backend = 'browser';
  let serverVoice = null;

  const ready = (async () => {
    if (await probeVoicevox()) {
      backend = 'voicevox';
    } else {
      const status = await probeServerTts();
      if (status.available) {
        backend = 'server';
        serverVoice = status.voice;
      } else {
        console.warn(`[voice] OS speech unavailable: ${status.reason}`);
      }
    }
    console.log(`[voice] backend: ${backend}${serverVoice ? ` (${serverVoice})` : ''}`);
    return backend;
  })();

  async function speakVoicevox(text, onLevel) {
    const query = await fetch(
      `${VOICEVOX}/audio_query?speaker=${speaker}&text=${encodeURIComponent(text)}`,
      { method: 'POST' },
    ).then((r) => r.json());

    const wav = await fetch(`${VOICEVOX}/synthesis?speaker=${speaker}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(query),
    }).then((r) => r.arrayBuffer());

    return playWithLipSync(wav, onLevel);
  }

  async function speakServer(text, onLevel) {
    const res = await fetch(`/api/tts?text=${encodeURIComponent(text)}`);
    if (!res.ok) {
      const detail = await res.json().catch(() => ({}));
      throw new Error(detail.error ?? `tts failed: ${res.status}`);
    }
    return playWithLipSync(await res.arrayBuffer(), onLevel);
  }

  function speakBrowser(text, onLevel) {
    if (!('speechSynthesis' in window)) return Promise.resolve();

    const utterance = new SpeechSynthesisUtterance(text);
    utterance.lang = /[ぁ-んァ-ン一-龯]/.test(text) ? 'ja-JP' : 'en-US';
    const voice = speechSynthesis
      .getVoices()
      .find((v) => v.lang.startsWith(utterance.lang.slice(0, 2)));
    if (voice) utterance.voice = voice;

    return new Promise((resolve) => {
      // No audio stream to analyse here, so approximate the mouth with a
      // wobbling envelope for as long as the utterance is speaking.
      let running = true;
      const started = performance.now();
      const pump = () => {
        if (!running) return;
        const t = (performance.now() - started) / 1000;
        onLevel?.(0.32 + 0.28 * Math.sin(t * 17) + 0.14 * Math.sin(t * 6.3));
        requestAnimationFrame(pump);
      };

      const finish = () => {
        running = false;
        onLevel?.(0);
        resolve();
      };
      utterance.onend = finish;
      utterance.onerror = finish;

      speechSynthesis.cancel();
      speechSynthesis.speak(utterance);
      pump();
    });
  }

  let current = null;

  return {
    ready,
    get backend() {
      return backend;
    },
    /** @param {string} text @param {(level:number)=>void} [onLevel] */
    async speak(text, onLevel) {
      if (!text) return;
      await ready;
      this.stop();

      const attempt =
        backend === 'voicevox'
          ? speakVoicevox
          : backend === 'server'
            ? speakServer
            : speakBrowser;

      const task = attempt(text, onLevel).catch((err) => {
        console.warn(`[voice] ${backend} failed (${err.message}); using speechSynthesis`);
        return speakBrowser(text, onLevel);
      });

      current = task;
      await task;
      if (current === task) current = null;
    },
    stop() {
      if ('speechSynthesis' in window) speechSynthesis.cancel();
      current = null;
    },
  };
}

/**
 * Push-to-talk.
 *
 * Uses the Web Speech API, which needs Google's speech service — present in
 * Chrome, absent from Electron. Rather than failing silently on the first
 * spacebar hold, expose `unavailable` so the UI can say so out loud.
 */
export function createEars({ lang = 'ja-JP' } = {}) {
  const Recognition = window.SpeechRecognition ?? window.webkitSpeechRecognition;
  if (!Recognition) {
    return {
      supported: false,
      unavailable: 'このアプリでは音声入力が使えません（ブラウザで開くと使えます）',
      listening: false,
      start() {},
      stop: () => Promise.resolve(''),
    };
  }

  const recognition = new Recognition();
  recognition.lang = lang;
  recognition.interimResults = true;
  recognition.continuous = false;

  let onPartial = null;
  let settle = null;
  let transcript = '';
  let listening = false;
  let unavailable = null;

  recognition.addEventListener('result', (event) => {
    transcript = Array.from(event.results)
      .map((r) => r[0].transcript)
      .join('');
    onPartial?.(transcript);
  });
  recognition.addEventListener('end', () => {
    listening = false;
    settle?.(transcript.trim());
  });
  recognition.addEventListener('error', (event) => {
    // `network` / `service-not-allowed` is the Electron case: the API exists but
    // there is no speech service behind it.
    if (event.error === 'network' || event.error === 'service-not-allowed') {
      unavailable = 'このアプリでは音声入力が使えません（ブラウザで開くと使えます）';
    } else if (event.error === 'not-allowed') {
      unavailable = 'マイクの使用が許可されていません';
    }
    listening = false;
    settle?.(transcript.trim());
  });

  return {
    supported: true,
    get unavailable() {
      return unavailable;
    },
    get listening() {
      return listening;
    },
    /** @param {(partial: string) => void} [handler] */
    start(handler) {
      if (listening) return;
      onPartial = handler;
      transcript = '';
      listening = true;
      try {
        recognition.start();
      } catch {
        listening = false; // already running — harmless
      }
    },
    /** @returns {Promise<string>} the final transcript */
    stop() {
      if (!listening) return Promise.resolve('');
      return new Promise((resolve) => {
        settle = (text) => {
          settle = null;
          resolve(text);
        };
        recognition.stop();
      });
    },
  };
}
