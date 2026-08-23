// Dump reference frames as TSV, straight from the upstream TypeScript.
//
// This is the ground truth the C port is diffed against. It calls the real
// buildVoice with no reinterpretation: the only thing added here is the
// finalize step (cull, clamp, z-sort) that thinking-orbs' finalizeFrame
// performs, because the expo port does it inside its Skia recorder.
import { precomputeVoice, buildVoice } from './ref/voice.ts';
import { precomputeWave, buildWave, precomputeRubik, buildRubik } from './ref/lattice.ts';
import { precomputeRibbon, buildRibbon } from './ref/ribbon.ts';
import { frameBraid } from './ref/braid.ts';
import { frameWeb } from './ref/web.ts';

const SIZE = Number(process.env.ORB_SIZE ?? 466);
const R_MIN = 0.3;
const ALPHA_CULL = 0.02;

const opts = {};                       // every tuning takes its documented default
const s = precomputeVoice(opts);
const n = s.dotCount;

const buf = {
  xs: new Float32Array(n), ys: new Float32Array(n), zs: new Float32Array(n),
  rs: new Float32Array(n), ws: new Float32Array(n), as: new Float32Array(n),
  count: 0,
};

function frame(t, from, to, mix, amp) {
  buf.count = 0;
  buildVoice(buf, SIZE, t, opts, s, {
    amp, from, to, mix,
    rMul: 1, yaw: 0, pitch: 0, roll: 0, orient: undefined,
  });
  const out = [];
  for (let i = 0; i < buf.count; i++) {
    if (buf.as[i] < ALPHA_CULL) continue;
    out.push({
      x: buf.xs[i], y: buf.ys[i], z: buf.zs[i],
      r: Math.max(R_MIN, buf.rs[i]), w: buf.ws[i], a: buf.as[i],
    });
  }
  out.sort((a, b) => a.z - b.z);
  return out;
}

// --- wave -------------------------------------------------------------
// The playground's `listening` orb. Its own lattice (rings 15 / lonDensity 40 =
// 384 dots), no behaviour, no amplitude -- so it needs its own precompute and a
// frame helper that finalizes identically.
const ws = precomputeWave(opts);
const wbuf = {
  xs: new Float32Array(ws.dotCount), ys: new Float32Array(ws.dotCount),
  zs: new Float32Array(ws.dotCount), rs: new Float32Array(ws.dotCount),
  ws: new Float32Array(ws.dotCount), as: new Float32Array(ws.dotCount),
  count: 0,
};

function waveFrame(t, amp = 0) {
  wbuf.count = 0;
  buildWave(wbuf, SIZE, t, opts, ws, {
    amp: 0, from: 0, to: 0, mix: 1,
    /* rMul is how the device feeds the microphone level in. Same mapping as
     * orb_build_wave's. */
    rMul: Math.min(1.6, 1 + 5.0 * amp),
    yaw: 0, pitch: 0, roll: 0, orient: undefined,
  });
  const out = [];
  for (let i = 0; i < wbuf.count; i++) {
    if (wbuf.as[i] < ALPHA_CULL) continue;
    out.push({
      x: wbuf.xs[i], y: wbuf.ys[i], z: wbuf.zs[i],
      r: Math.max(R_MIN, wbuf.rs[i]), w: wbuf.ws[i], a: wbuf.as[i],
    });
  }
  out.sort((a, b) => a.z - b.z);
  return out;
}

// Rubik shares wave's lattice size but has its own precompute (the move list).
const rs_ = precomputeRubik(opts);
const rbuf = {
  xs: new Float32Array(rs_.dotCount), ys: new Float32Array(rs_.dotCount),
  zs: new Float32Array(rs_.dotCount), rs: new Float32Array(rs_.dotCount),
  ws: new Float32Array(rs_.dotCount), as: new Float32Array(rs_.dotCount),
  count: 0,
};

