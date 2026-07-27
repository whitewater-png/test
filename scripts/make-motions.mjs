#!/usr/bin/env node
// Generates .vrma motion files from scratch — no purchase, no mocap, no
// third-party licence to worry about.
//
//   node scripts/make-motions.mjs            # write every motion into motions/
//   node scripts/make-motions.mjs idle wave  # just these
//   node scripts/make-motions.mjs --list     # what's on offer
//
// How this works
// --------------
// A .vrma is a .glb carrying a `VRMC_vrm_animation` extension: a T-posed
// humanoid skeleton, a map from VRM bone names to node indices, and ordinary
// glTF animation channels driving those nodes' rotations.
//
// Every node in the skeleton below has an identity rest rotation, which makes
// the retargeting math collapse to nothing: the loader rewrites each key as
// `parentWorldRotation * key * inverse(boneWorldRotation)`, and with identity
// rest rotations that is just `key`. So a rotation written here is applied
// verbatim as a delta from the target model's own T-pose — whatever its
// proportions. That is the whole trick to authoring these by hand.
import * as THREE from 'three';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT_DIR = join(root, 'motions');
const FPS = 30;

// ---------------------------------------------------------------- skeleton

// Local offsets in metres, T-pose, Y up. Only `hips.y` actually affects
// retargeting (it scales hip movement onto the target model); the rest is here
// so the file is a well-formed skeleton if you open it in Blender.
const SKELETON = {
  hips: { parent: null, offset: [0, 0.72, 0] },
  spine: { parent: 'hips', offset: [0, 0.1, 0] },
  chest: { parent: 'spine', offset: [0, 0.12, 0] },
  neck: { parent: 'chest', offset: [0, 0.16, 0] },
  head: { parent: 'neck', offset: [0, 0.06, 0] },

  leftShoulder: { parent: 'chest', offset: [0.03, 0.13, 0] },
  leftUpperArm: { parent: 'leftShoulder', offset: [0.07, 0, 0] },
  leftLowerArm: { parent: 'leftUpperArm', offset: [0.2, 0, 0] },
  leftHand: { parent: 'leftLowerArm', offset: [0.19, 0, 0] },

  rightShoulder: { parent: 'chest', offset: [-0.03, 0.13, 0] },
  rightUpperArm: { parent: 'rightShoulder', offset: [-0.07, 0, 0] },
  rightLowerArm: { parent: 'rightUpperArm', offset: [-0.2, 0, 0] },
  rightHand: { parent: 'rightLowerArm', offset: [-0.19, 0, 0] },

  leftUpperLeg: { parent: 'hips', offset: [0.07, -0.04, 0] },
  leftLowerLeg: { parent: 'leftUpperLeg', offset: [0, -0.33, 0] },
  leftFoot: { parent: 'leftLowerLeg', offset: [0, -0.32, 0] },
  leftToes: { parent: 'leftFoot', offset: [0, -0.05, 0.08] },

  rightUpperLeg: { parent: 'hips', offset: [-0.07, -0.04, 0] },
  rightLowerLeg: { parent: 'rightUpperLeg', offset: [0, -0.33, 0] },
  rightFoot: { parent: 'rightLowerLeg', offset: [0, -0.32, 0] },
  rightToes: { parent: 'rightFoot', offset: [0, -0.05, 0.08] },
};

// ------------------------------------------------------------------ poses

const sin = (t, cycles = 1, phase = 0) => Math.sin(2 * Math.PI * (cycles * t + phase));
const cos = (t, cycles = 1, phase = 0) => Math.cos(2 * Math.PI * (cycles * t + phase));

// Which axis actually bends which joint was measured by rendering single-pose
// .vrma files onto a real model — not derived on paper. Worth writing down,
// because VRM 0.x models get a 180° flip during retargeting and poking the
// bones directly gives you the opposite answer:
//
//   shoulder raise   leftUpperArm  +Z up      rightUpperArm  -Z up
//   elbow flex       leftLowerArm  -Y         rightLowerArm  +Y
//   knee flex        lowerLeg      -X (both)
//   head nod / turn  head          +X down    +Y to the model's left
//
/**
 * Arms out sideways is the rest pose, and nobody stands like that. Every motion
 * starts from here: arms down at the sides, elbows slightly bent, feet apart.
 *
 * Angles are degrees, `[x, y, z]`, applied in XYZ order.
 */
