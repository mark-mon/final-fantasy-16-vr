// Depth rebuild shader for the frame warp mode (warp.cpp), from Rift Apart VR's rebuild.inc:
//   RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence. https://github.com/scubasteve2365/RiftApartVR
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files, to deal in the Software without restriction (see third_party/riftapartvr/LICENSE).
// Unchanged shader code. Inputs: t0 the drawn eye's image, t1 the rebuilt eye's own last image, t2 the drawn eye's depth,
// t3 the rebuilt eye's last depth, t4 its last motion vectors; u0 map, u1 output, u2 own map, u3 own movement, u4 stats.
// Depth is near / view distance (reversed infinite), the camera looks down -back, tangents are left, right, up, down.
static const char kRebuildHlsl[] = R"(
cbuffer C : register(b0) {
    float4 dRight, dUp, dBack, dPos;
    float4 sRight, sUp, sBack, sPos;
    float4 hRight, hUp, hBack; // .w of the three: the held camera's position
    float4 dTan, sTan, hTan;   // left, right, up, down
    float4 misc;               // near, map width, map height, source order
    float4 pRight, pUp, pBack; // the held image's previous camera (its motion vectors point there); .w its position
    float4 pTan;
    float4 motion;             // motion vector scale x, y to image units, ticks ahead over the vectors' interval, on
    float4 fill;               // x: gap filling, 0 nearest neighbour (1.02), 1 from the background side; y: search radius;
                               // z: moving things from the drawn eye above this movement over distance per tick, 0 off;
                               // w: edges, 1 picks among the four nearest depth texels per output pixel (1.05), 2 the same for colours only, 0 nearest
    float4 band;               // x: around moving things, this many depth texels also come from the drawn eye (1.06), 0 off;
                               // y: lab source view; z: lab stats slot; w: lab stats on
    // ff16vr: things that move, found in the DRAWN eye: the object part of its rebuilt motion vectors (mvfix: its vectors
    // minus the motion its camera alone gives; t5). Such pixels, and a band around them, come from the drawn eye: the own
    // image is a tick old for them (the two eyes otherwise show a character in different poses: "double characters").
    float4 objm;               // x: threshold in render pixels; y: on (t5 holds the drawn eye's object motion); z: band radius in texels;
                               // w: t4 holds the HELD eye's object motion: its texels that were moving when drawn are not
                               // splatted (they hold the character's old pose; the drawn eye shows the current one)
    float4 objmv;              // xy: motion vector scale to image units (the drawn eye's); zw: the held eye's
};
Texture2D<float4> srcColor : register(t0);
Texture2D<float4> histColor : register(t1);
Texture2D<float>  srcDepth : register(t2);
Texture2D<float>  heldDepth : register(t3);
Texture2D<float2> heldMv : register(t4);
Texture2D<float2> srcMv : register(t5);  // ff16vr: the drawn eye's object motion (mvfix; lab view 4 shows it)
RWTexture2D<uint>   map : register(u0);
RWTexture2D<float4> outColor : register(u1);
RWTexture2D<uint>   ownMap : register(u2);
RWTexture2D<float4> ownMove : register(u3);
RWByteAddressBuffer stats : register(u4);  // lab: per frame slot, pixels that fell back to the drawn eye
SamplerState lin : register(s0);

float3 EyeRay(float2 uv, float4 t) { return float3(lerp(t.x, t.y, uv.x), lerp(t.z, t.w, uv.y), -1); }
float3 ToWorld(float3 p, float4 r, float4 u, float4 b, float4 o) { return o.xyz + r.xyz * p.x + u.xyz * p.y + b.xyz * p.z; }
float3 ToEye(float3 w, float4 r, float4 u, float4 b, float4 o) { float3 v = w - o.xyz; return float3(dot(v, r.xyz), dot(v, u.xyz), dot(v, b.xyz)); }
float2 ToUv(float3 e, float4 t) { float2 q = e.xy / -e.z; return float2((q.x - t.x) / (t.y - t.x), (q.y - t.z) / (t.w - t.z)); }

