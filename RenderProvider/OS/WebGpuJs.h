#ifndef RENDERPROVIDER_WEBGPUJS_H__
#define RENDERPROVIDER_WEBGPUJS_H__

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>

/*
 * THE BROWSER'S DEVICE, IN JAVASCRIPT, because WebGPU is a JavaScript API and
 * its objects live on the page's main thread. Everything here runs there:
 * the C++ side (WebGpuInstance.h, WebGpuPresenter.h) reaches it through
 * MAIN_THREAD_EM_ASM, which proxies from whatever thread draws.
 *
 * A RAW STRING, INSTALLED ONCE, rather than EM_ASM blocks: EM_ASM's code is a
 * macro argument, so a comma outside parentheses splits it (CanvasSurface's
 * old note), and a renderer is mostly commas. Installed with `new Function`
 * into Module.etcsGpu; the heap views are handed in per call rather than
 * captured, since a growing shared memory replaces them.
 *
 *   open(stateAddr)  start the adapter/device request; the int32 at stateAddr
 *                    becomes 1 ready, -1 failed or lost. 0 while pending.
 *   close(stateAddr) forget that address (its instance is going).
 *   present(hdr)     draw one frame for one surface; 1 drawn (or nothing to
 *                    draw into), 0 finished -- no device, lost, or it threw.
 *   drop(id)         a surface's overlay and textures, gone.
 *   evict(id, slot)  one texture, gone.
 *
 * THE OVERLAY. A canvas holds one kind of context for life: the surface's
 * canvas already has a 2D one (the host floor presents with putImageData),
 * so the device draws into a second canvas laid exactly over it --
 * pointer-events none, so input still lands on the canvas the window
 * listens to. Leaving the device removes it and the floor shows again.
 */
static const char* const RP_WEBGPU_JS = R"JS(
var G = { device: null, format: null, state: 0, pending: null, flags: [], P: {}, pipes: null };
function heap32() { return Module.etcsHeap32(); }
function setFlags(v) {
  var h = heap32();
  for (var i = 0; i < G.flags.length; i++) Atomics.store(h, G.flags[i] >> 2, v);
}
function open(addr) {
  if (!navigator.gpu) return 0;
  if (G.flags.indexOf(addr) < 0) G.flags.push(addr);
  if (G.state !== 0) { Atomics.store(heap32(), addr >> 2, G.state); return G.state > 0 ? 1 : 0; }
  if (!G.pending) {
    G.pending = navigator.gpu.requestAdapter().then(function (a) {
      if (!a) throw new Error('no WebGPU adapter');
      return a.requestDevice();
    }).then(function (d) {
      G.device = d;
      G.format = navigator.gpu.getPreferredCanvasFormat();
      G.state = 1;
      d.lost.then(function (info) {
        console.warn('[etcs] WebGPU device lost: ' + (info && info.message));
        G.device = null; G.state = -1; setFlags(-1);
      });
      setFlags(1);
    }).catch(function (e) {
      console.warn('[etcs] WebGPU unavailable: ' + e);
      G.state = -1; setFlags(-1);
    });
  }
  return 1;
}
function close(addr) {
  var i = G.flags.indexOf(addr);
  if (i >= 0) G.flags.splice(i, 1);
}
var WGSL =
  'struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f, @location(1) uv: vec2f };' +
  '@group(0) @binding(0) var<uniform> view: vec4f;' +
  '@vertex fn vs(@builtin(vertex_index) vi: u32, @location(0) r: vec4f, @location(1) c: vec4f) -> VOut {' +
  '  var k = array<vec2f, 4>(vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0), vec2f(1.0, 1.0));' +
  '  let q = k[vi]; let p = r.xy + q * r.zw; var o: VOut;' +
  '  o.pos = vec4f(p.x / view.x * 2.0 - 1.0, 1.0 - p.y / view.y * 2.0, 0.0, 1.0);' +
  '  o.col = c; o.uv = q; return o; }' +
  '@fragment fn fsRect(i: VOut) -> @location(0) vec4f { return i.col; }' +
  '@group(1) @binding(0) var smp: sampler;' +
  '@group(1) @binding(1) var img: texture_2d<f32>;' +
  '@fragment fn fsBlit(i: VOut) -> @location(0) vec4f {' +
  '  let s = textureSample(img, smp, i.uv); return vec4f(s.rgb, s.a * i.col.a); }';
