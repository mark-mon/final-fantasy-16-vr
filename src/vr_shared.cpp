#include "vr_shared.h"

#include <atomic>
#include <mutex>

namespace vrs {
namespace {
std::mutex g_mutex;
HeadSample g_head;
constexpr int kRing = 16;
UsedSample g_used[kRing];
int g_usedNext = 0;
std::atomic<uint64_t> g_presents{0};
}  // namespace

void PublishHead(const HeadSample& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_head = s;
}

HeadSample LatestHead() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_head;
}

void RecordUsed(const UsedSample& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_used[g_usedNext] = s;
    g_usedNext = (g_usedNext + 1) % kRing;
}

bool UsedForPresent(uint64_t presentCount, int lag, UsedSample* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Latest update recorded while the present counter was <= presentCount - lag.
    uint64_t want = presentCount >= (uint64_t)lag ? presentCount - lag : 0;
    const UsedSample* best = nullptr;
    for (const auto& u : g_used) {
        if (!u.valid || u.presentTag > want) continue;
        if (!best || u.presentTag > best->presentTag) best = &u;
    }
    if (!best) {  // nothing that old yet: take the newest we have
        for (const auto& u : g_used)
            if (u.valid && (!best || u.presentTag > best->presentTag)) best = &u;
    }
    if (!best) return false;
    *out = *best;
    return true;
}

uint64_t PresentCount() { return g_presents.load(); }
void CountPresent() { g_presents.fetch_add(1); }

void QuatToMat3(const float q[4], float m[3][3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

void QuatMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void QuatConj(const float q[4], float o[4]) {
    o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3];
}

void QuatRotate(const float q[4], const float v[3], float o[3]) {
    float m[3][3];
    QuatToMat3(q, m);
    for (int i = 0; i < 3; ++i) o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

void PoseMul(const Pose& a, const Pose& b, Pose* o) {
    float q[4], t[3];
    QuatMul(a.q, b.q, q);
    QuatRotate(a.q, b.p, t);
    for (int i = 0; i < 4; ++i) o->q[i] = q[i];
    for (int i = 0; i < 3; ++i) o->p[i] = a.p[i] + t[i];
}

}  // namespace vrs