// ff16vr: does the drawn eye's texel q move by itself? Its object motion (vectors minus camera motion), in render pixels.
bool SrcMoving(int2 q, int2 size) {
    float l = length(srcMv[q] * objmv.xy * float2(size));
    return l > objm.x && l < 400;
}

[numthreads(16, 16, 1)] void Clear(uint3 id : SV_DispatchThreadID) {
    map[id.xy] = 0; ownMap[id.xy] = 0; ownMove[id.xy] = 0;
    if (band.w >= 0.5 && id.x == 0 && id.y == 0) stats.Store3((uint)band.z * 12, uint3(0, 0, 0));
}
groupshared uint gsFallback;

// A texel of the missing eye's own last image: where it is in the world now, and how far it moved since that image.
// The motion vector gives where the texel was in the image before (its previous camera). The same depth from that camera
// gives where it was in the world; for a still thing that is the same point, so only things that move get a movement.
float3 OwnMove(uint2 id, float2 uv, float3 w) {
    if (motion.w < 0.5) return 0;
    float2 prevUv = uv + heldMv[id] * motion.xy;
    float4 pPos = float4(pRight.w, pUp.w, pBack.w, 0);
    float3 ep = ToEye(w, pRight, pUp, pBack, pPos);
    if (ep.z >= -0.001) return 0;
    float3 was = ToWorld(EyeRay(prevUv, pTan) * -ep.z, pRight, pUp, pBack, pPos);
    float3 m = (w - was) * motion.z;
    return length(m) < 50 ? m : 0;
}

bool OwnTarget(uint2 id, out uint2 at, out uint key, out float3 move) {
    uint2 size = uint2(misc.y, misc.z);
    at = 0; key = 0; move = 0;
    if (id.x >= size.x || id.y >= size.y) return false;
    float d = max(heldDepth[id.xy], 0.000001);
    if (objm.w >= 0.5) {  // ff16vr: a texel of something that moved when this image was drawn: stale pose, leave it out
        float l = length(heldMv[id.xy] * objmv.zw * float2(size));
        if (l > objm.x && l < 400) return false;
    }
    float z = misc.x / d;
    float2 uv = (id.xy + 0.5) / size;
    float4 hPos = float4(hRight.w, hUp.w, hBack.w, 0);
    float3 w = ToWorld(EyeRay(uv, hTan) * z, hRight, hUp, hBack, hPos);
    move = OwnMove(id.xy, uv, w);
    float3 e = ToEye(w + move, dRight, dUp, dBack, dPos);
    if (e.z >= -0.001) return false;
    float2 t = ToUv(e, dTan);
    if (any(t < 0) || any(t >= 1)) return false;
    at = uint2(t * size);
    key = asuint(misc.x / -e.z);
    return true;
}

// The missing eye's own last depth, from the camera it was drawn with (moved on, with motion), into its camera for this tick.
[numthreads(16, 16, 1)] void SplatOwn(uint3 id : SV_DispatchThreadID) {
    if (band.w >= 0.5 && id.x < (uint)misc.y && id.y < (uint)misc.z) {
        // Lab stats: size of the held image's motion vectors in pixels, over texels that move more than 0.5 pixel.
        float l = length(heldMv[id.xy]);
        if (l > 0.5) {
            uint o;
            stats.InterlockedAdd((uint)band.z * 12 + 4, 1, o);
            stats.InterlockedAdd((uint)band.z * 12 + 8, (uint)(min(l, 4000.0) * 16), o);
        }
    }
    uint2 at; uint key; float3 move;
    if (!OwnTarget(id.xy, at, key, move)) return;
    uint old;
    InterlockedMax(ownMap[at], key, old);
}

// Second pass: the texel that won each place leaves its movement there, so the resolve can find where it came from.
[numthreads(16, 16, 1)] void SplatOwnMove(uint3 id : SV_DispatchThreadID) {
    uint2 at; uint key; float3 move;
    if (!OwnTarget(id.xy, at, key, move)) return;
    if (ownMap[at] == key) ownMove[at] = float4(move, 1);
}

