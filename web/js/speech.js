// Voice in and voice out.
//
// Output prefers a local VOICEVOX server (http://127.0.0.1:50021) because it
// gives us real audio to analyse, which means real lip sync. Without it we fall
// back to the browser's speechSynthesis and fake the mouth from a simple
// envelope — the timing is close enough to read as talking.

const VOICEVOX = 'http://127.0.0.1:50021';

export async function probeVoicevox() {
  try {
    const res = await fetch(`${VOICEVOX}/version`, { signal: AbortSignal.timeout(600) });
    return res.ok;
  } catch {
    return false;
  }
}

export function createVoice({ speaker = 1 } = {}) {
  let audioCtx = null;
  let useVoicevox = false;
  let current = null;

  probeVoicevox().then((ok) => {
    useVoicevox = ok;
    console.log(`[voice] ${ok ? 'VOICEVOX' : 'speechSynthesis'}`);
  });

  const context = () => (audioCtx ??= new (window.AudioContext ?? window.webkitAudioContext)());

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

    const ctx = context();
    const buffer = await ctx.decodeAudioData(wav);
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

  function speakBrowser(text, onLevel) {
    if (!('speechSynthesis' in window)) return Promise.resolve();

    const utterance = new SpeechSynthesisUtterance(text);
    utterance.lang = /[ぁ-んァ-ン一-龯]/.test(text) ? 'ja-JP' : 'en-US';
    const voice = speechSynthesis.getVoices().find((v) => v.lang.startsWith(utterance.lang.slice(0, 2)));
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

  return {
    /** @param {string} text @param {(level:number)=>void} [onLevel] */
    async speak(text, onLevel) {
      if (!text) return;
      this.stop();
      const task = useVoicevox
        ? speakVoicevox(text, onLevel).catch(() => speakBrowser(text, onLevel))
        : speakBrowser(text, onLevel);
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
 * Push-to-talk. Uses the Web Speech API, which is available in Chrome and in
 * Electron builds with speech enabled; everywhere else the text box is the path.
 */
export function createEars({ lang = 'ja-JP' } = {}) {
  const Recognition = window.SpeechRecognition ?? window.webkitSpeechRecognition;
  if (!Recognition) return null;

  const recognition = new Recognition();
  recognition.lang = lang;
  recognition.interimResults = true;
  recognition.continuous = false;

  let onPartial = null;
  let settle = null;
  let transcript = '';

  recognition.addEventListener('result', (event) => {
    transcript = Array.from(event.results)
      .map((r) => r[0].transcript)
      .join('');
    onPartial?.(transcript);
  });
  recognition.addEventListener('end', () => settle?.(transcript.trim()));
  recognition.addEventListener('error', () => settle?.(transcript.trim()));

  let listening = false;

  return {
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
      listening = false;
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
