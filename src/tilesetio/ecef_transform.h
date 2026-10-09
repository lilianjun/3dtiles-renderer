// Unified ECEF transform handling for 3D Tiles.
// 
// This module unifies the handling of Earth-Centered Earth-Fixed (ECEF)
// coordinates across different tile formats:
// - b3dm with RTC_CENTER (vertices are local, RTC is ECEF offset)
// - b3dm with WGS84 (vertices are directly in ECEF, no RTC)
// - i3dm with EAST_NORTH_UP (instances are in ECEF, converter conjugates)
// - cmpt with mixed content (each inner content has its own RTC/ENU)
//
// The key insight: all ECEF handling should go through a single code path
// that computes the world transform in double precision, then rebases by
// subtracting a local origin (for float32 GPU precision).

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace tilesetio {

// Result of ECEF transform computation.
struct EcefTransformResult {
    // The world transform in double precision, already rebased by
    // subtracting localOrigin (i.e., ready for float32 GPU upload).
    // Computed as: modelMatrix * tileTransform * rtcTranslate * upAxisFix
    //              with translation rebased by -localOrigin.
    glm::dmat4 worldTransform;
    
    // The local origin used for rebase (ECEF coordinates).
    glm::dvec3 localOrigin;
    
    // True if the tile uses ECEF coordinates (vs. local coordinates).
    bool isEcef;
};

// Compute the unified world transform for a tile.
// 
// Parameters:
// - tileTransform: the tile's transform from tileset.json (or identity)
// - rtcCenter: the RTC_CENTER from b3dm/i3dm feature table (or zero)
// - upAxisFix: the Y-up to Z-up conversion matrix (or identity)
// - modelMatrix: the global model matrix (or identity)
// - localOrigin: the rebase origin (typically root tile bounding volume center)
// - isEnuInstances: true if i3dm has EAST_NORTH_UP (instances need special handling)
//
// Returns the EcefTransformResult with the rebased world transform.
EcefTransformResult computeEcefTransform(
    const glm::dmat4& tileTransform,
    const glm::dvec3& rtcCenter,
    const glm::dmat4& upAxisFix,
    const glm::dmat4& modelMatrix,
    const glm::dvec3& localOrigin,
    bool isEnuInstances = false);

// For i3dm with EAST_NORTH_UP: undo the converter's conjugation.
// The converter does: conjugated = toTileInv * compose * toTile
// where toTile = upToZ * nodeTransform.
// This function recovers the original compose matrix.
//
// Parameters:
// - conjugated: the instance transform from the Model (TRANSLATION/ROTATION/SCALE)
// - nodeTransform: the glTF node's transform (base)
// - upToZ: the Y-up to Z-up rotation matrix
//
// Returns the original (unconjugated) instance transform.
glm::dmat4 undoEnuConjugation(
    const glm::dmat4& conjugated,
    const glm::dmat4& nodeTransform,
    const glm::dmat4& upToZ);

// Compute the ENU (East-North-Up) rotation matrix for a given ECEF position.
// Returns a 4x4 matrix with rotation [east, north, up] and translation = position.
glm::dmat4 computeEnuMatrix(const glm::dvec3& ecefPosition);

} // namespace tilesetio
