#!/usr/bin/env node
/**
 * P37-C2: CesiumJS headless rendering harness.
 *
 * For each tileset, renders with cesium.js using the SAME camera parameters
 * as our demo (see P37-C-param-alignment.md), then screenshots.
 *
 * Usage:
 *   node cesium_render.js --tileset <path> --output <png> \
 *     --eye x,y,z --target x,y,z --up x,y,z --fov deg \
 *     --background r,g,b,a --width W --height H
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

  const eye = args.eye.split(',').map(Number);
  const target = args.target.split(',').map(Number);
  const up = (args.up || '0,0,1').split(',').map(Number);
  const fov = parseFloat(args.fov || '60');
  const bg = (args.background || '0,0,0,1').split(',').map(Number);
  const width = parseInt(args.width || '800');
  const height = parseInt(args.height || '600');

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
<style>html,body,#cesiumContainer{margin:0;padding:0;width:${width}px;height:${height}px;overflow:hidden}</style>
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

  // Set camera (same params as our demo).
  viewer.camera.frustum.fov = Cesium.Math.toRadians(${fov});
  viewer.camera.frustum.aspectRatio = ${width} / ${height};
  viewer.camera.frustum.near = 0.1;
  viewer.camera.frustum.far = 10000.0;
  viewer.camera.setView({
    destination: new Cesium.Cartesian3(${eye[0]}, ${eye[1]}, ${eye[2]}),
    orientation: {
      direction: new Cesium.Cartesian3(
        ${target[0]} - ${eye[0]}, ${target[1]} - ${eye[1]}, ${target[2]} - ${eye[2]}),
      up: new Cesium.Cartesian3(${up[0]}, ${up[1]}, ${up[2]}),
    },
  });

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

    await page.setContent(html, { waitUntil: 'networkidle0' });
    await page.waitForFunction('window.__done === true', { timeout: 30000 });

    const error = await page.evaluate('window.__error');
    if (error) {
      console.error('Cesium error:', error);
      process.exit(1);
    }

    const tilesLoaded = await page.evaluate('window.__tilesLoaded');
    console.log(`tilesLoaded=${tilesLoaded}`);

    await page.screenshot({ path: output });
    console.log(`Screenshot: ${output}`);

    cesiumServer.close();
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch(e => { console.error(e); process.exit(1); });