function rubikFrame(t) {
  rbuf.count = 0;
  buildRubik(rbuf, SIZE, t, opts, rs_, {
    amp: 0, from: 0, to: 0, mix: 1,
    rMul: 1, yaw: 0, pitch: 0, roll: 0, orient: undefined,
  });
  const out = [];
  for (let i = 0; i < rbuf.count; i++) {
    if (rbuf.as[i] < ALPHA_CULL) continue;
    out.push({
      x: rbuf.xs[i], y: rbuf.ys[i], z: rbuf.zs[i],
      r: Math.max(R_MIN, rbuf.rs[i]), w: rbuf.ws[i], a: rbuf.as[i],
    });
  }
  out.sort((a, b) => a.z - b.z);
  return out;
}

// Timestamps chosen to land in different parts of the solve cycle: mid-scramble,
// mid-unwind, and inside the rest where nothing is turning.
const RUBIK_CASES = [
  ['rubik_a', 1.7],
  ['rubik_b', 5.0],
  ['rubik_c', 9.3],
  ['rubik_d', 12.4],
];

/*
 * Ribbon's own opts, not the shared ones. It is tuned down from the reference
 * defaults for frame budget -- 90 ghosts and 56 segments -- and the harness has
 * to pass the SAME numbers, because parity needs the two sides to agree rather
 * than to use defaults. Sharing `opts` would also have reached braid, whose
 * ghostN is deliberately still 150.
 */
const RIBBON_OPTS = { ghostN: 90, segs: 56 };
const bs_ = precomputeRibbon(RIBBON_OPTS);
const bbuf = {
  xs: new Float32Array(bs_.dotCount), ys: new Float32Array(bs_.dotCount),
  zs: new Float32Array(bs_.dotCount), rs: new Float32Array(bs_.dotCount),
  ws: new Float32Array(bs_.dotCount), as: new Float32Array(bs_.dotCount),
  count: 0,
};

function ribbonFrame(t, amp = 0) {
  bbuf.count = 0;
  /* wobMul is the undulation's depth, and what the device drives from the voice
   * level. Same mapping as orb_build_ribbon's. */
  const o = { ...RIBBON_OPTS, wobMul: Math.min(1.6, 0.08 + 2.2 * amp) };
  buildRibbon(bbuf, SIZE, t, o, bs_, {
    amp: 0, from: 0, to: 0, mix: 1,
    rMul: 1, yaw: 0, pitch: 0, roll: 0, orient: undefined,
  });
  const out = [];
  for (let i = 0; i < bbuf.count; i++) {
    if (bbuf.as[i] < ALPHA_CULL) continue;
    out.push({
      x: bbuf.xs[i], y: bbuf.ys[i], z: bbuf.zs[i],
      r: Math.max(R_MIN, bbuf.rs[i]), w: bbuf.ws[i], a: bbuf.as[i],
    });
  }
  out.sort((a, b) => a.z - b.z);
  return out;
}

// braid uses thinking-orbs' canvas API: it returns an already-finalized frame
// (culled, clamped, z-sorted) rather than filling a DotBuffer, so there is
// nothing for this harness to finalize.
function braidFrame(t) {
  return frameBraid(SIZE, t, {}).dots.map((d) => ({
    x: d.x, y: d.y, z: d.z, r: d.r, w: d.white, a: d.a ?? 1,
  }));
}

// web emits both dots and lines. Lines go out under a `~L` label so the two
// never share a case -- compare.py reads the column count per case.
function webFrame(t) { return frameWeb(SIZE, t, {}); }

const WEB_CASES = [
  ['web_a', 1.7],
  ['web_b', 5.4],
  ['web_c', 13.1],
];

const BRAID_CASES = [
  ['braid_a', 1.7],
  ['braid_b', 6.2],
  ['braid_c', 14.5],
];

const RIBBON_CASES = [
  ['ribbon_a', 1.7, 0.0],
  ['ribbon_b', 4.6, 0.0],
  ['ribbon_c', 11.9, 0.0],
  /* At voice level, which is the whole point of the wobMul path. */
  ['ribbon_amp', 4.6, 0.8],
];

const WAVE_CASES = [
  ['wave_a', 1.7, 0.0],
  ['wave_b', 3.3, 0.0],
  ['wave_c', 5.5, 0.0],
  ['wave_d', 13.2, 0.0],
  /* At a microphone level, which is the rMul path. */
  ['wave_amp', 3.3, 0.1],
];

