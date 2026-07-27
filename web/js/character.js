import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { VRMLoaderPlugin, VRMUtils } from '@pixiv/three-vrm';
import { VRMAnimationLoaderPlugin, VRMLookAtQuaternionProxy } from '@pixiv/three-vrm-animation';

const EMOTIONS = ['neutral', 'happy', 'angry', 'sad', 'relaxed', 'surprised'];

/** One loader that understands both .vrm and .vrma, shared with motion.js. */
export function createLoader() {
  const loader = new GLTFLoader();
  loader.register((parser) => new VRMLoaderPlugin(parser));
  loader.register((parser) => new VRMAnimationLoaderPlugin(parser));
  return loader;
}

export async function loadCharacter({ url, loader, scene }) {
  const gltf = await loader.loadAsync(url);
  const vrm = gltf.userData.vrm;
  if (!vrm) throw new Error(`${url} loaded, but it isn't a VRM file`);

  // Trim what we don't render. combineSkeletons in particular is a big win on
  // Blender exports, which tend to come out as many small skinned meshes.
  VRMUtils.removeUnnecessaryVertices(gltf.scene);
  VRMUtils.combineSkeletons(gltf.scene);
  VRMUtils.combineMorphs(vrm);

  // VRM 0.x faces +Z; VRM 1.0 faces -Z. Normalise so the camera rig below works
  // for either export.
  if (vrm.meta?.metaVersion === '0') VRMUtils.rotateVRM0(vrm);

  vrm.scene.traverse((obj) => {
    obj.frustumCulled = false; // springbones can push verts outside the bounds
  });

  // VRMA files can animate the look-at direction. That only works if a quaternion
  // proxy is present in the scene graph before the clip is built.
  const lookAtProxy = new VRMLookAtQuaternionProxy(vrm.lookAt);
  lookAtProxy.name = 'lookAtQuaternionProxy';
  vrm.scene.add(lookAtProxy);

  scene.add(vrm.scene);

  return new Character(vrm);
}

class Character {
  constructor(vrm) {
    this.vrm = vrm;
    this.expressions = vrm.expressionManager ?? null;

    this.lookAtEnabled = true;
    this._lookTarget = new THREE.Object3D();
    this._lookTarget.position.set(0, 1.35, 1.4);
    vrm.scene.add(this._lookTarget);
    if (vrm.lookAt) vrm.lookAt.target = this._lookTarget;

    this._blinkTimer = 1 + Math.random() * 3;
    this._blink = 0;

    this._emotion = 'neutral';
    this._emotionWeights = Object.fromEntries(EMOTIONS.map((e) => [e, 0]));
    this._emotionWeights.neutral = 1;
    this._emotionHold = 0;

    this._mouth = 0;
    this._mouthTarget = 0;
  }

  get scene() {
    return this.vrm.scene;
  }

  /** Available expression presets on this particular model. */
  get availableEmotions() {
    if (!this.expressions) return [];
    return EMOTIONS.filter((name) => this.expressions.getExpression(name));
  }

  /**
   * @param {string} name  one of EMOTIONS
   * @param {number} [holdSeconds] drift back to neutral after this long
   */
  setEmotion(name, holdSeconds = 6) {
    if (!EMOTIONS.includes(name)) return;
    this._emotion = name;
    this._emotionHold = name === 'neutral' ? 0 : holdSeconds;
  }

  /** 0…1 — drive this from audio amplitude for lip sync. */
  setMouthOpen(value) {
    this._mouthTarget = Math.min(Math.max(value, 0), 1);
  }

  setLookAtEnabled(enabled) {
    this.lookAtEnabled = enabled;
  }

  /** Aim the eyes at a point in normalised device coords (-1…1). */
  lookAtPointer(ndcX, ndcY, camera) {
    if (!this.lookAtEnabled) return;
    const target = new THREE.Vector3(ndcX, ndcY, 0.5).unproject(camera);
    this._lookTarget.position.lerp(this.vrm.scene.worldToLocal(target), 0.35);
  }

  update(delta) {
    this._updateBlink(delta);
    this._updateEmotion(delta);
    this._updateMouth(delta);
    this.vrm.update(delta);
  }

  _updateBlink(delta) {
    if (!this.expressions) return;
    this._blinkTimer -= delta;
    if (this._blinkTimer <= 0) {
      this._blink = 1;
      this._blinkTimer = 1.6 + Math.random() * 4;
    }
    this._blink = Math.max(0, this._blink - delta * 9);
    this.expressions.setValue('blink', this._blink);
  }

  _updateEmotion(delta) {
    if (!this.expressions) return;
    if (this._emotionHold > 0) {
      this._emotionHold -= delta;
      if (this._emotionHold <= 0) this._emotion = 'neutral';
    }
    for (const name of EMOTIONS) {
      const goal = name === this._emotion ? 1 : 0;
      const weight = THREE.MathUtils.damp(this._emotionWeights[name], goal, 6, delta);
      this._emotionWeights[name] = weight;
      if (name !== 'neutral') this.expressions.setValue(name, weight);
    }
  }

  _updateMouth(delta) {
    if (!this.expressions) return;
    this._mouth = THREE.MathUtils.damp(this._mouth, this._mouthTarget, 22, delta);
    this.expressions.setValue('aa', this._mouth);
  }
}
