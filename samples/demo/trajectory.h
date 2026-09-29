// Deterministic camera trajectory player (see docs/adr/0014-trajectory-replay.md).
//
// A trajectory is a CSV keyframe list: "frame,yaw,pitch,distance" per line,
// '#' starts a comment. The player maps a *logical frame index* (count of
// successful renderFrame() calls) to orbit-camera parameters with smoothstep
// interpolation between keyframes. Pure function of the frame index — no
// wall clock, no threads, no SDL, no SDK dependency — so the same trajectory
// replays bit-identically.
//
// Lives in the demo layer on purpose: trajectory playback is scripted host
// camera input, and the host (not the SDK) owns the camera (ADR-0012).

#pragma once

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace trajectory {

struct Keyframe {
    int frame = 0;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    double distance = 0.0;
};

struct CameraPose {
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    double distance = 0.0;
};

inline double smoothstep(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

// Shortest-path angular interpolation: 350 -> 10 goes +20, not -340.
inline double lerpAngle(double aDeg, double bDeg, double t) {
    double d = std::fmod(bDeg - aDeg, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return aDeg + d * t;
}

inline double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

class Player {
public:
    // Throws std::runtime_error on unparsable input or < 1 keyframe.
    static Player loadCsv(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("trajectory: cannot open " + path);
        }
        std::vector<Keyframe> keys;
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            // Strip comments and whitespace-only lines.
            std::string s;
            for (char c : line) {
                if (c == '#') break;
                s += c;
            }
            std::istringstream trimmed(s);
            std::string tok;
            bool blank = true;
            while (trimmed >> tok) {
                blank = false;
                break;
            }
            if (blank) continue;
            std::istringstream row(s);
            Keyframe k;
            char comma = 0;
            if (!(row >> k.frame >> comma) || comma != ',' ||
                !(row >> k.yawDeg >> comma) || comma != ',' ||
                !(row >> k.pitchDeg >> comma) || comma != ',' ||
                !(row >> k.distance)) {
                throw std::runtime_error("trajectory: bad line " +
                                         std::to_string(lineNo) + " in " + path);
            }
            if (k.frame < 0 || k.distance <= 0.0) {
                throw std::runtime_error("trajectory: bad values at line " +
                                         std::to_string(lineNo) + " in " + path);
            }
            keys.push_back(k);
        }
        if (keys.empty()) {
            throw std::runtime_error("trajectory: no keyframes in " + path);
        }
        for (size_t i = 1; i < keys.size(); ++i) {
            if (keys[i].frame <= keys[i - 1].frame) {
                throw std::runtime_error("trajectory: keyframes not strictly "
                                         "increasing in " + path);
            }
        }
        return Player(std::move(keys));
    }

    // Camera pose at logical frame index f (0-based). Clamps past the last
    // keyframe; before the first keyframe returns the first keyframe.
    CameraPose at(int f) const {
        const Keyframe& first = keys_.front();
        const Keyframe& last = keys_.back();
        if (f <= first.frame) {
            return {first.yawDeg, first.pitchDeg, first.distance};
        }
        if (f >= last.frame) {
            return {last.yawDeg, last.pitchDeg, last.distance};
        }
        for (size_t i = 1; i < keys_.size(); ++i) {
            const Keyframe& a = keys_[i - 1];
            const Keyframe& b = keys_[i];
            if (f >= a.frame && f <= b.frame) {
                const double t = smoothstep(
                    static_cast<double>(f - a.frame) /
                    static_cast<double>(b.frame - a.frame));
                return {lerpAngle(a.yawDeg, b.yawDeg, t),
                        lerp(a.pitchDeg, b.pitchDeg, t),
                        lerp(a.distance, b.distance, t)};
            }
        }
        return {last.yawDeg, last.pitchDeg, last.distance}; // unreachable
    }

    int lastFrame() const { return keys_.back().frame; }
    size_t keyframeCount() const { return keys_.size(); }

private:
    explicit Player(std::vector<Keyframe> keys) : keys_(std::move(keys)) {}
    std::vector<Keyframe> keys_;
};

} // namespace trajectory