// (label, behaviour, t, amp) cases. Behaviour indices are voice.ts's own.
// `idle` is included now that the projector shim implements roll faithfully. It
// is the only behaviour with idleW = 1, which switches on the whole body layer --
// float, breath, squash-and-stretch, spin drift and roll -- plus one of four
// gestures chosen per 9-second epoch. Several timestamps, to catch epochs with
// and without a gesture playing.
const CASES = [
  ['idle_a',        0, 1.7,  0.0],
  ['idle_b',        0, 5.5,  0.0],
  ['idle_c',        0, 13.2, 0.0],
  ['idle_d',        0, 20.9, 0.0],
  ['initializing',  1, 1.7,  0.0],
  ['listening',     2, 1.7,  0.0],
  ['listening_amp', 2, 3.3,  0.8],
  ['thinking',      3, 1.7,  0.0],
  ['speaking',      4, 1.7,  0.0],
  ['speaking_amp',  4, 3.3,  0.8],
  ['connecting',    5, 1.7,  0.0],
  ['buffering',     6, 1.7,  0.0],
  ['disconnected',  7, 1.7,  0.0],
  // A live transition, which is where the wavefront sign convention is decided.
  ['blend_l2s',     0, 2.5,  0.6],
];

for (const [label, t, amp] of WAVE_CASES) {
  for (const d of waveFrame(t, amp)) {
    process.stdout.write(
      `${label}\t${d.x.toFixed(4)}\t${d.y.toFixed(4)}\t${d.z.toFixed(4)}\t` +
      `${d.r.toFixed(4)}\t${d.w.toFixed(4)}\t${d.a.toFixed(4)}\n`);
  }
}

for (const [label, t] of RUBIK_CASES) {
  for (const d of rubikFrame(t)) {
    process.stdout.write(
      `${label}\t${d.x.toFixed(4)}\t${d.y.toFixed(4)}\t${d.z.toFixed(4)}\t` +
      `${d.r.toFixed(4)}\t${d.w.toFixed(4)}\t${d.a.toFixed(4)}\n`);
  }
}

for (const [label, t, amp] of RIBBON_CASES) {
  for (const d of ribbonFrame(t, amp)) {
    process.stdout.write(
      `${label}\t${d.x.toFixed(4)}\t${d.y.toFixed(4)}\t${d.z.toFixed(4)}\t` +
      `${d.r.toFixed(4)}\t${d.w.toFixed(4)}\t${d.a.toFixed(4)}\n`);
  }
}

for (const [label, t] of WEB_CASES) {
  const fr = webFrame(t);
  for (const d of fr.dots) {
    process.stdout.write(
      `${label}\t${d.x.toFixed(4)}\t${d.y.toFixed(4)}\t${d.z.toFixed(4)}\t` +
      `${d.r.toFixed(4)}\t${d.white.toFixed(4)}\t${(d.a ?? 1).toFixed(4)}\n`);
  }
  for (const l of fr.lines) {
    process.stdout.write(
      `${label}~L\t${l.x1.toFixed(4)}\t${l.y1.toFixed(4)}\t` +
      `${l.x2.toFixed(4)}\t${l.y2.toFixed(4)}\t` +
      `${l.white.toFixed(4)}\t${(l.a ?? 1).toFixed(4)}\t${l.w.toFixed(4)}\n`);
  }
}

for (const [label, t] of BRAID_CASES) {
  for (const d of braidFrame(t)) {
    process.stdout.write(
      `${label}\t${d.x.toFixed(4)}\t${d.y.toFixed(4)}\t${d.z.toFixed(4)}\t` +
      `${d.r.toFixed(4)}\t${d.w.toFixed(4)}\t${d.a.toFixed(4)}\n`);
  }
}

for (const [label, b, t, amp] of CASES) {
  let dots;
  if (label === 'blend_l2s') dots = frame(t, 2, 4, 0.4, amp);   // listening -> speaking
  else dots = frame(t, b, b, 1, amp);
  for (const d of dots) {
    process.stdout.write(
      `${label}\t${d.x.toFixed(4)}\t${d.y.toFixed(4)}\t${d.z.toFixed(4)}\t` +
      `${d.r.toFixed(4)}\t${d.w.toFixed(4)}\t${d.a.toFixed(4)}\n`);
  }
}