function relaxedPose() {
  return {
    leftUpperArm: [0, 0, -72],
    rightUpperArm: [0, 0, 72],
    leftLowerArm: [0, -14, 0],
    rightLowerArm: [0, 14, 0],
    leftHand: [0, 0, -4],
    rightHand: [0, 0, 4],
    leftUpperLeg: [0, 0, 2],
    rightUpperLeg: [0, 0, -2],
  };
}

/** Add `delta` degrees onto whatever `pose` already has for that bone. */
function add(pose, bone, delta) {
  const current = pose[bone] ?? [0, 0, 0];
  pose[bone] = [current[0] + delta[0], current[1] + delta[1], current[2] + delta[2]];
}

/**
 * Set one arm from two intuitive 0…1 dials, replacing whatever was there.
 *
 * `raise` deliberately tops out well below horizontal. An upper arm at 0° *is*
 * the T-pose, and a swing that sweeps through it twice a bar looks like a
 * scarecrow no matter how good the rest of the body is — the first cut of these
 * dances made exactly that mistake. Big visible movement should come from
 * `fold` (the elbow), which has the whole range to play with.
 */
function armSwing(pose, side, { raise = 0, fold = 0.2, twist = 0 } = {}) {
  const mirror = side === 'left' ? 1 : -1;
  const upper = 72 - 52 * raise; // 72° down … 20° down, never horizontal
  const elbow = 14 + 96 * fold;
  pose[`${side}UpperArm`] = [0, 0, -mirror * upper];
  pose[`${side}LowerArm`] = [0, -mirror * elbow, 0];
  pose[`${side}Hand`] = [0, -mirror * twist, -mirror * 4];
}

// ---------------------------------------------------------------- motions

// Each motion is sampled over `duration` seconds. `frame(t01)` receives
// normalised time 0…1 and returns { pose, hips, expressions }. Looping motions
// must line up at t=0 and t=1 — build them out of sin/cos and they will.

