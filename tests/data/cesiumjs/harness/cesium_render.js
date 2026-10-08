#!/usr/bin/env node
/**
 * P37-C2: CesiumJS headless rendering harness.
 *
 * Renders a tileset with cesium.js and captures the reference image
 * DIRECTLY from the WebGL canvas (canvas.toDataURL), plus the ACTUAL
 * camera parameters extracted from the Cesium session after rendering.
 *
 * Camera: by default the camera AUTO-Frames the whole tileset via
 *   viewer.camera.viewBoundingSphere(tileset.boundingSphere)
 * which is the synchronous, deterministic equivalent of viewer.zoomTo()
 * (same default offset: heading 0, pitch -45deg, range auto-computed
 * from bounding-sphere radius and fov). Explicit --eye/--target/--up
 * overrides auto-framing (manual mode, for debugging only).
 *
 * Usage:
 *   node cesium_render.js --tileset <path> --output <png>
 *     [--eye x,y,z --target x,y,z --up x,y,z] [--fov deg]
 *     [--background r,g,b,a] [--width W] [--height H]
 *
 * Params export: set CAPTURE_PARAMS=/path/params.json to save the
 * post-render camera params (position/direction/up/fov/aspectRatio/
 * near/far) + width/height/backgroundColor as JSON.
 *
 * The tileset path is served via a local HTTP server (cesium.js requires
 * http:// for tile loading, not file://).
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

// Minimal static file server for the tileset directory.
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
      // Security: prevent directory traversal.
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

async function main() {
  const args = parseArgs();
  const tilesetPath = path.resolve(args.tileset);
  const tilesetDir = path.dirname(tilesetPath);
  const output = args.output || '/tmp/cesium_render.png';

  const eye = args.eye ? args.eye.split(',').map(Number) : null;
  const target = args.target ? args.target.split(',').map(Number) : null;
  const up = (args.up || '0,0,1').split(',').map(Number);
  const fov = parseFloat(args.fov || '60');
  const bg = (args.background || '0,0,0,1').split(',').map(Number);
  const width = parseInt(args.width || '400');
  const height = parseInt(args.height || '300');
  // Auto-framing is the default. Explicit --eye switches to manual mode.
  const autoFrame = !eye;

  // Serve the tileset directory.
  const port = 18777;
  const server = await serveDirectory(tilesetDir, port);
  const tilesetUrl = `http://localhost:${port}/${path.basename(tilesetPath)}`;

  // CesiumJS from local checkout (sparse checkout has the built version?).
  // We use the npm cesium package instead for reliability.
  const cesiumPath = path.join(__dirname, 'node_modules', 'cesium', 'Build', 'CesiumUnminified');

  const browser = await puppeteer.launch({
    headless: 'new',
    executablePath: '/home/hatch/workspace/chrome-feasibility/chrome-headless-shell-linux64/chrome-headless-shell',
    args: [
      '--use-gl=swiftshader',  // Software WebGL
      '--enable-unsafe-swiftshader',
      '--no-sandbox',
      '--disable-dev-shm-usage',
      `--window-size=${width},${height}`,
    ],
  });

  try {
    const page = await browser.newPage();
    await page.setViewport({ width, height, deviceScaleFactor: 1 });

    // Build the HTML page.
    const html = `
<!DOCTYPE html>
<html><head><meta charset="utf-8">
<script src="http://localhost:${port + 1}/Cesium.js"></script>
<style>html,body,#cesiumContainer{margin:0;padding:0;width:${width}px;height:${height}px;overflow:hidden}#cesiumContainer canvas{width:${width}px!important;height:${height}px!important;display:block}.cesium-viewer-bottom{display:none!important}</style>
</head><body>
<div id="cesiumContainer"></div>
<script>
async function run() {
  const viewer = new Cesium.Viewer('cesiumContainer', {
    animation: false, baseLayerPicker: false, fullscreenButton: false,
    geocoder: false, homeButton: false, infoBox: false,
    sceneModePicker: false, selectionIndicator: false,
    timeline: false, navigationHelpButton: false,
    useBrowserRecommendedResolution: false,
  });
  viewer.resolutionScale = 1.0;
  // Ensure the canvas backing store matches the requested size BEFORE
  // any rendering happens (setting canvas.width/height later would
  // clear the framebuffer).
  viewer.resize();

  // P37-C param alignment: disable sky, atmosphere, shadows, IBL.
  viewer.scene.skyBox = undefined;
  viewer.scene.skyAtmosphere.show = false;
  viewer.scene.shadowMap.enabled = false;
  viewer.scene.globe = undefined;  // No globe, just the tileset.
  viewer.scene.backgroundColor = new Cesium.Color(${bg[0]}, ${bg[1]}, ${bg[2]}, ${bg[3]});
  viewer.scene.sun = undefined;
  viewer.scene.moon = undefined;
  // Disable IBL.
  if (viewer.scene.imageBasedLighting) {
    viewer.scene.imageBasedLighting.imageBasedLightingFactor = new Cesium.Cartesian2(0, 0);
  }

  const tileset = await Cesium.Cesium3DTileset.fromUrl('${tilesetUrl}');
  viewer.scene.primitives.add(tileset);
  await tileset.readyPromise;

  // Set frustum first (fov affects auto-framing range computation).
  viewer.camera.frustum.fov = Cesium.Math.toRadians(${fov});
  viewer.camera.frustum.aspectRatio = ${width} / ${height};
  viewer.camera.frustum.near = 0.1;
  viewer.camera.frustum.far = 10000.0;
  window.__viewer = viewer;  // Store for param extraction

  if (${autoFrame}) {
    // Auto-frame the whole tileset: synchronous, deterministic equivalent
    // of viewer.zoomTo(tileset) (same default offset math, no flight
    // animation). The camera ends up looking at the bounding-sphere
    // center from a distance that fits the whole sphere in the frustum.
    const bs = tileset.boundingSphere;
    if (!bs) { throw new Error('auto-framing failed: tileset.boundingSphere is unavailable'); }
    viewer.camera.viewBoundingSphere(bs);
    window.__autoFramed = true;
  } else {
    // Manual override (debugging only): explicit eye/target/up.
    viewer.camera.setView({
      destination: new Cesium.Cartesian3(${eye ? eye[0] : 0}, ${eye ? eye[1] : 0}, ${eye ? eye[2] : 0}),
      orientation: {
        direction: new Cesium.Cartesian3(
          ${target ? target[0] - eye[0] : 0}, ${target ? target[1] - eye[1] : 0}, ${target ? target[2] - eye[2] : 0}),
        up: new Cesium.Cartesian3(${up[0]}, ${up[1]}, ${up[2]}),
      },
    });
    window.__autoFramed = false;
  }

  // Wait for tiles to load (allTilesLoaded event or timeout).
  await new Promise((resolve) => {
    let done = false;
    const finish = () => { if (!done) { done = true; resolve(); } };
    tileset.allTilesLoaded.addEventListener(finish);
    setTimeout(finish, 15000);  // 15s timeout.
  });
  // Extra settle frames.
  await new Promise(r => setTimeout(r, 1000));

  window.__done = true;
  window.__tilesLoaded = tileset.tilesLoaded;
}
run().catch(e => { window.__error = String(e); window.__done = true; });
</script>
</body></html>`;

    // Serve cesium.js on port+1.
    const cesiumServer = await serveDirectory(cesiumPath, port + 1);

    // The page signals completion itself via window.__done; don't wait for
    // network idle (fragile with large local script fetches).
    await page.setContent(html, { waitUntil: 'domcontentloaded' });
    await page.waitForFunction('window.__done === true', { timeout: 60000 });

    // Check for Cesium-side errors BEFORE touching the viewer: if the
    // tileset failed to load, window.__viewer was never created and the
    // resize below would throw a confusing TypeError instead.
    const error = await page.evaluate('window.__error');
    if (error) {
      console.error('Cesium error:', error);
      process.exit(1);
    }

    // Canvas sizing must happen AFTER layout is done. The in-page
    // viewer.resize() runs before first layout (clientWidth reads 0),
    // leaving the canvas at its 300x150 default. Re-sync here, then do
    // a fresh synchronous render before export (see below).
    await page.evaluate((w, h) => {
        const viewer = window.__viewer;
        if (!viewer) return;
        const container = document.getElementById('cesiumContainer');
        container.style.width = w + 'px';
        container.style.height = h + 'px';
        void container.offsetWidth; // force layout
        viewer.resize();
    }, width, height);
    // NOTE: do NOT touch canvas.width/height directly: it would clear
    // the framebuffer. viewer.resize() + a fresh render is the safe path.

    const tilesLoaded = await page.evaluate('window.__tilesLoaded');
    console.log(`tilesLoaded=${tilesLoaded}`);

    // Extract ACTUAL camera params from Cesium (must be extracted after render, per rule)
    const extracted = await page.evaluate(() => {
        const viewer = window.__viewer;
        if (!viewer) return null;
        const cam = viewer.camera;
        const frustum = cam.frustum;
        return {
            position: [cam.position.x, cam.position.y, cam.position.z],
            direction: [cam.direction.x, cam.direction.y, cam.direction.z],
            up: [cam.up.x, cam.up.y, cam.up.z],
            fov: Cesium.Math.toDegrees(frustum.fov),
            aspectRatio: frustum.aspectRatio,
            near: frustum.near,
            far: frustum.far,
        };
    });
    
    if (extracted && process.env.CAPTURE_PARAMS) {
        const autoFramed = await page.evaluate('window.__autoFramed === true');
        const params = {
            width: width,
            height: height,
            autoFramed: autoFramed,
            camera: extracted,
            backgroundColor: [bg[0], bg[1], bg[2], bg[3]],
        };
        fs.writeFileSync(process.env.CAPTURE_PARAMS, JSON.stringify(params, null, 2));
        console.log(`Params extracted and saved: ${process.env.CAPTURE_PARAMS}`);
    }

    // Get image directly from the WebGL canvas (not a page screenshot).
    // Cesium does NOT set preserveDrawingBuffer, so the drawing buffer may
    // be cleared after compositing. Do a synchronous render and export the
    // canvas in the SAME JS task, while the buffer is still valid.
    const dataUrl = await page.evaluate(() => {
        const viewer = window.__viewer;
        if (!viewer) return null;
        viewer.scene.render();
        const canvas = document.querySelector('#cesiumContainer canvas');
        if (!canvas) return null;
        return canvas.toDataURL('image/png');
    });
    
    if (!dataUrl) {
        console.error('Failed to get canvas image');
        process.exit(1);
    }
    
    // Save data URL to file
    const base64 = dataUrl.replace(/^data:image\/png;base64,/, '');
    fs.writeFileSync(output, Buffer.from(base64, 'base64'));
    console.log(`Screenshot (from canvas): ${output}`);

    cesiumServer.close();
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch(e => { console.error(e); process.exit(1); });
