#!/usr/bin/env python3
"""P37-A: Inventory cesium.js test tilesets by format and features."""
import json, os, sys
from pathlib import Path

ROOT = Path(__file__).parent
SOURCES = {
    "specs": ROOT / "Specs/Data/Cesium3DTiles",
    "samples": ROOT / "Apps/SampleData/Cesium3DTiles",
    "3d-tiles-samples": ROOT / "3d-tiles-samples",
}

def analyze_tileset(tileset_path):
    """Return (formats, features) for a tileset.json."""
    formats = set()
    features = set()
    try:
        with open(tileset_path) as f:
            ts = json.load(f)
    except Exception as e:
        return {"_error": str(e)}, set()

    base = tileset_path.parent
    asset_ver = ts.get("asset", {}).get("version", "?")
    features.add(f"tileset-version-{asset_ver}")

    if "extensionsUsed" in ts:
        for ext in ts["extensionsUsed"]:
            features.add(f"ext-{ext}")

    def walk(node):
        # content formats
        content = node.get("content", {})
        # 1.0 style: content.uri / content.url
        # 1.1 style: content.uri (also)
        uri = content.get("uri") or content.get("url", "")
        if uri:
            # strip query params
            u = uri.split("?")[0].split("#")[0]
            # data: URIs embed content inline; detect format from base64 magic
            if u.startswith("data:"):
                import base64
                try:
                    b64 = u.split(",", 1)[1]
                    magic = base64.b64decode(b64[:8])
                    if magic[:4] in (b"b3dm", b"i3dm", b"pnts", b"cmpt"):
                        formats.add(magic[:4].decode())
                    elif magic[:4] == b"glTF":
                        formats.add("glb")
                    features.add("data-uri-content")
                except Exception:
                    pass
            else:
                ext = os.path.splitext(u)[1].lower().lstrip(".")
                if ext:
                    formats.add(ext)
                # check actual file for magic (b3dm/i3dm/pnts/cmpt/glb)
                try:
                    fp = base / u
                    if fp.exists() and fp.is_file():
                        with open(fp, "rb") as f:
                            magic = f.read(4)
                        if magic in (b"b3dm", b"i3dm", b"pnts", b"cmpt"):
                            formats.add(magic.decode())
                        elif magic == b"glTF":
                            formats.add("glb")
                except Exception:
                    pass
        # 1.1 multiple contents
        for c in content.get("contents", []) if isinstance(content, dict) else []:
            uri = c.get("uri", "")
            if uri:
                ext = os.path.splitext(uri.split("?")[0])[1].lower().lstrip(".")
                if ext:
                    formats.add(ext)
        # implicit tiling
        if "implicitTiling" in node:
            features.add("implicit-tiling")
            it = node["implicitTiling"]
            features.add(f"implicit-{it.get('subdivisionScheme', '?')}")
        # metadata
        if "metadata" in node:
            features.add("tile-metadata")
        # 1.1 content metadata
        if isinstance(content, dict) and "metadata" in content:
            features.add("content-metadata")
        # bounding volume types
        bv = node.get("boundingVolume", {})
        for k in ("box", "region", "sphere"):
            if k in bv:
                features.add(f"bv-{k}")
        # refine
        if "refine" in node:
            features.add(f"refine-{node['refine']}")
        # geometricError 0 (leaf-ish)
        # children
        for child in node.get("children", []):
            walk(child)

    # extensions at tileset level
    if "extensions" in ts:
        for ext in ts["extensions"]:
            features.add(f"ext-{ext}")

    walk(ts.get("root", {}))
    return formats, features

def main():
    rows = []
    for src_name, src_path in SOURCES.items():
        if not src_path.exists():
            print(f"WARN: {src_path} missing", file=sys.stderr)
            continue
        for tj in sorted(src_path.rglob("tileset.json")):
            rel = tj.relative_to(ROOT)
            formats, features = analyze_tileset(tj)
            rows.append((src_name, str(rel), formats, features))

    # Summary by format
    from collections import Counter
    fmt_counter = Counter()
    feat_counter = Counter()
    for _, _, fmts, feats in rows:
        for f in fmts:
            if not f.startswith("_"):
                fmt_counter[f] += 1
        for f in feats:
            feat_counter[f] += 1

    print(f"# CesiumJS Test Data Inventory")
    print(f"\nTotal tilesets: {len(rows)}")
    print(f"\n## By source")
    for src_name in SOURCES:
        n = sum(1 for r in rows if r[0] == src_name)
        print(f"- {src_name}: {n}")
    print(f"\n## By content format")
    for fmt, n in fmt_counter.most_common():
        print(f"- {fmt}: {n}")
    print(f"\n## By feature")
    for feat, n in feat_counter.most_common():
        print(f"- {feat}: {n}")

    # Write detailed CSV
    with open(ROOT / "INVENTORY.csv", "w") as f:
        f.write("source,path,formats,features\n")
        for src, rel, fmts, feats in rows:
            fmts_s = ";".join(sorted(str(x) for x in fmts))
            feats_s = ";".join(sorted(feats))
            f.write(f'{src},"{rel}","{fmts_s}","{feats_s}"\n')
    print(f"\nWrote INVENTORY.csv ({len(rows)} rows)")

if __name__ == "__main__":
    main()
