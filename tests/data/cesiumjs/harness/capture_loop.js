#!/usr/bin/env node
/**
 * P37: Reused-page batch capture.
 *
 * One browser, one page, one Cesium Viewer for the whole batch.
 * Per fixture: load tileset -> de-light customShader -> auto-frame ->
 * wait tiles -> render -> canvas PNG -> params -> destroy tileset.
 *
 * Much faster than cesium_render.js (one browser per fixture): no repeated
 * browser startup or Cesium.js fetch+parse (~5s saved per fixture).
 *
 * Output layout matches batch_capture.js: <out>/<name>/render.png + params.json
 *
 * Usage:
 *   node capture_loop.js --list <file> --out <dir> [--root <data-dir>]
 *     [--fov 60] [--background 0.1,0.1,0.1,1] [--width 400] [--height 300]
 *     [--port 18777]
 *
 * List file format (same as batch_capture.js): "<name> <tileset.json path>" per line.
 * Tileset URLs are served from --root (default: <harness>/../.. = tests/data).
 */
const puppeteer = require('puppeteer');
const http = require('http');
const fs = require('fs');
const path = require('path');

function parseArgs() {
  const args = {};
  for (let i = 2; i < process.argv.length; i += 2) {
    const key = process.argv[i].replace(/^--/, '');
    args[key] = process.argv[i + 1];
  }
  return args;
}

function serveDirectory(rootDir, port) {
  const mime = {
    '.json': 'application/json',
    '.b3dm': 'application/octet-stream',
    '.i3dm': 'application/octet-stream',
    '.pnts': 'application/octet-stream',
    '.cmpt': 'application/octet-stream',
    '.glb': 'model/gltf-binary',
    '.gltf': 'model/gltf+json',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
  };
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      let urlPath = decodeURIComponent(req.url.split('?')[0]);
      let filePath = path.join(rootDir, urlPath);
      if (!filePath.startsWith(rootDir)) {
        res.writeHead(403); res.end(); return;
      }
      fs.readFile(filePath, (err, data) => {
        if (err) { res.writeHead(404); res.end(); return; }
        const ext = path.extname(filePath).toLowerCase();
        res.writeHead(200, { 'Content-Type': mime[ext] || 'application/octet-stream', 'Access-Control-Allow-Origin': '*' });
        res.end(data);
      });
    });
    server.listen(port, () => resolve(server));
  });
}

// The in-page capture function. Destroys the previous tileset first,
// then loads/renders/captures the new one. Returns a plain JSON-able object.
const CAPTURE_FN = `
window.__capture = async function(tilesetUrl) {
  const viewer = window.__viewer;
  // Destroy previous tileset (load/render/capture/unload loop).
  if (window.__tileset) {
    try { viewer.scene.primitives.remove(window.__tileset); } catch (e) {}
    try { window.__tileset.destroy(); } catch (e) {}
    window.__tileset = null;
  }
  try {
    const tileset = await Cesium.Cesium3DTileset.fromUrl(tilesetUrl);
    // De-light: UNLIT + pre-decode undecoded color paths (same as cesium_render.js).
    tileset.customShader = new Cesium.CustomShader({
      lightingModel: Cesium.LightingModel.UNLIT,
      fragmentShaderText:
        'void fragmentMain(FragmentInput fsInput, inout czm_modelMaterial material) {\\n' +
        '#if !defined(HAS_BASE_COLOR_TEXTURE) && !defined(HAS_SRGB_COLOR)\\n' +
        '  material.diffuse = czm_srgbToLinear(material.diffuse);\\n' +
        '#endif\\n' +
        '}\\n',
    });
    viewer.scene.primitives.add(tileset);
    await tileset.readyPromise;
    window.__tileset = tileset;

    // Frustum (fov affects auto-framing range).
    viewer.camera.frustum.fov = Cesium.Math.toRadians(window.__fov);
    viewer.camera.frustum.aspectRatio = window.__width / window.__height;
    viewer.camera.frustum.near = 0.1;
    viewer.camera.frustum.far = 10000.0;

    // Auto-frame (viewBoundingSphere = viewer.zoomTo equivalent).
    const bs = tileset.boundingSphere;
    if (!bs) throw new Error('auto-framing failed: boundingSphere unavailable');
    viewer.camera.viewBoundingSphere(bs);

    // Wait for tiles (event or 15s timeout), then settle 1s.
    await new Promise((resolve) => {
      let done = false;
      const finish = () => { if (!done) { done = true; resolve(); } };
      tileset.allTilesLoaded.addEventListener(finish);
      setTimeout(finish, 15000);
    });
    await new Promise(r => setTimeout(r, 1000));

    // Render + canvas export in the SAME JS task (no preserveDrawingBuffer).
    viewer.scene.render();
    const canvas = viewer.scene.canvas;
    const dataUrl = canvas.toDataURL('image/png');

    // Extract WC camera params (must be after render).
    const cam = viewer.camera, frustum = cam.frustum;
    return {
      ok: true,
      dataUrl: dataUrl,
      tilesLoaded: tileset.tilesLoaded,
      params: {
        width: window.__width,
        height: window.__height,
        autoFramed: true,
        camera: {
          position: [cam.positionWC.x, cam.positionWC.y, cam.positionWC.z],
          direction: [cam.directionWC.x, cam.directionWC.y, cam.directionWC.z],
          up: [cam.upWC.x, cam.upWC.y, cam.upWC.z],
          fov: Cesium.Math.toDegrees(frustum.fov),
          aspectRatio: frustum.aspectRatio,
          near: frustum.near,
          far: frustum.far,
        },
        backgroundColor: window.__bg,
      },
    };
  } catch (e) {
    return { ok: false, error: String(e) };
  }
};
`;