const MOTIONS = {
  idle: {
    duration: 5,
    loop: true,
    describe: '待機。呼吸と重心移動だけの小さな動き',
    frame(t) {
      const pose = relaxedPose();
      const breath = sin(t, 1); // one slow breath per loop
      const sway = sin(t, 0.5);

      add(pose, 'chest', [-1.6 * breath, 0, 0]);
      add(pose, 'spine', [-0.8 * breath, 0, 1.4 * sway]);
      add(pose, 'neck', [0.9 * breath, 1.5 * sway, 0]);
      add(pose, 'head', [0, 2.2 * sway, -1.2 * sway]);
      add(pose, 'leftShoulder', [0, 0, -1.5 * breath]);
      add(pose, 'rightShoulder', [0, 0, 1.5 * breath]);
      add(pose, 'leftUpperArm', [0, 0, -2 * breath]);
      add(pose, 'rightUpperArm', [0, 0, 2 * breath]);

      return { pose, hips: [0.012 * sway, 0.006 * breath, 0] };
    },
  },

  wave: {
    duration: 2.6,
    loop: false,
    describe: '挨拶。右手を上げて振る',
    frame(t) {
      const pose = relaxedPose();
      // Ease the arm up over the first 25%, hold, then drop it at the end.
      const raise = THREE.MathUtils.smoothstep(t, 0, 0.25) * (1 - THREE.MathUtils.smoothstep(t, 0.8, 1));
      const flap = sin(t, 3.5) * raise;

      add(pose, 'rightUpperArm', [0, 0, -128 * raise]);
      add(pose, 'rightLowerArm', [0, 26 * raise, -18 * raise + 9 * flap]);
      add(pose, 'rightHand', [0, 0, 18 * flap]);
      add(pose, 'chest', [0, -5 * raise, 0]);
      add(pose, 'neck', [-4 * raise, 0, 0]);
      add(pose, 'head', [-3 * raise, 4 * raise, 0]);

      return { pose, expressions: { happy: 0.65 * raise } };
    },
  },

  nod: {
    duration: 1.4,
    loop: false,
    describe: 'うなずく（はい）',
    frame(t) {
      const pose = relaxedPose();
      const swing = Math.sin(Math.PI * t) * sin(t, 2);
      add(pose, 'neck', [10 * swing, 0, 0]);
      add(pose, 'head', [12 * swing, 0, 0]);
      add(pose, 'chest', [3 * swing, 0, 0]);
      return { pose, expressions: { happy: 0.3 * Math.sin(Math.PI * t) } };
    },
  },

  shake: {
    duration: 1.6,
    loop: false,
    describe: '首を横に振る（いいえ）',
    frame(t) {
      const pose = relaxedPose();
      const swing = Math.sin(Math.PI * t) * sin(t, 2);
      add(pose, 'neck', [0, 14 * swing, 0]);
      add(pose, 'head', [0, 16 * swing, 2 * swing]);
      add(pose, 'chest', [0, 4 * swing, 0]);
      return { pose };
    },
  },

  dance: {
    duration: 4,
    loop: true,
    describe: '軽く踊る。2拍でステップ、腕を交互に上げる',
    frame(t) {
      const pose = relaxedPose();
      const beat = sin(t, 4); // four beats per loop
      const halfBeat = sin(t, 2);
      const bounce = Math.abs(sin(t, 4));

      // Weight shifts side to side; knees soak up the bounce.
      add(pose, 'hips', [0, 8 * halfBeat, 3 * halfBeat]);
      add(pose, 'spine', [2 * bounce, -4 * halfBeat, -3 * halfBeat]);
      add(pose, 'chest', [-3 * bounce, -5 * halfBeat, 0]);
      add(pose, 'neck', [2 * bounce, 4 * halfBeat, 0]);
      add(pose, 'head', [3 * bounce, 6 * halfBeat, -5 * halfBeat]);

      // Arms alternate, elbows folded so the hands stay up near the shoulders.
      // Keeping the upper arms below horizontal is what stops this reading as a
      // T-pose with the volume turned up.
      const up = (halfBeat + 1) / 2; // 0…1
      const down = 1 - up;
      armSwing(pose, 'left', { raise: 0.35 + 0.3 * up, fold: 0.3 + 0.55 * up, twist: 12 * beat });
      armSwing(pose, 'right', { raise: 0.35 + 0.3 * down, fold: 0.3 + 0.55 * down, twist: -12 * beat });

      // Knees flex on the down-beat so the bounce reads as weight, not floating.
      add(pose, 'leftUpperLeg', [4 * bounce, 0, 0]);
      add(pose, 'rightUpperLeg', [4 * bounce, 0, 0]);
      add(pose, 'leftLowerLeg', [-16 * bounce, 0, 0]);
      add(pose, 'rightLowerLeg', [-16 * bounce, 0, 0]);

      return {
        pose,
        hips: [0.03 * halfBeat, -0.045 * bounce, 0],
        expressions: { happy: 0.8 },
      };
    },
  },

  'dance-bouncy': {
    duration: 3.2,
    loop: true,
    describe: 'よく跳ねる元気なダンス。膝を深く、腕を高く',
    frame(t) {
      const pose = relaxedPose();
      const halfBeat = sin(t, 2);
      const bounce = Math.abs(sin(t, 4));
      const beat = sin(t, 4);
      const up = (halfBeat + 1) / 2;
      const down = 1 - up;

      add(pose, 'hips', [0, 10 * halfBeat, 5 * halfBeat]);
      add(pose, 'spine', [5 * bounce, -5 * halfBeat, -4 * halfBeat]);
      add(pose, 'chest', [-6 * bounce, -6 * halfBeat, 0]);
      add(pose, 'neck', [3 * bounce, 5 * halfBeat, 0]);
      add(pose, 'head', [5 * bounce, 8 * halfBeat, -7 * halfBeat]);

      // Hands pump hard, but the pumping lives in the elbows.
      armSwing(pose, 'left', { raise: 0.45 + 0.4 * up, fold: 0.25 + 0.7 * down, twist: 18 * beat });
      armSwing(pose, 'right', { raise: 0.45 + 0.4 * down, fold: 0.25 + 0.7 * up, twist: -18 * beat });

      // Deep knee flex, and the hips drop far enough that it reads as a jump.
      add(pose, 'leftUpperLeg', [10 * bounce, 0, 0]);
      add(pose, 'rightUpperLeg', [10 * bounce, 0, 0]);
      add(pose, 'leftLowerLeg', [-34 * bounce, 0, 0]);
      add(pose, 'rightLowerLeg', [-34 * bounce, 0, 0]);
      add(pose, 'leftFoot', [16 * bounce, 0, 0]);
      add(pose, 'rightFoot', [16 * bounce, 0, 0]);

      return {
        pose,
        hips: [0.035 * halfBeat, -0.085 * bounce, 0],
        expressions: { happy: 0.9 },
      };
    },
  },

  'dance-slow': {
    duration: 9,
    loop: true,
    describe: 'ゆったり揺れるダンス。ながら作業の横で流しておく用',
    frame(t) {
      const pose = relaxedPose();
      const sway = sin(t, 1);
      const drift = sin(t, 0.5);
      const rise = (sin(t, 1, -0.25) + 1) / 2; // 0…1, offset a quarter phase

      add(pose, 'hips', [0, 5 * sway, 4 * sway]);
      add(pose, 'spine', [0, -3 * sway, -4 * sway]);
      add(pose, 'chest', [-2 * rise, -3 * sway, -2 * sway]);
      add(pose, 'neck', [1.5 * rise, 3 * sway, 0]);
      add(pose, 'head', [1 * rise, 5 * sway, -6 * sway]);

      // Both arms drift together in a slow, narrow arc — nothing sudden.
      armSwing(pose, 'left', { raise: 0.16 + 0.2 * rise, fold: 0.22 + 0.16 * rise, twist: 8 * sway });
      armSwing(pose, 'right', { raise: 0.16 + 0.2 * rise, fold: 0.22 + 0.16 * rise, twist: -8 * sway });

      add(pose, 'leftLowerLeg', [-6 * rise, 0, 0]);
      add(pose, 'rightLowerLeg', [-6 * rise, 0, 0]);

      return { pose, hips: [0.03 * sway, -0.015 * rise, 0], expressions: { relaxed: 0.6 } };
    },
  },

  'dance-idol': {
    duration: 4,
    loop: true,
    describe: 'アイドル寄り。肘を高く構えて左右にステップ、首をかしげる',
    frame(t) {
      const pose = relaxedPose();
      const step = sin(t, 2);
      const beat = sin(t, 4);
      const bounce = Math.abs(sin(t, 4));
      const point = (sin(t, 2, -0.25) + 1) / 2; // which side is pointing

      add(pose, 'hips', [0, 7 * step, 6 * step]);
      add(pose, 'spine', [1 * bounce, -4 * step, -5 * step]);
      add(pose, 'chest', [-2 * bounce, -5 * step, -2 * step]);
      add(pose, 'neck', [1 * bounce, 5 * step, 0]);
      // The head tilt is what sells it — it lags the step by a quarter beat.
      add(pose, 'head', [2 * bounce, 7 * step, -11 * sin(t, 2, -0.12)]);

      // Hands stay up by the shoulders throughout; the accent is a flick of the
      // forearm, not a swing of the whole arm.
      armSwing(pose, 'left', { raise: 0.74 + 0.2 * point, fold: 0.72 + 0.26 * point, twist: 14 * beat });
      armSwing(pose, 'right', {
        raise: 0.74 + 0.2 * (1 - point),
        fold: 0.72 + 0.26 * (1 - point),
        twist: -14 * beat,
      });

      add(pose, 'leftUpperLeg', [3 * bounce, 0, 0]);
      add(pose, 'rightUpperLeg', [3 * bounce, 0, 0]);
      add(pose, 'leftLowerLeg', [-12 * bounce, 0, 0]);
      add(pose, 'rightLowerLeg', [-12 * bounce, 0, 0]);

      return {
        pose,
        hips: [0.028 * step, -0.03 * bounce, 0],
        expressions: { happy: 0.85 },
      };
    },
  },

  spin: {
    duration: 3,
    loop: true,
    describe: 'その場でくるっと一回転',
    frame(t) {
      const pose = relaxedPose();
      const turn = 360 * t; // a full revolution across the loop
      const lift = Math.sin(Math.PI * Math.min(1, t * 1.2));

      add(pose, 'hips', [0, turn, 0]);
      add(pose, 'leftUpperArm', [0, 0, 40 * lift]);
      add(pose, 'rightUpperArm', [0, 0, -40 * lift]);
      add(pose, 'leftLowerArm', [0, -20 * lift, 0]);
      add(pose, 'rightLowerArm', [0, 20 * lift, 0]);
      add(pose, 'spine', [0, 0, 3 * lift]);
      add(pose, 'head', [0, -12 * lift, 0]);

      return { pose, hips: [0, 0.02 * lift, 0] };
    },
  },
};

