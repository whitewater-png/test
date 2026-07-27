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

  const clock = new THREE.Clock();
  const updaters = new Set();

  renderer.setAnimationLoop(() => {
    const delta = clock.getDelta();
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
    /** Frame the character: `head` for the mascot bust, `full` for the stage. */
    frame(kind) {
      if (kind === 'head') {
        camera.fov = 22;
        camera.position.set(0, 1.34, 1.15);
        controls?.target.set(0, 1.34, 0);
      } else {
        camera.fov = 28;
        camera.position.set(0, 1.05, 2.6);
        controls?.target.set(0, 0.95, 0);
      }
      camera.updateProjectionMatrix();
      controls?.update();
    },
  };
}
