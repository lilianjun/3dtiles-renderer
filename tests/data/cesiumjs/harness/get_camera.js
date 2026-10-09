const puppeteer = require('puppeteer');

async function main() {
    const browser = await puppeteer.launch({
        headless: 'new',
        executablePath: '/home/hatch/workspace/chrome-feasibility/chrome-headless-shell-linux64/chrome-headless-shell',
        args: ['--use-gl=swiftshader', '--enable-unsafe-swiftshader', '--no-sandbox'],
    });
    const page = await browser.newPage();
    await page.setViewport({ width: 400, height: 300, deviceScaleFactor: 1 });
    
    const html = `<!DOCTYPE html><html><head><meta charset="utf-8">
<script src="http://localhost:8899/../../workspace/3dtiles-renderer/tests/data/cesiumjs/Build/CesiumUnminified/Cesium.js"></script>
<style>html,body,#c{margin:0;padding:0;width:400px;height:300px;overflow:hidden}.cesium-viewer-bottom{display:none!important}</style>
</head><body><div id="c"></div><script>
async function run() {
  const viewer = new Cesium.Viewer('c', {
    animation:false,baseLayerPicker:false,fullscreenButton:false,geocoder:false,
    homeButton:false,infoBox:false,sceneModePicker:false,selectionIndicator:false,
    timeline:false,navigationHelpButton:false,useBrowserRecommendedResolution:false,
  });
  viewer.resolutionScale = 1.0;
  viewer.scene.backgroundColor = new Cesium.Color(0.1,0.1,0.1,1.0);
  const tileset = await Cesium.Cesium3DTileset.fromUrl('http://localhost:8899/tileset.json');
  viewer.scene.primitives.add(tileset);
  await tileset.readyPromise;
  await viewer.zoomTo(tileset);
  await new Promise(r => setTimeout(r, 2000));
  const cam = viewer.camera;
  window.__cam = {
    eye: [cam.position.x, cam.position.y, cam.position.z],
    direction: [cam.direction.x, cam.direction.y, cam.direction.z],
    up: [cam.up.x, cam.up.y, cam.up.z],
    fov: Cesium.Math.toDegrees(cam.frustum.fov),
  };
  window.__done = true;
}
run();
</script></body></html>`;
    
    await page.setContent(html, { waitUntil: 'networkidle0' });
    await page.waitForFunction('window.__done === true', { timeout: 30000 });
    const cam = await page.evaluate('window.__cam');
    console.log(JSON.stringify(cam, null, 2));
    await browser.close();
}
main().catch(e => { console.error(e); process.exit(1); });