async function main() {
  const args = parseArgs();
  const listFile = args.list;
  const outDir = args.out || '/tmp/caploop';
  const harnessDir = __dirname;
  const root = path.resolve(args.root || path.join(harnessDir, '..', '..'));
  const fov = parseFloat(args.fov || '60');
  const bg = (args.background || '0.1,0.1,0.1,1').split(',').map(Number);
  const width = parseInt(args.width || '400');
  const height = parseInt(args.height || '300');
  const basePort = parseInt(args.port || '18777');
  const jobs = Math.max(1, parseInt(args.jobs || '1'));

  const fixtures = fs.readFileSync(listFile, 'utf8').split('\n')
    .map(l => l.trim()).filter(l => l && !l.startsWith('#'))
    .map(l => { const i = l.indexOf(' '); return { name: l.slice(0, i), file: l.slice(i + 1) }; });

  // Split fixtures round-robin across workers.
  const chunks = Array.from({ length: jobs }, () => []);
  fixtures.forEach((fx, i) => chunks[i % jobs].push(fx));

  const results = await Promise.all(
    chunks.map((chunk, wi) => runWorker(chunk, basePort + wi * 2, {
      outDir, root, fov, bg, width, height, harnessDir, worker: wi,
    }))
  );
  const ok = results.reduce((s, r) => s + r.ok, 0);
  const failed = results.reduce((s, r) => s + r.failed, 0);
  console.log(`Done: ${ok} ok, ${failed} failed`);
  process.exit(0); // ensure exit (http keep-alive can stall server.close)
}

