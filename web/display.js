// Output stage: draws the 15-bit PSX frame onto the canvas at the screen's native resolution through a
// WebGL2 filter shader. Falls back to a plain nearest-neighbour 2D canvas when WebGL2 is unavailable.
//   sharp  - sharp-bilinear: crisp square pixels, no uneven columns at non-integer scales
//   smooth - xBR level 2 (Hyllian's algorithm): redraws pixel-art edges as smooth lines at any scale
//   crt    - beam-shaped scanlines, aperture-grille mask, gamma-correct blending

export const FILTERS = [
  { id: 'smooth', name: 'Smooth' },
  { id: 'crt', name: 'CRT' },
  { id: 'sharp', name: 'Sharp' },
];

const TEX_W = 1024, TEX_H = 512;          // covers every PSX display mode (up to 640x480)

const VERT = `#version 300 es
out vec2 vUV;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUV = vec2(p.x, 1.0 - p.y);                    // full-screen triangle, v = 0 at the top row
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

const HEAD = `#version 300 es
precision highp float;
precision highp int;
uniform sampler2D uTex;
uniform ivec2 uSize;     // source frame size in pixels
uniform vec2 uOut;       // canvas size in device pixels
in vec2 vUV;
out vec4 oColor;
vec3 T(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), uSize - 1), 0).rgb; }
`;

const SHARP = HEAD + `
void main() {
  vec2 size = vec2(uSize);
  vec2 texel = vUV * size;
  vec2 scale = max(floor(uOut / size), vec2(1.0));
  vec2 fl = floor(texel), cd = texel - fl - 0.5;
  vec2 range = 0.5 - 0.5 / scale;
  vec2 t = clamp(fl + (cd - clamp(cd, -range, range)) * scale + 0.5, vec2(0.5), size - 0.5);
  oColor = vec4(texture(uTex, t / vec2(${TEX_W}.0, ${TEX_H}.0)).rgb, 1.0);
}`;

// xBR-lv2, evaluated directly at the output resolution. Each of the four vector lanes handles one corner
// of the source pixel (x: bottom-right, y: top-right, z: top-left, w: bottom-left).
//      A1 B1 C1
//   A0  A  B  C C4
//   D0  D  E  F F4
//   G0  G  H  I I4
//      G5 H5 I5
const SMOOTH = HEAD + `
const vec3 Yw = 48.0 * vec3(0.2126, 0.7152, 0.0722);
const float EQ_THRESHOLD = 15.0, LV2_COEF = 2.0;
const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0), Bo = vec4(1.0, 1.0, -1.0, -1.0), Co = vec4(1.5, 0.5, -0.5, 0.5);
const vec4 Ax = vec4(1.0, -1.0, -1.0, 1.0), Bx = vec4(0.5, 2.0, -0.5, -2.0), Cx = vec4(1.0, 1.0, -0.5, 0.0);
const vec4 Ay = vec4(1.0, -1.0, -1.0, 1.0), By = vec4(2.0, 0.5, -2.0, -0.5), Cy = vec4(2.0, 0.0, -1.0, 0.5);
const vec4 Ci = vec4(0.25);
vec4 df(vec4 a, vec4 b) { return abs(a - b); }
vec4 ne(vec4 a, vec4 b) { return vec4(notEqual(a, b)); }
vec4 eq(vec4 a, vec4 b) { return vec4(lessThan(abs(a - b), vec4(EQ_THRESHOLD))); }
float cdf(vec3 a, vec3 b) { vec3 d = abs(a - b); return d.r + d.g + d.b; }
void main() {
  vec2 pos = vUV * vec2(uSize);
  ivec2 o = ivec2(floor(pos));
  vec2 fp = pos - floor(pos);
  vec3 A1 = T(o + ivec2(-1, -2)), B1 = T(o + ivec2(0, -2)), C1 = T(o + ivec2(1, -2));
  vec3 A  = T(o + ivec2(-1, -1)), B  = T(o + ivec2(0, -1)), C  = T(o + ivec2(1, -1));
  vec3 D  = T(o + ivec2(-1,  0)), E  = T(o),                F  = T(o + ivec2(1,  0));
  vec3 G  = T(o + ivec2(-1,  1)), H  = T(o + ivec2(0,  1)), I  = T(o + ivec2(1,  1));
  vec3 G5 = T(o + ivec2(-1,  2)), H5 = T(o + ivec2(0,  2)), I5 = T(o + ivec2(1,  2));
  vec3 A0 = T(o + ivec2(-2, -1)), D0 = T(o + ivec2(-2, 0)), G0 = T(o + ivec2(-2, 1));
  vec3 C4 = T(o + ivec2( 2, -1)), F4 = T(o + ivec2( 2, 0)), I4 = T(o + ivec2( 2, 1));

  vec4 b = vec4(dot(B, Yw), dot(D, Yw), dot(H, Yw), dot(F, Yw));
  vec4 c = vec4(dot(C, Yw), dot(A, Yw), dot(G, Yw), dot(I, Yw));
  vec4 e = vec4(dot(E, Yw));
  vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
  vec4 i4 = vec4(dot(I4, Yw), dot(C1, Yw), dot(A0, Yw), dot(G5, Yw));
  vec4 i5 = vec4(dot(I5, Yw), dot(C4, Yw), dot(A1, Yw), dot(G0, Yw));
  vec4 h5 = vec4(dot(H5, Yw), dot(F4, Yw), dot(B1, Yw), dot(D0, Yw));
  vec4 f4 = h5.yzwx;

  vec4 fx = Ao * fp.y + Bo * fp.x, fxl = Ax * fp.y + Bx * fp.x, fxu = Ay * fp.y + By * fp.x;

  vec4 lv0 = ne(e, f) * ne(e, h);
  vec4 lv1 = lv0 * clamp((1.0 - eq(f, b)) * (1.0 - eq(h, d))
                       + eq(e, i) * (1.0 - eq(f, i4)) * (1.0 - eq(h, i5)) + eq(e, g) + eq(e, c), 0.0, 1.0);
  vec4 lv2l = ne(e, g) * ne(d, g);
  vec4 lv2u = ne(e, c) * ne(b, c);

  // anti-aliased edge: blend across one output pixel, whatever the scale
  vec2 inv = vec2(uSize) / uOut;
  vec4 d45 = 0.5 * (abs(Ao) * inv.y + abs(Bo) * inv.x);
  vec4 dl  = 0.5 * (abs(Ax) * inv.y + abs(Bx) * inv.x);
  vec4 du  = 0.5 * (abs(Ay) * inv.y + abs(By) * inv.x);
  vec4 fx45i = clamp((fx  + d45 - Co - Ci) / (2.0 * d45), 0.0, 1.0);
  vec4 fx45  = clamp((fx  + d45 - Co)      / (2.0 * d45), 0.0, 1.0);
  vec4 fx30  = clamp((fxl + dl  - Cx)      / (2.0 * dl),  0.0, 1.0);
  vec4 fx60  = clamp((fxu + du  - Cy)      / (2.0 * du),  0.0, 1.0);

  vec4 wd1 = df(e, c) + df(e, g) + df(i, h5) + df(i, f4) + 4.0 * df(h, f);
  vec4 wd2 = df(h, d) + df(h, i5) + df(f, i4) + df(f, b) + 4.0 * df(e, i);

  vec4 edri = vec4(lessThanEqual(wd1, wd2)) * lv0;
  vec4 edr  = vec4(lessThan(wd1, wd2)) * lv1;
  vec4 edrl = vec4(lessThanEqual(LV2_COEF * df(f, g), df(h, c))) * lv2l * edr;
  vec4 edru = vec4(greaterThanEqual(df(f, g), LV2_COEF * df(h, c))) * lv2u * edr;

  vec4 m = max(max(fx30 * edrl, fx60 * edru), max(fx45 * edr, fx45i * edri));
  bvec4 px = lessThanEqual(df(e, f), df(e, h));

  vec3 r1 = mix(E,  px.x ? F : H, m.x);
  r1      = mix(r1, px.z ? D : B, m.z);
  vec3 r2 = mix(E,  px.y ? B : F, m.y);
  r2      = mix(r2, px.w ? H : D, m.w);
  oColor = vec4(mix(r1, r2, step(cdf(E, r1), cdf(E, r2))), 1.0);
}`;

// Scanlines are only drawn when each source line covers enough device pixels to show them, and the
// phosphor mask only when each source pixel is at least ~3 device pixels wide; otherwise they fade out.
const CRT = HEAD + `
vec3 L(ivec2 p) { vec3 c = T(p); return c * c; }                 // to (approximately) linear light
vec3 row(int y, float x) {
  float xs = x - 0.5, x0 = floor(xs), t = smoothstep(0.0, 1.0, xs - x0);
  return mix(L(ivec2(int(x0), y)), L(ivec2(int(x0) + 1, y)), t);
}
vec3 beam(float d, vec3 c) { vec3 s = mix(vec3(0.27), vec3(0.42), sqrt(c)); return exp(-0.5 * d * d / (s * s)); }
void main() {
  vec2 size = vec2(uSize), pos = vUV * size;
  float lines = size.y > 300.0 ? size.y * 0.5 : size.y;           // interlaced 480-line modes: one beam per field line
  float ly = pos.y * lines / size.y - 0.5, y0 = floor(ly), dy = ly - y0;
  float k = size.y / lines;
  vec3 a = row(int((y0 + 0.5) * k), pos.x), b = row(int((y0 + 1.5) * k), pos.x);
  float scanAmt = clamp((uOut.y / lines - 1.5) / 1.5, 0.0, 1.0);
  vec3 scan = (a * beam(dy, a) + b * beam(1.0 - dy, b)) * 1.15;
  vec3 col = mix(mix(a, b, dy), scan, scanAmt);
  float maskAmt = clamp((uOut.x / size.x - 2.0) / 2.0, 0.0, 1.0);
  int mi = int(gl_FragCoord.x) % 3;
  vec3 mk = mi == 0 ? vec3(1.0, 0.7, 0.7) : mi == 1 ? vec3(0.7, 1.0, 0.7) : vec3(0.7, 0.7, 1.0);
  mk = mix(vec3(1.0), mk, maskAmt);
  col *= mk / dot(mk, vec3(1.0 / 3.0));
  float v = smoothstep(0.0, 0.06, vUV.x) * smoothstep(0.0, 0.06, 1.0 - vUV.x)
          * smoothstep(0.0, 0.06, vUV.y) * smoothstep(0.0, 0.06, 1.0 - vUV.y);
  oColor = vec4(sqrt(col) * mix(0.85, 1.0, v), 1.0);
}`;

const SOURCES = { sharp: SHARP, smooth: SMOOTH, crt: CRT };

export function createDisplay(canvas) {
  let w = 0, h = 0, pix = null, hasFrame = false, filter = 'smooth';
  let gl = canvas.getContext('webgl2', { alpha: false, antialias: false, depth: false, stencil: false });
  let progs = {}, tex = null, vao = null, ctx2d = null, off = null;

  function compile(type, src) {
    const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS) && !gl.isContextLost()) throw new Error(gl.getShaderInfoLog(s));
    return s;
  }
  function initGL() {
    const vs = compile(gl.VERTEX_SHADER, VERT);
    progs = {};
    for (const [id, src] of Object.entries(SOURCES)) {
      try {
        const p = gl.createProgram();
        gl.attachShader(p, vs); gl.attachShader(p, compile(gl.FRAGMENT_SHADER, src)); gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        progs[id] = { p, uTex: gl.getUniformLocation(p, 'uTex'), uSize: gl.getUniformLocation(p, 'uSize'), uOut: gl.getUniformLocation(p, 'uOut') };
      } catch (e) { console.warn(`display: ${id} filter unavailable`, e); }
    }
    if (!progs.sharp) throw new Error('no usable shader');
    tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, TEX_W, TEX_H);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    vao = gl.createVertexArray();
    if (pix) upload();
  }
  if (gl) {
    try { initGL(); } catch (e) { console.warn('display: WebGL2 init failed, using 2D canvas', e); gl = null; }
    if (gl) {
      canvas.addEventListener('webglcontextlost', (e) => e.preventDefault());
      canvas.addEventListener('webglcontextrestored', () => { initGL(); draw(); });
    }
  }
  if (!gl) {
    // A canvas that tried WebGL2 can't switch to 2D, so swap in a fresh element.
    const c = canvas.cloneNode(false); canvas.replaceWith(c); canvas = c;
    ctx2d = canvas.getContext('2d', { alpha: false }); off = document.createElement('canvas');
  }

  // keep the backing store at the canvas's real size in device pixels so the shader output is 1:1
  const ro = new ResizeObserver(([entry]) => {
    const box = entry.devicePixelContentBoxSize && entry.devicePixelContentBoxSize[0];
    const cw = box ? box.inlineSize : Math.round(entry.contentRect.width * devicePixelRatio);
    const ch = box ? box.blockSize : Math.round(entry.contentRect.height * devicePixelRatio);
    if (cw && ch && (canvas.width !== cw || canvas.height !== ch)) { canvas.width = cw; canvas.height = ch; draw(); }
  });
  try { ro.observe(canvas, { box: 'device-pixel-content-box' }); } catch { ro.observe(canvas); }

  function upload() {
    if (gl) {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, pix);
    } else {
      if (off.width !== w || off.height !== h) { off.width = w; off.height = h; }
      off.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(pix.buffer, 0, w * h * 4), w, h), 0, 0);
    }
  }
  function draw() {
    if (!hasFrame) { clear(); return; }
    if (!gl) {
      ctx2d.imageSmoothingEnabled = false;
      ctx2d.drawImage(off, 0, 0, canvas.width, canvas.height);
      return;
    }
    if (gl.isContextLost()) return;
    const pr = progs[filter] || progs.sharp;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.useProgram(pr.p);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(pr.uTex, 0);
    gl.uniform2i(pr.uSize, w, h);
    gl.uniform2f(pr.uOut, canvas.width, canvas.height);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }
  function clear() {
    if (gl) { gl.clearColor(0, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); }
    else { ctx2d.fillStyle = '#000'; ctx2d.fillRect(0, 0, canvas.width, canvas.height); }
  }

  return {
    get canvas() { return canvas; },
    get filters() { return gl ? FILTERS.filter((f) => progs[f.id]) : []; },
    get filter() { return filter; },
    setFilter(id) { if (!gl || !progs[id]) return false; filter = id; draw(); return true; },
    // RGBA buffer (w*h*4) to fill with the next frame; then call present()
    pixels(fw, fh) {
      if (fw !== w || fh !== h || !pix) { w = fw; h = fh; pix = new Uint8Array(w * h * 4); }
      return pix;
    },
    present() { hasFrame = true; upload(); draw(); },
    reset() { hasFrame = false; clear(); },
  };
}
