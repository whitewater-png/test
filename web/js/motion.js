import * as THREE from 'three';
import { createVRMAnimationClip } from '@pixiv/three-vrm-animation';

/**
 * Plays .vrma clips on the character, crossfading between them and dropping back
 * to idle when a one-shot finishes.
 *
 * Clips are fetched lazily — a motions folder with twenty dances in it shouldn't
 * cost twenty downloads before the character appears.
 */
export function createMotionPlayer({ character, loader, motions = [] }) {
  const mixer = new THREE.AnimationMixer(character.scene);
  const catalog = new Map(motions.map((m) => [m.name, m.url]));
  const clips = new Map(); // name -> AnimationClip
  const actions = new Map(); // name -> AnimationAction

  let current = null;
  let idleName = catalog.has('idle') ? 'idle' : null;
  let autoDance = null; // { min, max, timer, pool }

  async function clipFor(name) {
    if (clips.has(name)) return clips.get(name);
    const url = catalog.get(name);
    if (!url) throw new Error(`unknown motion: ${name}`);

    const gltf = await loader.loadAsync(url);
    const vrmAnimation = gltf.userData.vrmAnimations?.[0];
    if (!vrmAnimation) throw new Error(`${name} is not a VRM Animation (.vrma) file`);

    const clip = createVRMAnimationClip(vrmAnimation, character.vrm);
    clip.name = name;
    clips.set(name, clip);
    return clip;
  }

  function actionFor(name, clip) {
    let action = actions.get(name);
    if (!action) {
      action = mixer.clipAction(clip);
      actions.set(name, action);
    }
    return action;
  }

  /**
   * @param {string} name
   * @param {{ loop?: boolean, fade?: number }} [opts]
   */
  async function play(name, { loop = false, fade = 0.35 } = {}) {
    const clip = await clipFor(name);
    const action = actionFor(name, clip);

    action.reset();
    action.setLoop(loop ? THREE.LoopRepeat : THREE.LoopOnce, Infinity);
    action.clampWhenFinished = !loop;
    action.enabled = true;

    if (current && current !== action) {
      action.crossFadeFrom(current, fade, false).play();
    } else {
      action.fadeIn(fade).play();
    }
    current = action;

    // A dance that animates the head shouldn't also be tracking the cursor.
    character.setLookAtEnabled(loop && name === idleName);
    if (autoDance) autoDance.timer = randomDelay(autoDance);

    return new Promise((resolve) => {
      if (loop) return resolve();
      const onFinished = (event) => {
        if (event.action !== action) return;
        mixer.removeEventListener('finished', onFinished);
        resolve();
      };
      mixer.addEventListener('finished', onFinished);
    });
  }

  async function idle({ fade = 0.5 } = {}) {
    character.setLookAtEnabled(true);
    if (!idleName) {
      current?.fadeOut(fade);
      current = null;
      return;
    }
    await play(idleName, { loop: true, fade });
  }

  const randomDelay = ({ min, max }) => min + Math.random() * (max - min);

  /**
   * Break the silence: every `min`–`max` seconds, pick a dance and do it.
   * Pass `null` to switch it off.
   */
  function setAutoDance(range, pool = danceNames()) {
    if (!range || pool.length === 0) {
      autoDance = null;
      return;
    }
    autoDance = { min: range.min ?? 20, max: range.max ?? 40, pool, timer: 0 };
    autoDance.timer = randomDelay(autoDance);
  }

  /** Every motion that isn't the idle loop. */
  function danceNames() {
    return [...catalog.keys()].filter((n) => n !== idleName);
  }

  let busy = false;
  async function playThenIdle(name, opts) {
    busy = true;
    try {
      await play(name, opts);
      await idle();
    } finally {
      busy = false;
    }
  }

  function update(delta) {
    mixer.update(delta);
    if (!autoDance || busy) return;
    autoDance.timer -= delta;
    if (autoDance.timer > 0) return;
    const pick = autoDance.pool[Math.floor(Math.random() * autoDance.pool.length)];
    playThenIdle(pick).catch((err) => console.warn(`[motion] ${pick}: ${err.message}`));
  }

  return {
    mixer,
    update,
    play,
    playThenIdle,
    idle,
    setAutoDance,
    danceNames,
    get names() {
      return [...catalog.keys()];
    },
    get idleName() {
      return idleName;
    },
    set idleName(name) {
      idleName = catalog.has(name) ? name : null;
    },
    has: (name) => catalog.has(name),
  };
}
