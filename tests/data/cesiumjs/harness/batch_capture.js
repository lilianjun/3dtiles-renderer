#!/usr/bin/env node
/**
 * Batch capture Cesium.js reference renders + params for conformance testing.
 *
 * For each tileset in the input list:
 *   1. Renders via cesium_render.js (Puppeteer + headless Chrome)
 *   2. Extracts ACTUAL camera params from Cesium session (CAPTURE_PARAMS)
 *   3. Saves to <outDir>/<name>/render.png + params.json
 *
 * Usage:
 *   node batch_capture.js --list <tilesets.txt> --out <dir> [--width 800 --height 600]
 *
 * tilesets.txt format (one per line):
 *   <name> <tileset.json path>
 *
 * Example:
 *   PointCloudRGB /path/to/tileset.json
 */
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

function parseArgs() {
  const args = process.argv.slice(2);
  const out = {};
  for (let i = 0; i < args.length; i += 2) {
    const key = args[i].replace(/^--/, '');
    out[key] = args[i + 1];
  }
  return out;
}

async function runCapture(name, tilesetPath, outDir, width, height) {
  const benchDir = path.join(outDir, name);
  fs.mkdirSync(benchDir, { recursive: true });
  
  const renderPng = path.join(benchDir, 'render.png');
  const paramsJson = path.join(benchDir, 'params.json');
  
  // Default camera: will be overridden by actual extraction
  // Use a reasonable default; the EXTRACTED params are what matter
  const cmd = 'node';
  const cmdArgs = [
    path.join(__dirname, 'cesium_render.js'),
    '--tileset', tilesetPath,
    '--output', renderPng,
    '--eye', '0,-25,15',
    '--target', '0,0,0',
    '--up', '0,0,1',
    '--fov', '60',
    '--background', '0.1,0.1,0.1,1',
    '--width', String(width),
    '--height', String(height),
  ];
  
  const env = { ...process.env, CAPTURE_PARAMS: paramsJson };
  
  return new Promise((resolve, reject) => {
    console.log(`[${name}] Capturing...`);
    const proc = spawn(cmd, cmdArgs, { env, stdio: 'inherit' });
    proc.on('close', (code) => {
      if (code === 0 && fs.existsSync(renderPng) && fs.existsSync(paramsJson)) {
        console.log(`[${name}] OK: ${renderPng}`);
        resolve(true);
      } else {
        console.error(`[${name}] FAILED (code=${code})`);
        resolve(false);
      }
    });
    proc.on('error', (err) => {
      console.error(`[${name}] ERROR: ${err.message}`);
      resolve(false);
    });
  });
}

async function main() {
  const args = parseArgs();
  const listFile = args.list;
  const outDir = args.out;
  const width = parseInt(args.width || '800');
  const height = parseInt(args.height || '600');
  
  if (!listFile || !outDir) {
    console.error('Usage: node batch_capture.js --list <tilesets.txt> --out <dir> [--width 800 --height 600]');
    process.exit(1);
  }
  
  const lines = fs.readFileSync(listFile, 'utf-8').split('\n').filter(l => l.trim() && !l.startsWith('#'));
  console.log(`Batch capture: ${lines.length} tilesets -> ${outDir}`);
  
  let ok = 0, fail = 0;
  for (const line of lines) {
    const [name, ...rest] = line.trim().split(/\s+/);
    const tilesetPath = rest.join(' ');
    const success = await runCapture(name, tilesetPath, outDir, width, height);
    if (success) ok++; else fail++;
  }
  
  console.log(`\nDone: ${ok} ok, ${fail} failed`);
  process.exit(fail > 0 ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
