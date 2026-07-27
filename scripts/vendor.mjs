#!/usr/bin/env node
// Copies the ESM builds we need out of node_modules into web/vendor/ so the
// page can run straight from an <script type="importmap"> with no bundler and
// no network access (the mascot has to work offline).
import { mkdir, copyFile, access } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const from = (p) => join(root, 'node_modules', p);
const to = (p) => join(root, 'web', 'vendor', p);

const FILES = [
  ['three/build/three.module.js', 'three/three.module.js'],
  ['three/build/three.core.js', 'three/three.core.js'],
  ['three/examples/jsm/loaders/GLTFLoader.js', 'three/addons/loaders/GLTFLoader.js'],
  ['three/examples/jsm/utils/BufferGeometryUtils.js', 'three/addons/utils/BufferGeometryUtils.js'],
  ['three/examples/jsm/utils/SkeletonUtils.js', 'three/addons/utils/SkeletonUtils.js'],
  ['three/examples/jsm/controls/OrbitControls.js', 'three/addons/controls/OrbitControls.js'],
  ['@pixiv/three-vrm/lib/three-vrm.module.js', 'three-vrm/three-vrm.module.js'],
  [
    '@pixiv/three-vrm-animation/lib/three-vrm-animation.module.js',
    'three-vrm-animation/three-vrm-animation.module.js',
  ],
];

let copied = 0;
for (const [src, dest] of FILES) {
  try {
    await access(from(src));
  } catch {
    console.warn(`[vendor] skipped (not installed): ${src}`);
    continue;
  }
  await mkdir(dirname(to(dest)), { recursive: true });
  await copyFile(from(src), to(dest));
  copied += 1;
}

console.log(`[vendor] copied ${copied}/${FILES.length} files into web/vendor/`);
if (copied < FILES.length) {
  console.log('[vendor] run `npm install` first if anything was skipped.');
}
