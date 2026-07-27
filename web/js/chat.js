import { chat } from './api.js';

/**
 * Wires the DOM (bubble, composer, push-to-talk) to the character.
 *
 * Hold Space to talk; release to send. Typing a bare motion name plays it
 * locally without spending a round trip on Claude.
 */
export function createChat({ character, motions, voice, ears, elements }) {
  const { bubble, bubbleText, statusEl, statusText, composer, input } = elements;

  let sessionId = null;
  let inFlight = null;

  function setStatus(state, text = '') {
    statusEl.dataset.state = state;
    statusText.textContent = text;
  }

  function say(text) {
    bubbleText.textContent = text;
    bubble.hidden = !text;
  }

  let clearTimer = null;
  function sayFor(text, ms = 9000) {
    say(text);
    clearTimeout(clearTimer);
    if (text) clearTimer = setTimeout(() => say(''), ms);
  }

  async function react({ speech, motion, emotion }) {
    if (emotion) character.setEmotion(emotion);
    sayFor(speech);

    // Dance and talk at the same time — they're independent channels.
    const moves = motion && motions.has(motion) ? motions.playThenIdle(motion) : Promise.resolve();
    const talking = voice.speak(speech, (level) => character.setMouthOpen(level));
    await Promise.allSettled([moves, talking]);
  }

  async function send(message) {
    const text = message.trim();
    if (!text) return;

    // "ポケダンス" typed on its own: just dance.
    if (motions.has(text)) {
      sayFor(`${text} 踊るね！`, 4000);
      character.setEmotion('happy');
      await motions.playThenIdle(text);
      return;
    }

    inFlight?.abort();
    const controller = new AbortController();
    inFlight = controller;

    setStatus('thinking', '考え中…');
    let streamed = '';
    say('…');

    try {
      const result = await chat(text, {
        sessionId,
        signal: controller.signal,
        onDelta: (delta) => {
          streamed += delta;
          // Strip control tags while streaming so they never flash on screen.
          say(streamed.replace(/\[\[[^\]]*\]?\]?/g, ''));
        },
      });
      sessionId = result.sessionId ?? sessionId;
      setStatus('idle');
      await react(result);
    } catch (err) {
      if (controller.signal.aborted) return;
      setStatus('error', 'エラー');
      sayFor(`うまく話せなかった… (${err.message})`, 12000);
      setTimeout(() => setStatus('idle'), 4000);
    } finally {
      if (inFlight === controller) inFlight = null;
    }
  }

  composer?.addEventListener('submit', (event) => {
    event.preventDefault();
    const text = input.value;
    input.value = '';
    send(text);
  });

  // ---- push to talk -------------------------------------------------------

  let holding = false;
  const typing = () => document.activeElement === input;

  if (ears) {
    window.addEventListener('keydown', (event) => {
      if (event.code !== 'Space' || event.repeat || holding || typing()) return;
      event.preventDefault();
      holding = true;
      voice.stop();
      setStatus('listening', '聞いてるよ');
      ears.start((partial) => say(partial || '…'));
    });

    window.addEventListener('keyup', async (event) => {
      if (event.code !== 'Space' || !holding) return;
      event.preventDefault();
      holding = false;
      setStatus('idle');
      const transcript = await ears.stop();
      if (transcript) send(transcript);
      else say('');
    });
  }

  return { send, say: sayFor, setStatus, get sessionId() { return sessionId; } };
}
