import * as THREE from 'three';
import { fetchManifest } from './api.js';
import { createStage } from './stage.js';
import { createLoader, loadCharacter } from './character.js';
import { createMotionPlayer } from './motion.js';
import { createVoice, createEars } from './speech.js';
import { createChat } from './chat.js';

const el = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);

// `mascot` = transparent desktop overlay, `stage` = the windowed viewer.
const mode = params.get('mode') === 'mascot' ? 'mascot' : 'stage';
document.body.dataset.mode = mode;

const boot = el('boot');
const bootDetail = el('boot-detail');
const fail = (message) => {
  boot.hidden = false;
  boot.querySelector('h1').textContent = '起動できませんでした';
  bootDetail.textContent = message;
};

async function start() {
  const manifest = await fetchManifest().catch(() => {
    throw new Error('ブリッジサーバーに繋がりません。`npm run serve` を先に起動してください。');
  });

  if (manifest.models.length === 0) {
    throw new Error(
      'models/ に .vrm ファイルがありません。\nBlenderから書き出したVRMを models/ に置いてから再読み込みしてください。',
    );
  }

  const stage = createStage({ canvas: el('stage'), mode });
  const loader = createLoader();

  const wanted = params.get('model');
  const entry = manifest.models.find((m) => m.name === wanted) ?? manifest.models[0];
  bootDetail.textContent = `${entry.name} を読み込み中…`;

  const character = await loadCharacter({ url: entry.url, loader, scene: stage.scene });
  const motions = createMotionPlayer({ character, loader, motions: manifest.motions });

  stage.frame(mode === 'mascot' ? 'head' : 'full');
  stage.onUpdate((delta) => {
    motions.update(delta);
    character.update(delta);
  });

  await motions.idle().catch((err) => console.warn(`[motion] idle: ${err.message}`));

  // Idle fidgeting: every 20–40s the character picks a dance, exactly like the
  // reference clip. Add ?autodance=off to sit still.
  if (params.get('autodance') !== 'off') {
    motions.setAutoDance({ min: 20, max: 40 });
  }

  // Eyes follow the cursor.
  const pointer = new THREE.Vector2(0, 0);
  window.addEventListener('pointermove', (event) => {
    pointer.set(
      (event.clientX / window.innerWidth) * 2 - 1,
      -(event.clientY / window.innerHeight) * 2 + 1,
    );
  });
  stage.onUpdate(() => character.lookAtPointer(pointer.x, pointer.y, stage.camera));

  const voice = createVoice();
  const ears = createEars();

  const ui = createChat({
    character,
    motions,
    voice,
    ears,
    elements: {
      bubble: el('bubble'),
      bubbleText: el('bubble-text'),
      statusEl: el('status'),
      statusText: el('status-text'),
      composer: el('composer'),
      input: el('composer-input'),
    },
  });

  if (!ears && mode === 'stage') {
    el('hint').textContent = 'このブラウザは音声認識に未対応です。下の入力欄から話しかけてください';
  }

  // One button per motion, so you can trigger a dance without saying anything.
  const bar = el('motion-bar');
  for (const name of motions.danceNames()) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = name;
    button.addEventListener('click', () => ui.send(name));
    bar.append(button);
  }

  // In the transparent overlay window, clicks should pass through to whatever is
  // behind the character — except when the cursor is actually over her.
  if (mode === 'mascot' && window.mascot?.setInteractive) {
    const raycaster = new THREE.Raycaster();
    let interactive = null;
    stage.onUpdate(() => {
      raycaster.setFromCamera(pointer, stage.camera);
      const hit = raycaster.intersectObject(character.scene, true).length > 0;
      if (hit !== interactive) {
        interactive = hit;
        window.mascot.setInteractive(hit);
      }
    });
  }

  boot.hidden = true;
  console.log(`[mascot] ready — ${manifest.motions.length} motion(s), brain: ${manifest.backend}`);

  // Handy from the devtools console: mascot.play('pokedance'), mascot.send('こんにちは')
  window.mascotApi = { character, motions, stage, voice, ui, play: motions.playThenIdle, send: ui.send };
}

start().catch((err) => {
  console.error(err);
  fail(err.message);
});