// ------------------------------------------------------------- glb writer

class GlbBuilder {
  constructor() {
    this.json = {
      asset: { version: '2.0', generator: 'vrm-desktop-mascot/make-motions' },
      extensionsUsed: ['VRMC_vrm_animation'],
      scene: 0,
      scenes: [{ nodes: [] }],
      nodes: [],
      accessors: [],
      bufferViews: [],
      buffers: [],
      animations: [],
    };
    this.chunks = [];
    this.byteLength = 0;
  }

  /** @returns {number} accessor index */
  addAccessor(data, type, { min, max } = {}) {
    const bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    // Every accessor here is float; glTF wants 4-byte alignment.
    const padding = (4 - (this.byteLength % 4)) % 4;
    if (padding) {
      this.chunks.push(new Uint8Array(padding));
      this.byteLength += padding;
    }

    this.json.bufferViews.push({ buffer: 0, byteOffset: this.byteLength, byteLength: bytes.byteLength });
    this.chunks.push(bytes);
    this.byteLength += bytes.byteLength;

    const components = { SCALAR: 1, VEC3: 3, VEC4: 4 }[type];
    this.json.accessors.push({
      bufferView: this.json.bufferViews.length - 1,
      componentType: 5126, // FLOAT
      count: data.length / components,
      type,
      ...(min ? { min, max } : {}),
    });
    return this.json.accessors.length - 1;
  }

