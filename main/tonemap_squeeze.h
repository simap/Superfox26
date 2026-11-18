// The panel renders only black, white and one red chromatic axis, so a full colour source must be
// reduced to that axis before it can show.
// Call tonemap_squeeze_begin once before tonemap_squeeze: it finds the red hue and cusp and fills
// the chroma tables that tonemap_squeeze reads.
#pragma once

#include <math.h>

#include "tonemap.h"

#define SQUEEZE_STEPS 64

static float s_squeeze_img[SQUEEZE_STEPS + 1];
static float s_squeeze_pan[SQUEEZE_STEPS + 1];
static float s_squeeze_red_l, s_squeeze_red_c;
static float s_squeeze_hue[2];
static float s_squeeze_cusp_l;

static float squeeze_at(const float *edge, float L)
{
  float x = L * SQUEEZE_STEPS;
  if (x <= 0.0f) return edge[0];
  if (x >= (float)SQUEEZE_STEPS) return edge[SQUEEZE_STEPS];
  const int i = (int)x;
  return edge[i] + (x - (float)i) * (edge[i + 1] - edge[i]);
}

static void tonemap_squeeze_begin(void (*oklab)(uint8_t, uint8_t, uint8_t, float[3]))
{
  float l0 = 0.0f, c0 = 0.0f;
  int next = 0;
  s_squeeze_red_c = 0.0f;
  for (int k = 0; k < 512 && next <= SQUEEZE_STEPS; k++) {
    float t[3];
    if (k < 256) {
      oklab((uint8_t)k, 0, 0, t);
    } else {
      oklab(255, (uint8_t)(k - 256), (uint8_t)(k - 256), t);
    }
    const float c = sqrtf(t[1] * t[1] + t[2] * t[2]);
    if (c > s_squeeze_red_c) {
      s_squeeze_red_c = c;
      s_squeeze_red_l = t[0];
      s_squeeze_hue[0] = t[1] / c;
      s_squeeze_hue[1] = t[2] / c;
    }
    while (next <= SQUEEZE_STEPS && (float)next / SQUEEZE_STEPS <= t[0]) {
      const float L = (float)next / SQUEEZE_STEPS;
      s_squeeze_img[next++] = t[0] > l0 ? c0 + (L - l0) * (c - c0) / (t[0] - l0) : c;
    }
    l0 = t[0];
    c0 = c;
  }
  while (next <= SQUEEZE_STEPS) s_squeeze_img[next++] = 0.0f;

  static float ec[TONEMAP_COUNT];
  float best_c = -1.0f;
  for (unsigned e = 0; e < TONEMAP_COUNT; e++) {
    ec[e] = sqrtf(k_tonemap[e].a * k_tonemap[e].a + k_tonemap[e].b * k_tonemap[e].b);
    if (ec[e] > best_c) {
      best_c = ec[e];
      s_squeeze_cusp_l = k_tonemap[e].L;
    }
  }
  for (int i = 0; i <= SQUEEZE_STEPS; i++) {
    const float L = (float)i / SQUEEZE_STEPS;
    float top = 0.0f;
    for (unsigned p = 0; p < TONEMAP_COUNT; p++) {
      const float lp = k_tonemap[p].L;
      if (lp > L) continue;
      for (unsigned q = 0; q < TONEMAP_COUNT; q++) {
        const float lq = k_tonemap[q].L;
        if (lq < L) continue;
        const float c = lq > lp ? ec[p] + (L - lp) * (ec[q] - ec[p]) / (lq - lp) : ec[p];
        if (c > top) top = c;
      }
    }
    s_squeeze_pan[i] = top;
  }
}

static void tonemap_squeeze(float t[3])
{
  const float L = t[0] < 0.0f ? 0.0f : t[0];
  const float L1 = TONEMAP_GAMMA == 1.0f ? L : powf(L, 1.0f / TONEMAP_GAMMA);
  float c = t[1] * s_squeeze_hue[0] + t[2] * s_squeeze_hue[1];
  if (c < 0.0f) c = 0.0f;
  t[0] = L1;
  t[1] = c * s_squeeze_hue[0];
  t[2] = c * s_squeeze_hue[1];
  if (TONEMAP_COMPRESS < 0.0f) return;
  float wr = squeeze_at(s_squeeze_img, L1) / s_squeeze_red_c;
  if (wr > 1.0f) wr = 1.0f;
  float rest = L1 - wr * s_squeeze_red_l;
  if (rest < 0.0f) rest = 0.0f;
  const float flat = rest + wr * s_squeeze_cusp_l;
  const float L2 = L1 + TONEMAP_COMPRESS * (flat - L1);
  t[0] = L2;
  if (c < 1e-6f) return;
  const float edge = squeeze_at(s_squeeze_img, L);
  float share = edge > 1e-6f ? c / edge : 1.0f;
  if (share > 1.0f) share = 1.0f;
  const float c2 = share * squeeze_at(s_squeeze_pan, L2);
  t[1] *= c2 / c;
  t[2] *= c2 / c;
}
