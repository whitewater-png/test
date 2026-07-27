// VRMA (VRM Animation) files are .glb containers carrying a `VRMC_vrm_animation`
// extension. Several commercial exporters ship files where that extension object
// is missing `specVersion`, or where the extension is absent from
// `extensionsUsed` — @pixiv/three-vrm-animation then refuses to load them.
//
// Both are pure JSON-chunk problems, so we can repair the buffer in memory
// before handing it to the loader (and write the fix back to disk from the CLI).

const MAGIC_GLTF = 0x46546c67; // 'glTF'
const CHUNK_JSON = 0x4e4f534a; // 'JSON'
const EXT = 'VRMC_vrm_animation';
const DEFAULT_SPEC_VERSION = '1.0';

/** @returns {{ json: object, jsonStart: number, jsonLength: number }} */
function readGlb(buffer) {
  if (buffer.byteLength < 20) throw new Error('not a GLB: file too short');
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  if (view.getUint32(0, true) !== MAGIC_GLTF) throw new Error('not a GLB: bad magic');

  let offset = 12;
  while (offset + 8 <= buffer.byteLength) {
    const chunkLength = view.getUint32(offset, true);
    const chunkType = view.getUint32(offset + 4, true);
    const chunkStart = offset + 8;
    if (chunkType === CHUNK_JSON) {
      const text = new TextDecoder().decode(buffer.subarray(chunkStart, chunkStart + chunkLength));
      return { json: JSON.parse(text), jsonStart: chunkStart, jsonLength: chunkLength };
    }
    offset = chunkStart + chunkLength;
  }
  throw new Error('not a GLB: no JSON chunk');
}

/**
 * Patch the glTF JSON in place.
 * @returns {string[]} human-readable list of what was changed (empty = nothing to do)
 */
function patchJson(json) {
  const fixes = [];
  const ext = json.extensions?.[EXT];
  if (!ext) return fixes; // not a VRMA at all — leave it alone

  if (typeof ext.specVersion !== 'string' || ext.specVersion.length === 0) {
    ext.specVersion = DEFAULT_SPEC_VERSION;
    fixes.push(`added extensions.${EXT}.specVersion = "${DEFAULT_SPEC_VERSION}"`);
  }
  if (!Array.isArray(json.extensionsUsed)) {
    json.extensionsUsed = [EXT];
    fixes.push('created extensionsUsed');
  } else if (!json.extensionsUsed.includes(EXT)) {
    json.extensionsUsed.push(EXT);
    fixes.push(`added ${EXT} to extensionsUsed`);
  }
  if (!json.asset || typeof json.asset.version !== 'string') {
    json.asset = { ...(json.asset ?? {}), version: '2.0' };
    fixes.push('added asset.version = "2.0"');
  }
  return fixes;
}

function encodeJsonChunk(json) {
  const raw = new TextEncoder().encode(JSON.stringify(json));
  const padded = (raw.byteLength + 3) & ~3;
  const out = new Uint8Array(padded).fill(0x20); // JSON chunks pad with spaces
  out.set(raw);
  return out;
}

/**
 * Repair a VRMA buffer. Returns the original buffer untouched when nothing needs fixing.
 * @param {Uint8Array} buffer
 * @returns {{ buffer: Uint8Array, fixes: string[] }}
 */
export function repairVrma(buffer) {
  const { json, jsonStart, jsonLength } = readGlb(buffer);
  const fixes = patchJson(json);
  if (fixes.length === 0) return { buffer, fixes };

  const head = buffer.subarray(0, jsonStart - 8);
  const tail = buffer.subarray(jsonStart + jsonLength);
  const chunk = encodeJsonChunk(json);

  const out = new Uint8Array(head.byteLength + 8 + chunk.byteLength + tail.byteLength);
  out.set(head, 0);
  const view = new DataView(out.buffer);
  view.setUint32(head.byteLength, chunk.byteLength, true);
  view.setUint32(head.byteLength + 4, CHUNK_JSON, true);
  out.set(chunk, head.byteLength + 8);
  out.set(tail, head.byteLength + 8 + chunk.byteLength);
  view.setUint32(8, out.byteLength, true); // total length in the GLB header

  return { buffer: out, fixes };
}