  build() {
    this.json.buffers.push({ byteLength: this.byteLength });

    const bin = new Uint8Array(this.byteLength);
    let offset = 0;
    for (const chunk of this.chunks) {
      bin.set(chunk, offset);
      offset += chunk.byteLength;
    }

    const jsonBytes = new TextEncoder().encode(JSON.stringify(this.json));
    const jsonPadded = new Uint8Array((jsonBytes.byteLength + 3) & ~3).fill(0x20);
    jsonPadded.set(jsonBytes);
    const binPadded = new Uint8Array((bin.byteLength + 3) & ~3);
    binPadded.set(bin);

    const total = 12 + 8 + jsonPadded.byteLength + 8 + binPadded.byteLength;
    const out = new Uint8Array(total);
    const view = new DataView(out.buffer);

    view.setUint32(0, 0x46546c67, true); // 'glTF'
    view.setUint32(4, 2, true);
    view.setUint32(8, total, true);
    view.setUint32(12, jsonPadded.byteLength, true);
    view.setUint32(16, 0x4e4f534a, true); // 'JSON'
    out.set(jsonPadded, 20);
    const binHeader = 20 + jsonPadded.byteLength;
    view.setUint32(binHeader, binPadded.byteLength, true);
    view.setUint32(binHeader + 4, 0x004e4942, true); // 'BIN'
    out.set(binPadded, binHeader + 8);

    return out;
  }
}

// ------------------------------------------------------------- generation

const euler = new THREE.Euler();
const quaternion = new THREE.Quaternion();

function quatFromDegrees([x, y, z]) {
  euler.set(
    THREE.MathUtils.degToRad(x),
    THREE.MathUtils.degToRad(y),
    THREE.MathUtils.degToRad(z),
    'XYZ',
  );
  quaternion.setFromEuler(euler);
  return [quaternion.x, quaternion.y, quaternion.z, quaternion.w];
}