[numthreads(16, 16, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
    uint2 size = uint2(misc.y, misc.z);
    if (id.x >= size.x || id.y >= size.y) return;
    float d = max(srcDepth[id.xy], 0.000001);
    float z = misc.x / d;
    float2 uv = (id.xy + 0.5) / size;
    float3 w = ToWorld(EyeRay(uv, sTan) * z, sRight, sUp, sBack, sPos);
    float3 e = ToEye(w, dRight, dUp, dBack, dPos);
    if (e.z >= -0.001) return;
    float2 t = ToUv(e, dTan);
    if (any(t < 0) || any(t >= 1)) return;
    uint2 at = uint2(t * size);
    uint old;
    InterlockedMax(map[at], asuint(misc.x / -e.z), old);
}

)"  // MSVC caps one string literal at 16 KB: the shader continues in the next literal.
R"(
// One depth guess for this output pixel, looked up in the own held image: how well the held depth agrees there (smaller is
// better, 1e9 when it does not), and the colour.
float TryOwn(uint key, int2 q, float3 ray, int2 size, out float4 color) {
    color = 0;
    if (key == 0) return 1e9;
    float4 hPos = float4(hRight.w, hUp.w, hBack.w, 0);
    float z = misc.x / asfloat(key);
    float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos) - ownMove[q].xyz;
    float3 e = ToEye(p, hRight, hUp, hBack, hPos);
    float2 s = ToUv(e, hTan);
    if (!(e.z < 0 && all(s >= 0) && all(s <= 1))) return 1e9;
    int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
    float seen = misc.x / max(heldDepth[ds], 0.000001);
    if (!(seen > -e.z * 0.93)) return 1e9;
    color = histColor.SampleLevel(lin, s, 0);
    return abs(seen + e.z) / -e.z;
}

float TryOther(uint key, float3 ray, int2 size, out float4 color) {
    color = 0;
    if (key == 0) return 1e9;
    float z = misc.x / asfloat(key);
    float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos);
    float3 e = ToEye(p, sRight, sUp, sBack, sPos);
    float2 s = ToUv(e, sTan);
    if (!(e.z < 0 && all(s >= 0) && all(s <= 1))) return 1e9;
    int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
    float seen = misc.x / max(srcDepth[ds], 0.000001);
    if (!(seen > -e.z * 0.93)) return 1e9;
    color = srcColor.SampleLevel(lin, s, 0);
    return abs(seen + e.z) / -e.z;
}