function pipes() {
  if (G.pipes && G.pipes.device === G.device) return G.pipes;
  var d = G.device;
  var mod = d.createShaderModule({ code: WGSL });
  var l0 = d.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } } ] });
  var l1 = d.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'non-filtering' } },
    { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } } ] });
  // Source-over, the host raster's blend and the Vulkan backend's: colour by
  // source alpha, alpha replaced. The overlay is opaque, so alpha is moot.
  var blend = { color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
                alpha: { srcFactor: 'one', dstFactor: 'zero', operation: 'add' } };
  var vtx = { module: mod, entryPoint: 'vs', buffers: [ { arrayStride: 32, stepMode: 'instance', attributes: [
    { shaderLocation: 0, offset: 0, format: 'float32x4' },
    { shaderLocation: 1, offset: 16, format: 'float32x4' } ] } ] };
  function make(entry, layouts) {
    return d.createRenderPipeline({
      layout: d.createPipelineLayout({ bindGroupLayouts: layouts }),
      vertex: vtx,
      fragment: { module: mod, entryPoint: entry, targets: [ { format: G.format, blend: blend } ] },
      primitive: { topology: 'triangle-strip' } });
  }
  // NEAREST, like the Vulkan backend: a pixel editor shows the pixels.
  G.pipes = { device: d, l0: l0, l1: l1, rect: make('fsRect', [l0]), blit: make('fsBlit', [l0, l1]),
              sampler: d.createSampler({ magFilter: 'nearest', minFilter: 'nearest' }) };
  return G.pipes;
}
function drop(id) {
  var p = G.P[id];
  if (!p) return;
  for (var s in p.tex) p.tex[s].t.destroy();
  if (p.inst) p.inst.destroy();
  if (p.uni) p.uni.destroy();
  if (p.ov && p.ov.parentNode) p.ov.parentNode.removeChild(p.ov);
  delete G.P[id];
}
function evict(id, slot) {
  var p = G.P[id];
  if (!p || !p.tex[slot]) return;
  p.tex[slot].t.destroy();
  delete p.tex[slot];
}
function place(p, base) {
  var cs = getComputedStyle(base);
  var hidden = cs.display === 'none' || cs.visibility === 'hidden';
  p.ov.style.display = hidden ? 'none' : '';
  if (hidden) return;
  if (cs.zIndex !== 'auto') p.ov.style.zIndex = String((parseInt(cs.zIndex, 10) || 0) + 1);
  // By where both boxes ARE, not by offsetLeft: the overlay's containing
  // block and the canvas's offsetParent are not always the same element.
  var br = base.getBoundingClientRect();
  var orc = p.ov.getBoundingClientRect();
  var l = parseFloat(p.ov.style.left) || 0;
  var t = parseFloat(p.ov.style.top) || 0;
  p.ov.style.left = (l + br.left + base.clientLeft - orc.left) + 'px';
  p.ov.style.top = (t + br.top + base.clientTop - orc.top) + 'px';
  p.ov.style.width = base.clientWidth + 'px';
  p.ov.style.height = base.clientHeight + 'px';
}
// hdr (int32): id, width, height, nops, opsF32, opsI32, nups, ups, clearF32, target
// opsF32: 8 per op -- x y w h and colour (a blit's opacity in the 4th)
// opsI32: 2 per op -- kind (0 rect, 1 blit) and texture slot
// ups: 4 per upload -- slot, w, h, address of w*h*4 RGBA bytes
function present(hdr) {
  var H = heap32();
  var U8 = Module.etcsHeapU8();
  var F = Module.etcsHeapF32();
  var b = hdr >> 2;
  var id = H[b], w = H[b + 1], h = H[b + 2], n = H[b + 3];
  var of = H[b + 4] >> 2, oi = H[b + 5] >> 2, nu = H[b + 6], up = H[b + 7] >> 2, cf = H[b + 8] >> 2;
  var target = UTF8ToString(H[b + 9]);
  if (!G.device) return 0;
  var base = document.getElementById(target);
  if (!base || !base.parentNode || w <= 0 || h <= 0) return 1;
  try {
    var pp = pipes();
    var d = G.device;
    var p = G.P[id];
    if (!p) {
      var ov = document.createElement('canvas');
      ov.id = target + '-gpu';
      ov.setAttribute('aria-hidden', 'true');
      ov.style.position = 'absolute';
      ov.style.pointerEvents = 'none';
      ov.style.margin = '0'; ov.style.padding = '0'; ov.style.border = '0';
      ov.style.left = '0px'; ov.style.top = '0px';
      base.parentNode.insertBefore(ov, base.nextSibling);
      var ctx = ov.getContext('webgpu');
      if (!ctx) { ov.parentNode.removeChild(ov); return 0; }
      ctx.configure({ device: d, format: G.format, alphaMode: 'opaque' });
      var uni = d.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      p = G.P[id] = { ov: ov, ctx: ctx, tex: {}, inst: null, cap: 0, uni: uni,
                      bg0: d.createBindGroup({ layout: pp.l0, entries: [ { binding: 0, resource: { buffer: uni } } ] }) };
    }
    place(p, base);
    if (p.ov.width !== w) p.ov.width = w;
    if (p.ov.height !== h) p.ov.height = h;
    d.queue.writeBuffer(p.uni, 0, new Float32Array([w, h, 0, 0]));

    for (var u = 0; u < nu; u++) {
      var slot = H[up + u * 4], tw = H[up + u * 4 + 1], th = H[up + u * 4 + 2], ta = H[up + u * 4 + 3];
      var e = p.tex[slot];
      if (!e || e.w !== tw || e.h !== th) {
        if (e) e.t.destroy();
        var tex = d.createTexture({ size: [tw, th], format: 'rgba8unorm',
                                    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        e = p.tex[slot] = { t: tex, w: tw, h: th,
          bg: d.createBindGroup({ layout: pp.l1, entries: [ { binding: 0, resource: pp.sampler },
                                                            { binding: 1, resource: tex.createView() } ] }) };
      }
      // slice(): a copy off the shared heap -- WebGPU will not read a
      // SharedArrayBuffer view.
      d.queue.writeTexture({ texture: e.t }, U8.slice(ta, ta + tw * th * 4),
                           { bytesPerRow: tw * 4, rowsPerImage: th }, { width: tw, height: th });
    }

    if (n > 0) {
      var bytes = n * 32;
      if (!p.inst || p.cap < bytes) {
        if (p.inst) p.inst.destroy();
        p.cap = Math.max(bytes, p.cap * 2, 4096);
        p.inst = d.createBuffer({ size: p.cap, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      }
      d.queue.writeBuffer(p.inst, 0, F.slice(of, of + n * 8));
    }
    var enc = d.createCommandEncoder();
    var pass = enc.beginRenderPass({ colorAttachments: [ { view: p.ctx.getCurrentTexture().createView(),
      clearValue: { r: F[cf], g: F[cf + 1], b: F[cf + 2], a: F[cf + 3] }, loadOp: 'clear', storeOp: 'store' } ] });
    if (n > 0) {
      pass.setBindGroup(0, p.bg0);
      pass.setVertexBuffer(0, p.inst);
      var i = 0;
      while (i < n) {
        if (H[oi + i * 2] === 0) {
          // Runs of rects are one instanced draw; order across kinds is kept.
          var j = i;
          while (j < n && H[oi + j * 2] === 0) j++;
          pass.setPipeline(pp.rect);
          pass.draw(4, j - i, 0, i);
          i = j;
          continue;
        }
        var te = p.tex[H[oi + i * 2 + 1]];
        if (te) {
          pass.setPipeline(pp.blit);
          pass.setBindGroup(1, te.bg);
          pass.draw(4, 1, 0, i);
        }
        i++;
      }
    }
    pass.end();
    d.queue.submit([ enc.finish() ]);
    return 1;
  } catch (err) {
    console.warn('[etcs] WebGPU present failed: ' + err);
    drop(id);
    return 0;
  }
}
return { open: open, close: close, present: present, drop: drop, evict: evict };
)JS";

// Once per page, on the main thread. Every entry point below calls it first,
// so no caller has to know whether it already ran.
static inline void rp_webgpu_install()
{
    MAIN_THREAD_EM_ASM({
        if (Module.etcsGpu) return;
        Module.etcsHeap32  = function() { return HEAP32; };
        Module.etcsHeapU8  = function() { return HEAPU8; };
        Module.etcsHeapF32 = function() { return HEAPF32; };
        Module.etcsGpu = (new Function('Module', 'UTF8ToString', UTF8ToString($0)))(Module, UTF8ToString);
    }, RP_WEBGPU_JS);
}
#endif

#endif // RENDERPROVIDER_WEBGPUJS_H__