function buildVrma(name, motion) {
  const glb = new GlbBuilder();

  // 1. skeleton nodes, parents before children so indices resolve
  const nodeIndex = {};
  const ordered = Object.keys(SKELETON).sort((a, b) => depth(a) - depth(b));
  for (const bone of ordered) {
    glb.json.nodes.push({ name: bone, translation: SKELETON[bone].offset });
    nodeIndex[bone] = glb.json.nodes.length - 1;
  }
  for (const bone of ordered) {
    const parent = SKELETON[bone].parent;
    if (parent == null) continue;
    const node = glb.json.nodes[nodeIndex[parent]];
    (node.children ??= []).push(nodeIndex[bone]);
  }
  glb.json.scenes[0].nodes.push(nodeIndex.hips);

  // 2. sample the motion
  const frameCount = Math.round(motion.duration * FPS) + 1;
  const times = new Float32Array(frameCount);
  const rotations = new Map(); // bone -> number[]
  const hips = [];
  const expressionValues = new Map(); // name -> number[]

  for (let i = 0; i < frameCount; i++) {
    const t = i / (frameCount - 1);
    times[i] = t * motion.duration;

    const { pose = {}, hips: hipOffset = [0, 0, 0], expressions = {} } = motion.frame(t);

    for (const bone of Object.keys(SKELETON)) {
      const angles = pose[bone] ?? [0, 0, 0];
      if (!rotations.has(bone)) rotations.set(bone, []);
      rotations.get(bone).push(...quatFromDegrees(angles));
    }
    // Hip motion is relative to the rest position; the loader rescales it to
    // the target model's own hip height.
    hips.push(
      SKELETON.hips.offset[0] + hipOffset[0],
      SKELETON.hips.offset[1] + hipOffset[1],
      SKELETON.hips.offset[2] + hipOffset[2],
    );
    for (const [expression, value] of Object.entries(expressions)) {
      if (!expressionValues.has(expression)) expressionValues.set(expression, []);
      expressionValues.get(expression).push(value, 0, 0); // weight rides in .x
    }
  }

  const timeAccessor = glb.addAccessor(times, 'SCALAR', {
    min: [times[0]],
    max: [times[times.length - 1]],
  });

  const samplers = [];
  const channels = [];
  const sample = (outputAccessor, node, path) => {
    samplers.push({ input: timeAccessor, output: outputAccessor, interpolation: 'LINEAR' });
    channels.push({ sampler: samplers.length - 1, target: { node, path } });
  };

  for (const [bone, values] of rotations) {
    sample(glb.addAccessor(new Float32Array(values), 'VEC4'), nodeIndex[bone], 'rotation');
  }
  sample(glb.addAccessor(new Float32Array(hips), 'VEC3'), nodeIndex.hips, 'translation');

  // 3. expression drivers get their own dummy nodes
  const expressionNodes = {};
  for (const [expression, values] of expressionValues) {
    glb.json.nodes.push({ name: `expression:${expression}` });
    const index = glb.json.nodes.length - 1;
    expressionNodes[expression] = { node: index };
    glb.json.scenes[0].nodes.push(index);
    sample(glb.addAccessor(new Float32Array(values), 'VEC3'), index, 'translation');
  }

  glb.json.animations.push({ name, samplers, channels });

  glb.json.extensions = {
    VRMC_vrm_animation: {
      specVersion: '1.0',
      humanoid: {
        humanBones: Object.fromEntries(
          Object.keys(SKELETON).map((bone) => [bone, { node: nodeIndex[bone] }]),
        ),
      },
      ...(Object.keys(expressionNodes).length ? { expressions: { preset: expressionNodes } } : {}),
    },
  };

  return glb.build();
}

function depth(bone) {
  let d = 0;
  let cursor = SKELETON[bone].parent;
  while (cursor) {
    d += 1;
    cursor = SKELETON[cursor].parent;
  }
  return d;
}

// Exported so tools (and experiments) can build a .vrma from an arbitrary
// pose function without going through the CLI.
export { buildVrma, SKELETON, MOTIONS, relaxedPose, add, FPS };

// -------------------------------------------------------------------- cli

const invokedDirectly = import.meta.url === `file://${process.argv[1]}`;
if (!invokedDirectly) {
  // Imported as a library — skip everything below.
} else {
  await main();
}

async function main() {
const args = process.argv.slice(2);

if (args.includes('--list') || args.includes('-l')) {
  for (const [name, motion] of Object.entries(MOTIONS)) {
    const kind = motion.loop ? 'ループ' : '単発';
    console.log(`${name.padEnd(8)} ${String(motion.duration).padStart(4)}s  ${kind}  ${motion.describe}`);
  }
  return;
}

const wanted = args.filter((a) => !a.startsWith('-'));
const selected = wanted.length ? wanted : Object.keys(MOTIONS);

await mkdir(OUT_DIR, { recursive: true });

for (const name of selected) {
  const motion = MOTIONS[name];
  if (!motion) {
    console.error(`✗ unknown motion: ${name} (try --list)`);
    process.exitCode = 1;
    continue;
  }
  const file = join(OUT_DIR, `${name}.vrma`);
  const bytes = buildVrma(name, motion);
  await writeFile(file, bytes);
  console.log(`✓ ${name}.vrma  ${motion.duration}s  ${(bytes.byteLength / 1024).toFixed(1)}KB`);
}

console.log(`\n→ ${OUT_DIR}`);
}
