import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { VRMLoaderPlugin, VRMUtils } from '@pixiv/three-vrm';

function showErr(message) {
  const el = document.getElementById('err');
  if (el) { el.style.display = 'block'; el.textContent = 'VRM表示エラー:\n' + message + '\n(.vrm / .glb ファイルを確認してね)'; }
}
window.addEventListener('error', (e) => showErr(e.message || String(e.error || e)));
window.addEventListener('unhandledrejection', (e) => showErr(String(e.reason)));

// チャット状態(Swiftから __setState__ で更新される)
const uiState = { mood: 'idle', talking: false };
window.__setState__ = function (s) { if (s) Object.assign(uiState, s); };

async function start(getBytes) {
  try {
    const canvas = document.getElementById('c');
    const renderer = new THREE.WebGLRenderer({ canvas, alpha: true, antialias: true });
    renderer.setClearColor(0x000000, 0);
    renderer.setPixelRatio(window.devicePixelRatio || 1);
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(30, window.innerWidth / Math.max(1, window.innerHeight), 0.1, 100);
    scene.add(new THREE.HemisphereLight(0xffffff, 0x444455, 1.3));
    const dl = new THREE.DirectionalLight(0xffffff, 1.0); dl.position.set(0.5, 1.5, 2); scene.add(dl);
    function resize() { renderer.setSize(window.innerWidth, window.innerHeight, false); camera.aspect = window.innerWidth / Math.max(1, window.innerHeight); camera.updateProjectionMatrix(); }
    window.addEventListener('resize', resize); resize();

    let currentVRM = null, displayObject = null, mixer = null;
    const clock = new THREE.Clock();
    const loader = new GLTFLoader(); loader.register((p) => new VRMLoaderPlugin(p));

    let buffer;
    try { buffer = await getBytes(); } catch (e) { showErr('モデル取得エラー: ' + (e && e.message ? e.message : e)); return; }
    if (!buffer) { showErr('モデルデータが空です'); return; }

    // 表情プリセットの有無を調べておく(モデルによって持っていないため)
    const has = {};
    function detectExpressions(vrm) {
      const names = ['aa','ih','ou','ee','oh','blink','happy','relaxed','surprised','sad','angry','neutral'];
      const em = vrm.expressionManager;
      names.forEach((n) => { has[n] = !!(em && em.getExpression && em.getExpression(n)); });
    }
    function setExpr(name, value) { if (has[name] && currentVRM && currentVRM.expressionManager) currentVRM.expressionManager.setValue(name, value); }

    let headBone = null, baseRootY = Math.PI;

    loader.parse(buffer, '', (gltf) => {
      const vrm = gltf.userData.vrm;
      if (vrm) {
        currentVRM = vrm; displayObject = vrm.scene;
        try { VRMUtils.removeUnnecessaryVertices && VRMUtils.removeUnnecessaryVertices(gltf.scene); } catch (_) {}
        try { VRMUtils.combineSkeletons && VRMUtils.combineSkeletons(gltf.scene); } catch (_) {}
        scene.add(vrm.scene);
        if (vrm.humanoid) {
          const la = vrm.humanoid.getNormalizedBoneNode('leftUpperArm');
          const ra = vrm.humanoid.getNormalizedBoneNode('rightUpperArm');
          const a = THREE.MathUtils.degToRad(72);
          if (la) la.rotation.z = a; if (ra) ra.rotation.z = -a;
          headBone = vrm.humanoid.getNormalizedBoneNode('head');
        }
        detectExpressions(vrm);
        if (vrm.update) vrm.update(0);
      } else {
        displayObject = gltf.scene; scene.add(displayObject);
        if (gltf.animations && gltf.animations.length) { mixer = new THREE.AnimationMixer(displayObject); gltf.animations.forEach((c) => mixer.clipAction(c).play()); }
      }
      displayObject.rotation.y = baseRootY;
      displayObject.updateWorldMatrix(true, true);
      const box = new THREE.Box3().setFromObject(displayObject);
      const size = box.getSize(new THREE.Vector3());
      const center = box.getCenter(new THREE.Vector3());
      const dist = size.y * 1.95 + 0.4;
      camera.position.set(center.x, center.y, dist);
      camera.lookAt(center.x, center.y, center.z);
      if (window.__onReady__) window.__onReady__({ hasVRM: !!vrm });
    }, (e) => showErr('モデル読み込み失敗: ' + (e && e.message ? e.message : e)));

    // アニメーション状態
    let blinkTimer = 0, nextBlink = 2 + Math.random() * 3, blinking = false;
    let mouthCur = 0, mouthTarget = 0, mouthTimer = 0;
    let happyCur = 0, relaxCur = 0;

    function animate() {
      requestAnimationFrame(animate);
      const dt = Math.min(0.05, clock.getDelta());
      const t = performance.now() / 1000;
      const talking = !!uiState.talking;
      const thinking = uiState.mood === 'thinking';

      if (currentVRM) {
        // --- まばたき ---
        blinkTimer += dt;
        if (!blinking && blinkTimer > nextBlink) { blinking = true; setExpr('blink', 1); }
        else if (blinking && blinkTimer > nextBlink + 0.12) { blinking = false; setExpr('blink', 0); blinkTimer = 0; nextBlink = 2 + Math.random() * 3; }

        // --- 口パク(発話中は口を開閉。ランダムな目標へ滑らかに追従)---
        if (talking) {
          mouthTimer -= dt;
          if (mouthTimer <= 0) { mouthTarget = Math.random() * 0.8; mouthTimer = 0.06 + Math.random() * 0.08; }
        } else { mouthTarget = 0; }
        mouthCur += (mouthTarget - mouthCur) * Math.min(1, dt * 18);
        setExpr('aa', mouthCur);

        // --- 表情(発話中は少し笑顔、考え中はリラックス)---
        const happyTarget = talking ? 0.35 : 0.12;
        happyCur += (happyTarget - happyCur) * Math.min(1, dt * 4);
        setExpr('happy', happyCur);
        const relaxTarget = thinking ? 0.5 : 0.0;
        relaxCur += (relaxTarget - relaxCur) * Math.min(1, dt * 4);
        setExpr('relaxed', relaxCur);

        // --- 頭の動き(発話中はうなずき・首かしげ、考え中は少し上を向く)---
        if (headBone) {
          const s = talking ? 1 : 0.3;
          const lookUp = thinking ? -0.14 : 0;
          headBone.rotation.x = Math.sin(t * 6) * 0.045 * s + lookUp;
          headBone.rotation.y = Math.sin(t * 1.3) * 0.06 * s;
          headBone.rotation.z = Math.sin(t * 2.2) * 0.035 * s;
        }

        currentVRM.update(dt);
      }
      if (mixer) mixer.update(dt);

      // --- 体の揺れ(発話中は少し大きめ)+ 呼吸の上下動 ---
      if (displayObject) {
        displayObject.rotation.y = baseRootY + Math.sin(t * 1.1) * 0.04 * (talking ? 1 : 0.35);
        displayObject.position.y = Math.sin(t / 1.4) * 0.01;
      }

      renderer.render(scene, camera);
    }
    animate();
  } catch (e) { showErr(String(e && e.message ? e.message : e)); }
}
window.__startVRM__ = start;
