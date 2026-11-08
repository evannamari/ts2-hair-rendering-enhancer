// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 evannamari
// Developed with the assistance of Claude (Anthropic).
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version. It is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.

#pragma once
// TS2HairRenderingEnhancer.h -- platform independent core of the hair triangle sorter (optional feature of TS2 Hair Rendering Enhancer).
// No D3D, no Windows: it only needs raw vertex / index bytes and the VS constants,
// so it can be unit-tested on any machine (see test_core.cpp).

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

namespace hs {

static const int kMaxBones = 60;      // c56..c235 = 180 registers = 60 bones * 3 registers

struct Bone { float r1[3], r2[3], r3[3], t[3]; };

// palette = the 180 float4 registers starting at c56, as returned by GetVertexShaderConstantF(56, buf, 180).
// Per bone: reg0 = row1.xyz, reg1 = row2.xyz, reg2 = translation.xyz (.w holds morph weights, ignored).
// The VS builds row3 = cross(row1, row2) per bone (see CreateCompositeSkinningMatrixFourWayPacked).
inline void BuildBones(const float* palette, Bone* out)
{
    for (int b = 0; b < kMaxBones; ++b) {
        const float* p = palette + b * 12;                       // 3 registers * 4 floats
        Bone& o = out[b];
        for (int k = 0; k < 3; ++k) { o.r1[k] = p[k]; o.r2[k] = p[4 + k]; o.t[k] = p[8 + k]; }
        o.r3[0] = o.r1[1] * o.r2[2] - o.r1[2] * o.r2[1];
        o.r3[1] = o.r1[2] * o.r2[0] - o.r1[0] * o.r2[2];
        o.r3[2] = o.r1[0] * o.r2[1] - o.r1[1] * o.r2[0];
    }
}

// Depth key per vertex = clip-space w = dot(c3.xyz, cameraSpacePos) + c3.w, where c3 is the 4th row of cameraToClip (c0..c3).
// Bigger key = farther from the camera.
//   posPtr / wPtr / iPtr : address of the FIRST vertex's position / blend weights (float3) / blend indices (ubyte4)
//   skinStride           : stride of the stream holding weights+indices
// Weights: w0,w1,w2 stored, w3 = 1 - w0 - w1 - w2 (implicit, as in the VS). Slots with ~0 weight are ignored.
inline bool ComputeDepthKeys(const uint8_t* posPtr, uint32_t posStride,
                             const uint8_t* wPtr, const uint8_t* iPtr, uint32_t skinStride,
                             uint32_t nVerts, const float* palette, const float c3[4], float* keys)
{
    Bone bones[kMaxBones];
    BuildBones(palette, bones);

    for (uint32_t i = 0; i < nVerts; ++i) {
        float p[3], w[4];
        memcpy(p, posPtr + (size_t)i * posStride, 12);
        memcpy(w, wPtr + (size_t)i * skinStride, 12);
        w[3] = 1.0f - w[0] - w[1] - w[2];
        const uint8_t* ix = iPtr + (size_t)i * skinStride;

        float cam[3] = { 0, 0, 0 };
        for (int k = 0; k < 4; ++k) {
            if (fabsf(w[k]) < 1e-6f) continue;
            const unsigned b = ix[k];
            if (b >= (unsigned)kMaxBones) return false;
            const Bone& B = bones[b];
            cam[0] += w[k] * (B.r1[0] * p[0] + B.r1[1] * p[1] + B.r1[2] * p[2] + B.t[0]);
            cam[1] += w[k] * (B.r2[0] * p[0] + B.r2[1] * p[1] + B.r2[2] * p[2] + B.t[1]);
            cam[2] += w[k] * (B.r3[0] * p[0] + B.r3[1] * p[1] + B.r3[2] * p[2] + B.t[2]);
        }
        const float key = c3[0] * cam[0] + c3[1] * cam[1] + c3[2] * cam[2] + c3[3];
        if (!(key == key) || fabsf(key) > 1e9f) return false;     // NaN / inf guard
        keys[i] = key;
    }
    return true;
}

struct Scratch { std::vector<float> triKey; std::vector<uint32_t> order; };

// Stable far-to-near sort of a TRIANGLE LIST (16-bit indices, relative to BaseVertexIndex).
// Triangle key = mean of its three vertex keys. Ties keep the author's order (stable).
inline bool SortTriangles(const uint16_t* ib, uint32_t prims, const float* keys, uint32_t nVerts,
                          uint16_t* out, Scratch& s)
{
    s.triKey.resize(prims);
    s.order.resize(prims);
    for (uint32_t t = 0; t < prims; ++t) {
        const uint32_t a = ib[t * 3], b = ib[t * 3 + 1], c = ib[t * 3 + 2];
        if (a >= nVerts || b >= nVerts || c >= nVerts) return false;
        s.triKey[t] = (keys[a] + keys[b] + keys[c]) * (1.0f / 3.0f);
        s.order[t] = t;
    }
    const float* tk = s.triKey.data();
    std::stable_sort(s.order.begin(), s.order.end(), [tk](uint32_t x, uint32_t y) { return tk[x] > tk[y]; });
    for (uint32_t i = 0; i < prims; ++i) {
        const uint32_t t = s.order[i];
        out[i * 3] = ib[t * 3]; out[i * 3 + 1] = ib[t * 3 + 1]; out[i * 3 + 2] = ib[t * 3 + 2];
    }
    return true;
}

} // namespace hs
