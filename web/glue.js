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

// カーソルの見る向きの符号(実機で左右が逆に感じたらここを反転)
const LOOK_SIGN = 1;

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const lerp = (a, b, t) => a + (b - a) * t;

async function start(getBytes) {
  try {
    const canvas = document.getElementById('c');
    const renderer = new THREE.WebGLRenderer({ canvas, alpha: true, antialias: true });
    renderer.setClearColor(0x000000, 0);
    renderer.setPixelRatio(window.devicePixelRatio || 1);
    renderer.toneMappingExposure = 1.15;   // 全体を少し明るめに
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(30, window.innerWidth / Math.max(1, window.innerHeight), 0.1, 100);
    // 明るく柔らかい照明: 空/地面の環境光を強めつつ、正面フィルトで影を起こす
    scene.add(new THREE.HemisphereLight(0xffffff, 0x8a8f99, 2.0));
    const dl = new THREE.DirectionalLight(0xffffff, 1.5); dl.position.set(0.5, 1.5, 2); scene.add(dl);
    const fill = new THREE.DirectionalLight(0xffffff, 0.7); fill.position.set(-0.6, 0.6, 1.5); scene.add(fill);
    scene.add(new THREE.AmbientLight(0xffffff, 0.35));
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
    let modelHeight = 1.5; // 座る時の沈み込み量などに使う(読み込み後に実測値へ)

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
          bone.lFoot = g('leftFoot'); bone.rFoot = g('rightFoot');
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
      modelHeight = size.y;
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
    let sitAmt = 0;                    // 0=立つ 1=座る(なめらかに遷移)
    let waveAmt = 0, waveUntil = -1e9; // 大きく手を振る(時間指定)

    // メニューからの「しぐさ」指示(Swiftが __gesture__ を呼ぶ)
    // 'sit'=座る / 'stand'=立つ / 'wave'=頭上で大きく手を振る
    let poseMode = 'stand';
    window.__gesture__ = function (name) {
      if (name === 'sit') poseMode = 'sit';
      else if (name === 'stand') poseMode = 'stand';
      else if (name === 'wave') waveUntil = performance.now() + 3200;
    };

    function pickIdleBehavior() {
      const r = Math.random();
      if (r < 0.36) { behavior = 'idle'; behaviorDur = 2.5 + Math.random() * 3; }
      else if (r < 0.60) { behavior = 'look'; behaviorDur = 2 + Math.random() * 2.5; lookH = (Math.random() * 2 - 1) * 0.5; lookV = (Math.random() * 2 - 1) * 0.22; }
      else if (r < 0.80) { behavior = 'turn'; behaviorDur = 3 + Math.random() * 3; bodyYawTarget = (Math.random() * 2 - 1) * Math.PI * 0.85; }
      else { behavior = 'walk'; behaviorDur = 2.5 + Math.random() * 3; }
      behaviorTime = 0;
    }

    // テスト用: 特定の行動を強制する(アプリでは未使用。ヘッドレス検証で使う)
    window.__debugForce__ = function (name, dur) { behavior = name; behaviorDur = dur || 6; behaviorTime = 0; if (name === 'turn') bodyYawTarget = Math.PI * 0.6; if (name === 'look') { lookH = 0.5; lookV = 0.2; } };
    window.__setYaw__ = function (v) { bodyYawTarget = v; }; // テスト用: 体の向きを固定(横から確認)
    // テスト用: 左足のワールド座標と遊脚判定(歩行の前後方向を数値で検証する)
    window.__footInfo__ = function () {
      const v = new THREE.Vector3();
      if (bone.lFoot) bone.lFoot.getWorldPosition(v);
      return { z: v.z, x: v.x, cw: Math.cos(legPhase), sw: Math.sin(legPhase) };
    };
    // テスト用: 左脚(股関節/膝/足首)のワールド座標。膝の曲がる向きの検証に使う
    window.__legGeo__ = function () {
      const hip = new THREE.Vector3(), knee = new THREE.Vector3(), ank = new THREE.Vector3();
      if (bone.lUpLeg) bone.lUpLeg.getWorldPosition(hip);
      if (bone.lLoLeg) bone.lLoLeg.getWorldPosition(knee);
      if (bone.lFoot) bone.lFoot.getWorldPosition(ank);
      return { hip: [hip.y, hip.z], knee: [knee.y, knee.z], ank: [ank.y, ank.z], cw: Math.cos(legPhase), sw: Math.sin(legPhase) };
    };

    function animate() {
      requestAnimationFrame(animate);
      const dt = Math.min(0.05, clock.getDelta());
      const t = performance.now() / 1000;
      const talking = !!uiState.talking;
      const thinking = uiState.mood === 'thinking';
      const pointerActive = (performance.now() - pointer.ts) < 600;

      // 状況クラスが変わったら行動を切り替える(会話中は歩かない等)
      const cls = talking ? 'talk' : thinking ? 'think' : pointerActive ? 'follow' : 'idle';
      if (cls !== ctxClass) { ctxClass = cls; behaviorTime = behaviorDur + 1; walkAmtTarget = 0; }

      behaviorTime += dt;
      if (behaviorTime > behaviorDur) {
        if (cls === 'idle') pickIdleBehavior();
        else { behavior = cls; behaviorDur = 1.5; behaviorTime = 0; }
      }

      // しぐさ(座る/手を振る)の量をなめらかに更新
      sitAmt = lerp(sitAmt, poseMode === 'sit' ? 1 : 0, Math.min(1, dt * 4));
      const waving = performance.now() < waveUntil;
      waveAmt = lerp(waveAmt, waving ? 1 : 0, Math.min(1, dt * 6));

      // 行動ごとの目標値(座っている間は歩かない)
      walkAmtTarget = (behavior === 'walk' && sitAmt < 0.5) ? 1 : 0;

      // 頭・体の目標の向きを状況で決める
      if (pointerActive) {
        // カーソルを体と頭で追いかける(頭を大きめ、体を控えめに)。
        // 縦は「カーソルが上→見上げる」になるよう pointer.y をそのまま使う。
        headYawTarget = clamp(pointer.x * LOOK_SIGN * 0.6, -0.7, 0.7);
        headPitchTarget = clamp(pointer.y * 0.4, -0.4, 0.45);
        bodyYawTarget = clamp(pointer.x * LOOK_SIGN * 0.5, -0.6, 0.6);
      } else if (behavior === 'look') {
        headYawTarget = lookH; headPitchTarget = lookV;
      } else if (thinking) {
        headYawTarget = 0.12; headPitchTarget = 0.12;
      } else {
        headYawTarget = 0; headPitchTarget = 0;
      }

      // なめらかに追従
      const k = Math.min(1, dt * 3.5);
      bodyYaw = lerp(bodyYaw, bodyYawTarget, k);
      headYaw = lerp(headYaw, headYawTarget, Math.min(1, dt * 5));
      headPitch = lerp(headPitch, headPitchTarget, Math.min(1, dt * 5));
      walkAmt = lerp(walkAmt, walkAmtTarget, Math.min(1, dt * 4));

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
        const happyTarget = waveAmt > 0.1 ? 0.6 : (talking ? 0.35 : 0.12);
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

        // --- 脚: 歩行サイクル を あぐら(座り)とブレンドする ---
        const walking = walkAmt > 0.02;
        if (window.__legLock__ !== undefined) legPhase = window.__legLock__; // テスト用: 位相固定
        else if (walking) legPhase += dt * 5.5;              // 歩調(少しゆっくり)
        const a = legPhase;
        const sw = Math.sin(a) * walkAmt;
        const cwALK = Math.cos(a) * walkAmt;
        const swingL = Math.max(0, -Math.sin(a)) * walkAmt;  // 左脚の遊脚量(0..1) 中央でピーク
        const swingR = Math.max(0,  Math.sin(a)) * walkAmt;  // 右脚の遊脚量
        // 人間の歩行に近づける:
        //  ・太もも: 前後スイング(接地で前=正) + 遊脚でわずかに持ち上げ(足が地面をこする程度)
        //  ・膝: 遊脚で自然に曲げ(膝が山なり)、接地では伸ばす
        //  ・足首: 前接地でつま先を上げかかとから / 蹴り出しでつま先立ち
        // 膝は負方向でふくらはぎが後ろに畳まれる(実測で確定)。正だと膝が逆に曲がる。
        const THIGH = 0.42, KNEE = 1.0, LIFT = 0.12, ANKLE = 0.5;
        const walkThighL =  THIGH * cwALK + LIFT * swingL;
        const walkThighR = -THIGH * cwALK + LIFT * swingR;
        const walkShinL = -KNEE * swingL, walkShinR = -KNEE * swingR;
        const walkAnkleL =  ANKLE * cwALK, walkAnkleR = -ANKLE * cwALK;

        // ぺたん座り(割座/W-sit): 太ももを前へ+内旋、膝を深く曲げてすねを後ろ+外へ開く
        const sp = window.__sitp__ || {};
        const stx = (sp.thighX !== undefined ? sp.thighX : 1.5);   // 前傾(flex)大きめで太ももを寝かせお尻を床へ
        const stz = (sp.thighZ !== undefined ? sp.thighZ : 0.15);  // 外転(膝の開き)
        const sty = (sp.thighY !== undefined ? sp.thighY : -0.7);  // 内旋(すねを外へ送る)
        const ssx = (sp.shinX  !== undefined ? sp.shinX  : -3.2);  // 膝を深く曲げ、足裏をお尻へ・すねを床に寝かせる
        const ssy = (sp.shinY  !== undefined ? sp.shinY  : -0.5);  // すねを外側へ開く
        const ssz = (sp.shinZ  !== undefined ? sp.shinZ  : 0.0);   // すねのひねり(微調整)
        if (bone.lUpLeg) bone.lUpLeg.rotation.set(lerp(walkThighL, stx, sitAmt), lerp(0,  sty, sitAmt), lerp(0,  stz, sitAmt));
        if (bone.rUpLeg) bone.rUpLeg.rotation.set(lerp(walkThighR, stx, sitAmt), lerp(0, -sty, sitAmt), lerp(0, -stz, sitAmt));
        if (bone.lLoLeg) bone.lLoLeg.rotation.set(lerp(walkShinL, ssx, sitAmt), lerp(0,  ssy, sitAmt), lerp(0,  ssz, sitAmt));
        if (bone.rLoLeg) bone.rLoLeg.rotation.set(lerp(walkShinR, ssx, sitAmt), lerp(0, -ssy, sitAmt), lerp(0, -ssz, sitAmt));
        if (bone.lFoot) bone.lFoot.rotation.x = lerp(walkAnkleL, 0, sitAmt);
        if (bone.rFoot) bone.rFoot.rotation.x = lerp(walkAnkleR, 0, sitAmt);

        // 骨盤と上体: 歩行の重心移動・ひねりが人間らしさの要。座り時は前傾のみ。
        const sitHipsX = (sp.hips !== undefined ? sp.hips : 0.0);
        const sitLean  = (sp.lean !== undefined ? sp.lean : 0.06);
        if (bone.hips) bone.hips.rotation.set(lerp(0, sitHipsX, sitAmt), -0.10 * cwALK, 0.05 * sw);
        if (bone.spine) bone.spine.rotation.set(lerp(0, sitLean, sitAmt), bodyYaw * 0.25 + 0.06 * cwALK, 0);
        if (bone.chest) bone.chest.rotation.y = 0.04 * cwALK; // 肩を骨盤と逆にひねる

        // --- 腕: 通常は下ろした姿勢(歩行中は左右対称に前後へ小さく振る)。
        //     手を振る時は右腕を頭上へ上げて大きく振る ---
        // 腕は同じ側の脚と逆位相で振る(前脚のとき腕は後ろ)
        let lArmZ = baseArmZ, lArmX = -0.3 * cwALK;
        let rArmZ = -baseArmZ, rArmX = 0.3 * cwALK;
        if (waveAmt > 0.01) {
          // 右腕を頭上へ上げる(z を大きく) + 左右へ大きく振る
          const swing = Math.sin(t * 9) * 0.32;
          rArmZ = lerp(rArmZ, 1.35 + swing, waveAmt);
          rArmX = lerp(rArmX, -0.1, waveAmt);
          if (bone.rLoArm) bone.rLoArm.rotation.z = lerp(bone.rLoArm.rotation.z || 0, -0.25, waveAmt);
        } else if (bone.rLoArm) {
          bone.rLoArm.rotation.z = lerp(bone.rLoArm.rotation.z || 0, 0, Math.min(1, dt * 6));
        }
        // 座り時は両手を前(両足の間)へ寄せて床につく感じにする。
        // 腕は下ろしたまま(z=baseArmZ)、前へ屈曲(x)+内側へ(y)。
        // 座り時は両手を膝(太ももの前)の上に置く。
        const saX  = (sp.armX  !== undefined ? sp.armX  : 0.85); // 肩の屈曲(前へ=膝へ)
        const saY  = (sp.armY  !== undefined ? sp.armY  : 0.0);  // 左右(膝幅に合わせて中央寄せは控えめ)
        const saLo = (sp.loArm !== undefined ? sp.loArm : 0.6);  // 肘を曲げて前腕を膝に載せる
        if (bone.lUpArm) bone.lUpArm.rotation.set(lerp(lArmX, saX, sitAmt), lerp(0,  saY, sitAmt), lerp(lArmZ,  baseArmZ, sitAmt));
        if (bone.rUpArm) bone.rUpArm.rotation.set(lerp(rArmX, saX, sitAmt), lerp(0, -saY, sitAmt), lerp(rArmZ, -baseArmZ, sitAmt));
        if (bone.lLoArm) bone.lLoArm.rotation.x = lerp(0, saLo, sitAmt);
        if (bone.rLoArm && waveAmt <= 0.01) bone.rLoArm.rotation.x = lerp(0, saLo, sitAmt);

        currentVRM.update(dt);
      }
      if (mixer) mixer.update(dt);

      // --- ルート: 体の向き + 待機の揺れ + 呼吸/歩調の上下動 + 重心移動 + 座りの沈み込み ---
      if (displayObject) {
        const sway = Math.sin(t * 1.1) * 0.03 * (talking ? 1 : 0.4) * (1 - sitAmt) * (1 - walkAmt);
        displayObject.rotation.y = baseRootY + bodyYaw + sway;
        const bob = Math.sin(t / 1.4) * 0.008 * (1 - walkAmt);        // 呼吸(歩行中は控えめ)
        const walkBob = -0.012 * Math.cos(2 * legPhase) * walkAmt;    // 歩調の上下動(2歩で1周期・接地で沈む)
        const dropf = (window.__sitp__ && window.__sitp__.drop !== undefined) ? window.__sitp__.drop : 0.48;
        const sitDrop = sitAmt * modelHeight * dropf;                 // 座ると腰を落とす
        displayObject.position.y = bob + walkBob - sitDrop;
        displayObject.position.x = 0.025 * Math.sin(legPhase) * walkAmt; // 立脚側へ重心を移す左右の揺れ
      }

      renderer.render(scene, camera);
    }
    animate();
  } catch (e) { showErr(String(e && e.message ? e.message : e)); }
}
window.__startVRM__ = start;
