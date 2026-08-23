// Dump reference frames as TSV, straight from the upstream TypeScript.
//
// This is the ground truth the C port is diffed against. It calls the real
// buildVoice with no reinterpretation: the only thing added here is the
// finalize step (cull, clamp, z-sort) that thinking-orbs' finalizeFrame
// performs, because the expo port does it inside its Skia recorder.
import { precomputeVoice, buildVoice } from './ref/voice.ts';

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