async function runWorker(fixtures, port, cfg) {
  const { outDir, root, fov, bg, width, height, harnessDir, worker } = cfg;
  const dataServer = await serveDirectory(root, port);
  const cesiumPath = path.join(harnessDir, 'node_modules', 'cesium', 'Build', 'Cesium');
  const cesiumServer = await serveDirectory(cesiumPath, port + 1);

  const browser = await puppeteer.launch({
    headless: 'new',
    executablePath: '/home/hatch/workspace/chrome-feasibility/chrome-headless-shell-linux64/chrome-headless-shell',
    args: ['--use-gl=swiftshader', '--enable-unsafe-swiftshader', '--no-sandbox',
           '--disable-dev-shm-usage', `--window-size=${width},${height}`],
  });

  // Build the page once: Viewer + __capture function.
  async function newPage() {
    const page = await browser.newPage();
    await page.setViewport({ width, height, deviceScaleFactor: 1 });
    const html = `<!DOCTYPE html><html><head><meta charset="utf-8">
<script src="http://localhost:${port + 1}/Cesium.js"></script>
<style>html,body,#cesiumContainer{margin:0;padding:0;width:${width}px;height:${height}px;overflow:hidden}</style>
</head><body><div id="cesiumContainer"></div><script>
window.__width = ${width}; window.__height = ${height};
window.__fov = ${fov}; window.__bg = [${bg.join(',')}];
window.__viewer = null; window.__tileset = null;
async function initViewer() {
  const viewer = new Cesium.Viewer('cesiumContainer', {
    animation: false, baseLayerPicker: false, fullscreenButton: false,
    geocoder: false, homeButton: false, infoBox: false,
    sceneModePicker: false, selectionIndicator: false,
    timeline: false, navigationHelpButton: false,
    useBrowserRecommendedResolution: false,
  });
  viewer.resolutionScale = 1.0;
  viewer.scene.skyBox = undefined;
  viewer.scene.skyAtmosphere.show = false;
  viewer.scene.shadowMap.enabled = false;
  viewer.scene.globe = undefined;
  viewer.scene.backgroundColor = new Cesium.Color(${bg[0]}, ${bg[1]}, ${bg[2]}, ${bg[3]});
  viewer.scene.sun = undefined;
  viewer.scene.moon = undefined;
  if (viewer.scene.imageBasedLighting) {
    viewer.scene.imageBasedLighting.imageBasedLightingFactor = new Cesium.Cartesian2(0, 0);
  }
  window.__viewer = viewer;
}
${CAPTURE_FN}
initViewer().then(() => { window.__ready = true; })
  .catch(e => { window.__error = String(e); window.__ready = true; });
<\/script></body></html>`;
    await page.setContent(html, { waitUntil: 'domcontentloaded' });
    await page.waitForFunction('window.__ready === true', { timeout: 60000 });
    const err = await page.evaluate('window.__error');
    if (err) throw new Error('viewer init failed: ' + err);
    // Canvas backing store: set directly. viewer.resize() early-returns when
    // CSS client size is unchanged (CesiumWidget caches _canvasClientWidth),
    // leaving the canvas at its 300x150 default. Setting canvas.width/height
    // clears the framebuffer, but at init nothing has rendered yet.
    // (Same reason cesium_render.js re-syncs after layout.)
    await page.evaluate((w, h) => {
      const container = document.getElementById('cesiumContainer');
      container.style.width = w + 'px';
      container.style.height = h + 'px';
      void container.offsetWidth; // force layout
      const canvas = document.querySelector('#cesiumContainer canvas');
      canvas.width = w;
      canvas.height = h;
    }, width, height);
    return page;
  }

  let page = await newPage();
  let ok = 0, failed = 0;

  for (const fx of fixtures) {
    const rel = path.relative(root, path.resolve(fx.file));
    const url = `http://localhost:${port}/${rel.split(path.sep).join('/')}`;
    const benchDir = path.join(outDir, fx.name);
    fs.mkdirSync(benchDir, { recursive: true });
    try {
      // Per-fixture watchdog: 120s. On timeout, rebuild the page and continue.
      const result = await Promise.race([
        page.evaluate((u) => window.__capture(u), url),
        new Promise((_, reject) => setTimeout(() => reject(new Error('capture timeout 120s')), 120000)),
      ]);
      if (result && result.ok && result.dataUrl) {
        const base64 = result.dataUrl.replace(/^data:image\/png;base64,/, '');
        fs.writeFileSync(path.join(benchDir, 'render.png'), Buffer.from(base64, 'base64'));
        fs.writeFileSync(path.join(benchDir, 'params.json'), JSON.stringify(result.params, null, 2));
        console.log(`[w${worker}][${fx.name}] OK (tilesLoaded=${result.tilesLoaded})`);
        ok++;
      } else {
        console.error(`[w${worker}][${fx.name}] FAILED: ${(result && result.error) || 'no data'}`);
        failed++;
      }
    } catch (e) {
      console.error(`[w${worker}][${fx.name}] ERROR: ${e.message} -- rebuilding page`);
      failed++;
      try { await page.close(); } catch (_) {}
      try { page = await newPage(); } catch (e2) { console.error('page rebuild failed: ' + e2.message); }
    }
  }

  await browser.close();
  dataServer.close();
  cesiumServer.close();
  return { ok, failed };
}

main().catch(e => { console.error(e); process.exit(1); });