[numthreads(16, 16, 1)] void Resolve(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) gsFallback = 0;
    GroupMemoryBarrierWithGroupSync();
    uint w, h;
    outColor.GetDimensions(w, h);
    bool inside = id.x < w && id.y < h;
    uint fellBack = 0;
    if (inside) {
    float2 uv = (id.xy + 0.5) / float2(w, h);
    int2 size = int2(misc.y, misc.z);
    int2 at = clamp(int2(uv * size), int2(0, 0), size - 1);
    uint c = map[at];
    uint co = ownMap[at];
    int2 coAt = at;
    bool back = fill.x >= 0.5;
    if (c == 0 || co == 0) {
        // Cracks between projected texels. Wider gaps stay holes. Keys are near / depth, so a bigger key is nearer. 1.02 took
        // the nearest neighbour, which grows a moving foreground (Ratchet) by a texel at its outline; filling from the farthest
        // neighbour takes the background, which is what a gap at an outline is.
        bool fillOther = c == 0, fillOwn = co == 0;
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(at + int2(x, y), int2(0, 0), size - 1);
            uint m = map[q], o = ownMap[q];
            if (fillOther && m != 0 && (c == 0 || (back ? m < c : m > c))) c = m;
            if (fillOwn && o != 0 && (co == 0 || (back ? o < co : o > co))) {
                co = o;
                coAt = q;
            }
        }
    }
    float3 ray = EyeRay(uv, dTan);
    float3 worldDir = dRight.xyz * ray.x + dUp.xyz * ray.y + dBack.xyz * ray.z;
    // Both candidates, each checked against the depth of the image it comes from.
    float4 hPos = float4(hRight.w, hUp.w, hBack.w, 0);
    bool ownOk = false, otherOk = false;
    float4 ownColor = 0, otherColor = 0;
    int2 srcAt = int2(-1, -1);  // ff16vr: the drawn eye's texel this pixel comes from
    if (misc.w >= 0.5 && co != 0) {
        float z = misc.x / asfloat(co);
        float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos) - ownMove[coAt].xyz;
        float3 e = ToEye(p, hRight, hUp, hBack, hPos);
        float2 s = ToUv(e, hTan);
        if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
            int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
            float seen = misc.x / max(heldDepth[ds], 0.000001);
            if (seen > -e.z * 0.93) {
                ownColor = histColor.SampleLevel(lin, s, 0);
                ownOk = true;
            }
        }
    }
    if (c != 0 && (misc.w < 1.5 || misc.w >= 2.5)) {
        float z = misc.x / asfloat(c);
        float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos);
        float3 e = ToEye(p, sRight, sUp, sBack, sPos);
        float2 s = ToUv(e, sTan);
        if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
            // What the drawn eye holds there must be this surface, not something in front of it.
            int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
            float seen = misc.x / max(srcDepth[ds], 0.000001);
            if (seen > -e.z * 0.93) {
                otherColor = srcColor.SampleLevel(lin, s, 0);
                otherOk = true;
                srcAt = ds;
            }
        }
    }
    if (fill.w >= 0.5) {
        // Edges: the depth maps are at scene size, the output twice that. Instead of the nearest texel, try the four around
        // this pixel and keep the one whose surface the source image agrees with best, so outlines follow the source image
        // rather than steps of two output pixels.
        float2 f = uv * size - 0.5;
        int2 b = int2(floor(f));
        float bestOwn = 1e9, bestOther = 1e9;
        float4 colOwn = ownColor, colOther = otherColor;
        uint keyOwn = co, keyOther = c;
        int2 atOwn = coAt;
        for (int k = 0; k < 4; ++k) {
            int2 q = clamp(b + int2(k & 1, k >> 1), int2(0, 0), size - 1);
            float4 cc;
            if (misc.w >= 0.5) {
                uint o = ownMap[q];
                float m = TryOwn(o, q, ray, size, cc);
                if (m < bestOwn) { bestOwn = m; colOwn = cc; keyOwn = o; atOwn = q; }
            }
            if (misc.w < 1.5 || misc.w >= 2.5) {
                uint t = map[q];
                float m = TryOther(t, ray, size, cc);
                if (m < bestOther) { bestOther = m; colOther = cc; keyOther = t; }
            }
        }
        // 1: the chosen texels also feed the choice between the two images below. 2: colours only, that choice as before.
        bool keys = fill.w < 1.5;
        if (bestOwn < 1e8) { ownOk = true; ownColor = colOwn; if (keys) { co = keyOwn; coAt = atOwn; } }
        if (bestOther < 1e8) { otherOk = true; otherColor = colOther; if (keys) c = keyOther; }
    }
    // The drawn eye wins where it sees something clearly nearer than the own image (moved in front since), and in order 3
    // also clearly farther (what the own image shows there has moved away). Otherwise the own image, if it has the pixel.
    bool nearer = c != 0 && co != 0 && asfloat(c) > asfloat(co) * 1.1;
    bool farther = c != 0 && co != 0 && asfloat(c) * 1.1 < asfloat(co);
    // 1.03: things that move by themselves (Ratchet, enemies, traffic) come from the eye drawn this tick, which has their
    // pose and animation now, where both images see the same surface. The own image is a tick old for them. Still things,
    // lights included, stay with the own image.
    bool moving = false;
    if (fill.z > 0 && co != 0 && c != 0 && otherOk) {
        float zo = misc.x / asfloat(co);
        bool same = asfloat(c) < asfloat(co) * 1.1 && asfloat(c) * 1.1 > asfloat(co);
        moving = same && length(ownMove[coAt].xyz) > fill.z * zo;
    }
    // 1.06: a band around anything moving also comes from the drawn eye. The own image's pixels just outside a moving
    // outline are a soft blend of it and the background (DLSS smooths edges), so they would keep a faint old outline.
    if (!moving && band.x >= 1 && fill.z > 0 && otherOk && co != 0) {
        int r = (int)band.x;
        for (int y = -r; y <= r && !moving; ++y) for (int x = -r; x <= r && !moving; ++x) {
            int2 q = clamp(coAt + int2(x, y), int2(0, 0), size - 1);
            uint o = ownMap[q];
            if (o != 0 && length(ownMove[q].xyz) > fill.z * (misc.x / asfloat(o))) moving = true;
        }
    }
    // ff16vr (not in Rift Apart VR): in order 3 without motion vectors (FF16's do not describe camera motion), the band
    // comes from depth: within band.x texels, a texel where the two images clearly disagree (something moved there).
    if (!moving && band.x >= 1 && fill.z <= 0 && misc.w >= 2.5 && otherOk && co != 0) {
        int r = (int)band.x;
        for (int y = -r; y <= r && !moving; ++y) for (int x = -r; x <= r && !moving; ++x) {
            int2 q = clamp(coAt + int2(x, y), int2(0, 0), size - 1);
            uint o = ownMap[q], t = map[q];
            if (o != 0 && t != 0 && (asfloat(t) > asfloat(o) * 1.1 || asfloat(t) * 1.1 < asfloat(o))) moving = true;
        }
    }
    // ff16vr: moving things seen in the drawn eye (its motion vectors against its camera motion), and a band around them.
    if (!moving && objm.y >= 0.5 && otherOk && srcAt.x >= 0 && misc.w >= 0.5) {
        int r = (int)objm.z;
        for (int y = -r; y <= r && !moving; ++y) for (int x = -r; x <= r && !moving; ++x) {
            int2 q = clamp(srcAt + int2(x, y), int2(0, 0), size - 1);
            if (SrcMoving(q, size)) moving = true;
        }
    }
    bool preferOther = misc.w < 0.5 || (otherOk && (nearer || moving || (misc.w >= 2.5 && farther)));
    bool ok = true;
    float4 color;
    int src = 0;  // lab view: 1 drawn eye, 2 own image, 3 background stretch, 4 last resort
    if (preferOther && otherOk) { color = otherColor; src = 1; }
    else if (ownOk) { color = ownColor; src = 2; }
    else if (otherOk) { color = otherColor; src = 1; }
    else if (back && misc.w >= 0.5) {
        // Uncovered and seen by neither image (behind a moving Ratchet, say): stretch the nearest background of the own image
        // over it. Search rings for the farthest surface the own map has, then look it up as if it were at this pixel.
        uint bk = 0;
        int2 bAt = at;
        int r = (int)fill.y;
        for (int ring = 2; ring <= r && bk == 0; ring += 2) {
            for (int k = 0; k < 8; ++k) {
                int2 d = int2(k == 0 || k == 1 || k == 7 ? ring : k == 3 || k == 4 || k == 5 ? -ring : 0,
                              k == 1 || k == 2 || k == 3 ? ring : k == 5 || k == 6 || k == 7 ? -ring : 0);
                int2 q = clamp(at + d, int2(0, 0), size - 1);
                uint o = ownMap[q];
                if (o != 0 && (bk == 0 || o < bk)) {
                    bk = o;
                    bAt = q;
                }
            }
        }
        bool got = false;
        if (bk != 0) {
            float z = misc.x / asfloat(bk);
            float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos) - ownMove[bAt].xyz;
            float3 e = ToEye(p, hRight, hUp, hBack, hPos);
            float2 s = ToUv(e, hTan);
            if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
                // ff16vr: that spot of the own image may hold the character's old pose (what uncovered this pixel): then
                // take the drawn eye's background at the same world point, when it sees that far surface there.
                bool stale = false;
                if (objm.w >= 0.5) {
                    float l = length(heldMv[int2(s * size)] * objmv.zw * float2(size));
                    stale = l > objm.x && l < 400;
                }
                if (stale) {
                    float3 es = ToEye(p, sRight, sUp, sBack, sPos);
                    float2 ss = ToUv(es, sTan);
                    if (es.z < 0 && all(ss >= 0) && all(ss <= 1)) {
                        float dz = misc.x / max(srcDepth[int2(ss * size)], 0.000001);
                        if (dz > -es.z * 0.9) {  // the drawn eye sees the background (or farther) there
                            color = srcColor.SampleLevel(lin, ss, 0);
                            got = true;
                        }
                    }
                }
                if (!got) {
                    color = histColor.SampleLevel(lin, s, 0);
                    got = true;
                }
            }
        }
        if (!got) {
            ok = false;
            float3 v = float3(dot(worldDir, hRight.xyz), dot(worldDir, hUp.xyz), dot(worldDir, hBack.xyz));
            float2 s = v.z < 0 ? ToUv(v, hTan) : uv;
            color = histColor.SampleLevel(lin, saturate(s), 0);
        }
    }
    else {
        ok = false;
        float3 v = float3(dot(worldDir, hRight.xyz), dot(worldDir, hUp.xyz), dot(worldDir, hBack.xyz));
        float2 s = v.z < 0 ? ToUv(v, hTan) : uv;
        color = histColor.SampleLevel(lin, saturate(s), 0);
    }
    if (src == 0) src = ok ? 3 : 4;
    if (band.y >= 0.5 && band.y < 1.5) {
        // Lab: which source each pixel came from, over a dimmed picture. Red drawn eye, green own image (moving: yellow), blue
        // background stretch, magenta last resort.
        // Red: the own image had nothing valid here. Cyan: it had, but the drawn eye saw something nearer.
        float3 tint = src == 1 ? (moving ? float3(1, 1, 0) : ownOk ? float3(0, 1, 1) : float3(1, 0, 0)) : src == 2 ? float3(0, 1, 0) : src == 3 ? float3(0, 0, 1) : float3(1, 0, 1);
        color.rgb = color.rgb * 0.35 + tint * 0.65;
    }
    if (band.y >= 1.5) {
        // ff16vr lab (not in Rift Apart VR): 2 = the held image's motion vectors in output pixels (red |x| / 16, green |y| / 16,
        // blue: x < 0); 3 = its movement over distance per tick (/ 0.03, white = 0.03); 4 = the drawn eye's motion vectors.
        int2 q = clamp(int2(uv * size), int2(0, 0), size - 1);
        if (band.y < 2.5 || band.y >= 3.5) {
            float2 px = (band.y < 2.5 ? heldMv[q] : srcMv[q]) * motion.xy * float2(size);
            color.rgb = float3(min(abs(px.x) / 16, 1), min(abs(px.y) / 16, 1), px.x < 0 ? 0.5 : 0);
        } else {
            uint o = ownMap[q];
            float r = o != 0 ? length(ownMove[q].xyz) / (misc.x / asfloat(o)) : 0;
            color.rgb = min(r / 0.03, 1).xxx;
        }
    }
    outColor[id.xy] = float4(color.rgb, 1);
    fellBack = (src == 1 && !moving && !ownOk) || src == 4 ? 1 : 0;
    }
    if (band.w >= 0.5) {
        if (fellBack) InterlockedAdd(gsFallback, 1);
        GroupMemoryBarrierWithGroupSync();
        if (gi == 0 && gsFallback > 0) stats.InterlockedAdd((uint)band.z * 12, gsFallback);
    }
}
)";

