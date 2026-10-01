#!/bin/bash
# P37-A: Fetch cesium.js 3D Tiles test data.
# Usage: ./fetch_cesiumjs_data.sh
# Downloads ~138MB into tests/data/cesiumjs/ (gitignored).
set -e
cd "$(dirname "$0")"

if [ ! -d "Specs" ]; then
  echo "Fetching CesiumGS/cesium (sparse: Specs/Data/Cesium3DTiles, Apps/SampleData/Cesium3DTiles)..."
  git init -q
  git remote add origin https://github.com/CesiumGS/cesium.git 2>/dev/null || true
  git config core.sparseCheckout true
  printf "Specs/Data/Cesium3DTiles/\nApps/SampleData/Cesium3DTiles/\n" > .git/info/sparse-checkout
  git pull --depth 1 origin main
else
  echo "Specs/ already exists, skipping cesium fetch"
fi

if [ ! -d "3d-tiles-samples" ]; then
  echo "Fetching CesiumGS/3d-tiles-samples..."
  git clone --depth 1 https://github.com/CesiumGS/3d-tiles-samples.git 3d-tiles-samples
else
  echo "3d-tiles-samples/ already exists, skipping"
fi

echo "Done. Run ./inventory.py to regenerate INVENTORY.csv"
