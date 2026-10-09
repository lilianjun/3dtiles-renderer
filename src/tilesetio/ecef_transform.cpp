// Unified ECEF transform handling implementation.

#include "ecef_transform.h"
#include <cmath>

namespace tilesetio {

EcefTransformResult computeEcefTransform(
    const glm::dmat4& tileTransform,
    const glm::dvec3& rtcCenter,
    const glm::dmat4& upAxisFix,
    const glm::dmat4& modelMatrix,
    const glm::dvec3& localOrigin,
    bool isEnuInstances) {
    
    EcefTransformResult result;
    result.localOrigin = localOrigin;
    
    // Detect ECEF: if rtcCenter is large (>1000m), or if tileTransform
    // has large translation, we're in ECEF mode.
    double rtcLen = glm::length(rtcCenter);
    double tileTransLen = glm::length(glm::dvec3(
        tileTransform[3][0], tileTransform[3][1], tileTransform[3][2]));
    result.isEcef = (rtcLen > 1000.0) || (tileTransLen > 1000.0);
    
    // Compute world transform in double precision:
    //   worldT = modelMatrix * tileTransform * translate(rtcCenter) * upAxisFix
    glm::dmat4 worldT = tileTransform;
    
    if (rtcCenter != glm::dvec3(0.0)) {
        glm::dmat4 rtcT(1.0);
        rtcT[3][0] = rtcCenter.x;
        rtcT[3][1] = rtcCenter.y;
        rtcT[3][2] = rtcCenter.z;
        worldT = worldT * rtcT;
    }
    
    worldT = worldT * upAxisFix;
    worldT = modelMatrix * worldT;
    
    // Rebase: subtract localOrigin for float32 GPU precision.
    // This brings ECEF coordinates (~6e6) down to local (~0).
    worldT[3][0] -= localOrigin.x;
    worldT[3][1] -= localOrigin.y;
    worldT[3][2] -= localOrigin.z;
    
    result.worldTransform = worldT;
    return result;
}

glm::dmat4 undoEnuConjugation(
    const glm::dmat4& conjugated,
    const glm::dmat4& nodeTransform,
    const glm::dmat4& upToZ) {
    
    // The converter did: conjugated = toTileInv * compose * toTile
    // where toTile = upToZ * nodeTransform.
    // To undo: compose = toTile * conjugated * toTileInv.
    glm::dmat4 toTile = upToZ * nodeTransform;
    glm::dmat4 toTileInv = glm::inverse(toTile);
    return toTile * conjugated * toTileInv;
}

glm::dmat4 computeEnuMatrix(const glm::dvec3& ecefPosition) {
    // Compute the ENU frame at the given ECEF position.
    // Up = normalize(position)
    // East = normalize(cross((0,0,1), up))
    // North = cross(up, east)
    
    glm::dvec3 up = glm::normalize(ecefPosition);
    glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    glm::dvec3 north = glm::cross(up, east);
    
    // Build 4x4 matrix: columns are east, north, up, position.
    glm::dmat4 result(1.0);
    result[0][0] = east.x;  result[0][1] = east.y;  result[0][2] = east.z;
    result[1][0] = north.x; result[1][1] = north.y; result[1][2] = north.z;
    result[2][0] = up.x;    result[2][1] = up.y;    result[2][2] = up.z;
    result[3][0] = ecefPosition.x;
    result[3][1] = ecefPosition.y;
    result[3][2] = ecefPosition.z;
    
    return result;
}

} // namespace tilesetio
