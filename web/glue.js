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

// マウスカーソルの方向(Swiftが __setPointer__ で更新)。
// x,y は -1..1 に正規化。x:右が正 / y:上が正。キャラが体と頭で追いかける。
const pointer = { x: 0, y: 0, ts: -1e9 };
window.__setPointer__ = function (p) { if (p) { pointer.x = p.x; pointer.y = p.y; pointer.ts = performance.now(); } };

// カーソルの見る向きの符号(実機で左右が逆に感じたらここを 1 に戻す)
const LOOK_SIGN = -1;

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const lerp = (a, b, t) => a + (b - a) * t;

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

    // 使うボーン群(無いモデルもあるので都度 null チェック)
    const bone = {};
    const baseArmZ = THREE.MathUtils.degToRad(72); // 腕を下ろす角度
    let baseRootY = Math.PI;

    loader.parse(buffer, '', (gltf) => {
      const vrm = gltf.userData.vrm;
      if (vrm) {
        currentVRM = vrm; displayObject = vrm.scene;
        try { VRMUtils.removeUnnecessaryVertices && VRMUtils.removeUnnecessaryVertices(gltf.scene); } catch (_) {}
        try { VRMUtils.combineSkeletons && VRMUtils.combineSkeletons(gltf.scene); } catch (_) {}
        scene.add(vrm.scene);
        if (vrm.humanoid) {
          const g = (n) => vrm.humanoid.getNormalizedBoneNode(n);
          bone.head = g('head'); bone.neck = g('neck'); bone.spine = g('spine'); bone.chest = g('chest');
          bone.hips = g('hips');
          bone.lUpArm = g('leftUpperArm'); bone.rUpArm = g('rightUpperArm');
          bone.lLoArm = g('leftLowerArm'); bone.rLoArm = g('rightLowerArm');
          bone.lUpLeg = g('leftUpperLeg'); bone.rUpLeg = g('rightUpperLeg');
          bone.lLoLeg = g('leftLowerLeg'); bone.rLoLeg = g('rightLowerLeg');
          if (bone.lUpArm) bone.lUpArm.rotation.z = baseArmZ;
          if (bone.rUpArm) bone.rUpArm.rotation.z = -baseArmZ;
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

    // --- 表情アニメーションの状態 ---
    let blinkTimer = 0, nextBlink = 2 + Math.random() * 3, blinking = false;
    let mouthCur = 0, mouthTarget = 0, mouthTimer = 0;
    let happyCur = 0, relaxCur = 0;

    // --- 待機中の「気まぐれ行動」の状態 ---
    // behavior: idle(揺れ) / look(キョロキョロ) / turn(振り向く) / walk(その場歩き) / wave(手を振る)
    let behavior = 'idle', behaviorTime = 0, behaviorDur = 3, ctxClass = '';
    let bodyYaw = 0, bodyYawTarget = 0;        // 体の向き(baseRootY への追加)
    let headYaw = 0, headYawTarget = 0;         // 頭の左右
    let headPitch = 0, headPitchTarget = 0;     // 頭の上下
    let lookH = 0, lookV = 0;                   // 'look' 行動での注視方向
    let walkAmt = 0, walkAmtTarget = 0, legPhase = 0; // 足踏み
    let waveAmt = 0, waveAmtTarget = 0;         // 手振り

    function pickIdleBehavior() {
      const r = Math.random();
      if (r < 0.34) { behavior = 'idle'; behaviorDur = 2.5 + Math.random() * 3; }
      else if (r < 0.56) { behavior = 'look'; behaviorDur = 2 + Math.random() * 2.5; lookH = (Math.random() * 2 - 1) * 0.5; lookV = (Math.random() * 2 - 1) * 0.22; }
      else if (r < 0.74) { behavior = 'turn'; behaviorDur = 3 + Math.random() * 3; bodyYawTarget = (Math.random() * 2 - 1) * Math.PI * 0.85; }
      else if (r < 0.90) { behavior = 'walk'; behaviorDur = 2.5 + Math.random() * 3; }
      else { behavior = 'wave'; behaviorDur = 2.2 + Math.random() * 1.3; }
      behaviorTime = 0;
    }

    // テスト用: 特定の行動を強制する(アプリでは未使用。ヘッドレス検証で使う)
    window.__debugForce__ = function (name, dur) { behavior = name; behaviorDur = dur || 6; behaviorTime = 0; if (name === 'turn') bodyYawTarget = Math.PI * 0.6; if (name === 'look') { lookH = 0.5; lookV = 0.2; } };

    function animate() {
      requestAnimationFrame(animate);
      const dt = Math.min(0.05, clock.getDelta());
      const t = performance.now() / 1000;
      const talking = !!uiState.talking;
      const thinking = uiState.mood === 'thinking';
      const pointerActive = (performance.now() - pointer.ts) < 600;

      // 状況クラスが変わったら行動を切り替える(会話中は歩かない等)
      const cls = talking ? 'talk' : thinking ? 'think' : pointerActive ? 'follow' : 'idle';
      if (cls !== ctxClass) { ctxClass = cls; behaviorTime = behaviorDur + 1; walkAmtTarget = 0; waveAmtTarget = 0; }

      behaviorTime += dt;
      if (behaviorTime > behaviorDur) {
        if (cls === 'idle') pickIdleBehavior();
        else { behavior = cls; behaviorDur = 1.5; behaviorTime = 0; }
      }

      // 行動ごとの目標値
      walkAmtTarget = (behavior === 'walk') ? 1 : 0;
      waveAmtTarget = (behavior === 'wave') ? 1 : 0;

      // 頭・体の目標の向きを状況で決める
      if (pointerActive) {
        // カーソルを体と頭で追いかける(頭を大きめ、体を控えめに)
        headYawTarget = clamp(pointer.x * LOOK_SIGN * 0.6, -0.7, 0.7);
        headPitchTarget = clamp(-pointer.y * 0.35, -0.35, 0.45);
        bodyYawTarget = clamp(pointer.x * LOOK_SIGN * 0.5, -0.6, 0.6);
      } else if (behavior === 'look') {
        headYawTarget = lookH; headPitchTarget = lookV;
      } else if (thinking) {
        headYawTarget = 0.12; headPitchTarget = -0.14;
      } else {
        headYawTarget = 0; headPitchTarget = 0;
      }

      // なめらかに追従
      const k = Math.min(1, dt * 3.5);
      bodyYaw = lerp(bodyYaw, bodyYawTarget, k);
      headYaw = lerp(headYaw, headYawTarget, Math.min(1, dt * 5));
      headPitch = lerp(headPitch, headPitchTarget, Math.min(1, dt * 5));
      walkAmt = lerp(walkAmt, walkAmtTarget, Math.min(1, dt * 4));
      waveAmt = lerp(waveAmt, waveAmtTarget, Math.min(1, dt * 5));

      if (currentVRM) {
        // --- まばたき ---
        blinkTimer += dt;
        if (!blinking && blinkTimer > nextBlink) { blinking = true; setExpr('blink', 1); }
        else if (blinking && blinkTimer > nextBlink + 0.12) { blinking = false; setExpr('blink', 0); blinkTimer = 0; nextBlink = 2 + Math.random() * 3; }

        // --- 口パク(発話中)---
        if (talking) { mouthTimer -= dt; if (mouthTimer <= 0) { mouthTarget = Math.random() * 0.8; mouthTimer = 0.06 + Math.random() * 0.08; } }
        else { mouthTarget = 0; }
        mouthCur += (mouthTarget - mouthCur) * Math.min(1, dt * 18);
        setExpr('aa', mouthCur);

        // --- 表情 ---
        const happyTarget = talking ? 0.35 : (behavior === 'wave' ? 0.5 : 0.12);
        happyCur += (happyTarget - happyCur) * Math.min(1, dt * 4);
        setExpr('happy', happyCur);
        const relaxTarget = thinking ? 0.5 : 0.0;
        relaxCur += (relaxTarget - relaxCur) * Math.min(1, dt * 4);
        setExpr('relaxed', relaxCur);

        // --- 頭(向き + 発話中のうなずき/首かしげを重ねる)---
        if (bone.head) {
          const s = talking ? 1 : 0.3;
          bone.head.rotation.x = headPitch + Math.sin(t * 6) * 0.045 * s;
          bone.head.rotation.y = headYaw + Math.sin(t * 1.3) * 0.05 * s;
          bone.head.rotation.z = Math.sin(t * 2.2) * 0.03 * s;
        }
        if (bone.neck) bone.neck.rotation.y = headYaw * 0.35;

        // --- 上半身の軽いひねり(体の向きに追従)---
        if (bone.spine) bone.spine.rotation.y = bodyYaw * 0.25;

        // --- 足踏み(その場歩き)+ 腕振り ---
        const walking = walkAmt > 0.02;
        if (walking) legPhase += dt * 7;
        const sw = Math.sin(legPhase) * walkAmt;
        if (bone.lUpLeg) bone.lUpLeg.rotation.x = 0.42 * sw;
        if (bone.rUpLeg) bone.rUpLeg.rotation.x = -0.42 * sw;
        if (bone.lLoLeg) bone.lLoLeg.rotation.x = Math.max(0, -sw) * 0.6 * walkAmt;
        if (bone.rLoLeg) bone.rLoLeg.rotation.x = Math.max(0, sw) * 0.6 * walkAmt;

        // 腕: 基本は下ろした姿勢。歩行中は前後に小さく振る
        let lArmZ = baseArmZ, rArmZ = -baseArmZ, lArmX = -0.16 * sw, rArmX = 0.16 * sw;
        // 手を振る: 右腕を上げてヒラヒラ
        if (waveAmt > 0.01) {
          rArmZ = lerp(-baseArmZ, -0.35, waveAmt);
          rArmX = lerp(rArmX, -0.2, waveAmt);
          if (bone.rLoArm) bone.rLoArm.rotation.z = -0.4 + Math.sin(t * 10) * 0.4 * waveAmt;
        } else if (bone.rLoArm) {
          bone.rLoArm.rotation.z = lerp(bone.rLoArm.rotation.z || 0, 0, Math.min(1, dt * 5));
        }
        if (bone.lUpArm) { bone.lUpArm.rotation.z = lArmZ; bone.lUpArm.rotation.x = lArmX; }
        if (bone.rUpArm) { bone.rUpArm.rotation.z = rArmZ; bone.rUpArm.rotation.x = rArmX; }

        currentVRM.update(dt);
      }
      if (mixer) mixer.update(dt);

      // --- ルート: 体の向き + 待機の揺れ + 呼吸/足踏みの上下動 ---
      if (displayObject) {
        const sway = Math.sin(t * 1.1) * 0.03 * (talking ? 1 : 0.4);
        displayObject.rotation.y = baseRootY + bodyYaw + sway;
        const bob = Math.sin(t / 1.4) * 0.008;                 // 呼吸
        const step = walkAmt * Math.abs(Math.sin(legPhase)) * 0.012; // 足踏みの弾み
        displayObject.position.y = bob + step;
      }

      renderer.render(scene, camera);
    }
    animate();
  } catch (e) { showErr(String(e && e.message ? e.message : e)); }
}
window.__startVRM__ = start;
