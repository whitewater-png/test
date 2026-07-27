import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

/**
 * Renderer + scene + camera + render loop.
 *
 * `alpha: true` and a fully transparent clear colour are what let the same page
 * double as a desktop overlay — in the Electron mascot window the OS composites
 * whatever the canvas leaves transparent straight onto the wallpaper.
 */
export function createStage({ canvas, mode }) {
  const renderer = new THREE.WebGLRenderer({
    canvas,
    alpha: true,
    antialias: true,
    premultipliedAlpha: false,
  });
  renderer.setClearColor(0x000000, 0);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;

  const scene = new THREE.Scene();

  const camera = new THREE.PerspectiveCamera(28, 1, 0.1, 20);
  camera.position.set(0, 1.32, 2.1);

  // Soft three-point-ish lighting: VRM/MToon materials look flat under a single
  // light and blown out under anything strong.
  const key = new THREE.DirectionalLight(0xffffff, 1.6);
  key.position.set(1, 2, 1.6);
  const fill = new THREE.DirectionalLight(0xdce6ff, 0.5);
  fill.position.set(-1.4, 1, -0.8);
  scene.add(key, fill, new THREE.AmbientLight(0xffffff, 0.85));

  let controls = null;
  if (mode === 'stage') {
    controls = new OrbitControls(camera, renderer.domElement);
    controls.target.set(0, 1.25, 0);
    controls.enablePan = false;
    controls.enableDamping = true;
    controls.minDistance = 0.7;
    controls.maxDistance = 5;
    controls.update();
  }

  function resize() {
    const { clientWidth: w, clientHeight: h } = canvas;
    if (w === 0 || h === 0) return;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  resize();
  window.addEventListener('resize', resize);

  const timer = new THREE.Timer();
  const updaters = new Set();

  renderer.setAnimationLoop(() => {
    timer.update();
    const delta = timer.getDelta();
    for (const update of updaters) update(delta);
    controls?.update();
    renderer.render(scene, camera);
  });

  return {
    renderer,
    scene,
    camera,
    controls,
    /** @param {(delta: number) => void} fn */
    onUpdate(fn) {
      updaters.add(fn);
      return () => updaters.delete(fn);
    },
    /**
     * Point the camera at the character, sized from her actual bounding box —
     * characters come in wildly different heights, and hard-coded camera
     * positions crop someone's head off the moment they swap models.
     *
     * @param {THREE.Object3D} target
     * @param {{ fit?: 'full'|'upper'|'head', margin?: number }} [opts]
     */
    frame(target, { fit = 'full', margin = 1.1 } = {}) {
      const box = new THREE.Box3().setFromObject(target);
      if (box.isEmpty()) return;

      const size = box.getSize(new THREE.Vector3());
      const center = box.getCenter(new THREE.Vector3());

      // Crop the box down from the top for the closer shots. A T-posed model is
      // far wider than it is deep, so height drives the framing either way.
      if (fit !== 'full') {
        const keep = fit === 'head' ? 0.16 : 0.42; // fraction of total height
        box.min.y = box.max.y - size.y * keep;
        box.getSize(size);
        box.getCenter(center);
        size.x = Math.min(size.x, size.y * 1.2); // ignore outstretched arms
      }

      const fov = THREE.MathUtils.degToRad(camera.fov);
      const forHeight = size.y / 2 / Math.tan(fov / 2);
      const forWidth = size.x / 2 / Math.tan(fov / 2) / camera.aspect;
      const distance = Math.max(forHeight, forWidth) * margin + size.z / 2;

      camera.position.set(center.x, center.y, center.z + distance);
      camera.near = Math.max(0.01, distance / 100);
      camera.far = distance * 20;
      camera.updateProjectionMatrix();

      if (controls) {
        controls.target.copy(center);
        controls.minDistance = distance * 0.25;
        controls.maxDistance = distance * 4;
        controls.update();
      }
    },
  };
}
