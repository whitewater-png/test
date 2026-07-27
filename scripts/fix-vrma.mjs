#!/usr/bin/env node
// Repair VRMA files on disk. The bridge server already repairs them in memory
// on the way to the browser; this is for when you'd rather fix the source file.
//
//   node scripts/fix-vrma.mjs                # repairs everything in motions/
//   node scripts/fix-vrma.mjs a.vrma b.vrma  # repairs the named files
//   node scripts/fix-vrma.mjs --check        # report only, write nothing
import { readdir, readFile, writeFile, copyFile } from 'node:fs/promises';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { repairVrma } from '../server/vrma.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const checkOnly = args.includes('--check');
const named = args.filter((a) => !a.startsWith('--'));

const targets = named.length
  ? named.map((p) => resolve(p))
  : (await readdir(join(root, 'motions')))
      .filter((f) => extname(f).toLowerCase() === '.vrma')
      .map((f) => join(root, 'motions', f));

if (targets.length === 0) {
  console.log('No .vrma files found. Drop your motions into motions/ first.');
  process.exit(0);
}

let repaired = 0;
for (const file of targets) {
  let result;
  try {
    result = repairVrma(new Uint8Array(await readFile(file)));
  } catch (err) {
    console.error(`✗ ${file}: ${err.message}`);
    continue;
  }
  if (result.fixes.length === 0) {
    console.log(`· ${file}: already valid`);
    continue;
  }
  repaired += 1;
  if (checkOnly) {
    console.log(`! ${file}: ${result.fixes.join('; ')} (--check, not written)`);
    continue;
  }
  await copyFile(file, `${file}.bak`);
  await writeFile(file, result.buffer);
  console.log(`✓ ${file}: ${result.fixes.join('; ')} (original kept as .bak)`);
}

console.log(`\n${repaired}/${targets.length} file(s) needed repair.`);
