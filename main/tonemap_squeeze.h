// The panel renders only black, white and one red chromatic axis, so a full colour source must be
// reduced to that axis before it can show.
// Call tonemap_squeeze_begin once before tonemap_squeeze: it finds the red hue and cusp and fills
// the chroma edges that tonemap_squeeze reads.
#pragma once

#include <math.h>

#include "tonemap.h"

#define SQUEEZE_STEPS 64

static float s_squeeze_img[SQUEEZE_STEPS + 1];
// The panel edge, the upper hull of the entries over L as its corners, lightest last. A table in
// steps of L cuts every corner that falls between two steps.
static float s_squeeze_hull_l[TONEMAP_COUNT], s_squeeze_hull_c[TONEMAP_COUNT];
static int s_squeeze_hull_n;
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

// The most chroma the panel has at L, from the hull.
static float squeeze_pan_at(float L)
{
  if (L <= s_squeeze_hull_l[0]) return s_squeeze_hull_c[0];
  for (int i = 1; i < s_squeeze_hull_n; i++) {
    if (L <= s_squeeze_hull_l[i]) {
      const float l0 = s_squeeze_hull_l[i - 1], c0 = s_squeeze_hull_c[i - 1];
      return c0 + (L - l0) * (s_squeeze_hull_c[i] - c0) / (s_squeeze_hull_l[i] - l0);
    }
  }
  return s_squeeze_hull_c[s_squeeze_hull_n - 1];
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
  // The hull from the darkest entry, the one with the most chroma among equals. From each corner
  // the next is the lighter entry on the steepest line, the farthest of equals.
  unsigned cur = 0;
  for (unsigned e = 1; e < TONEMAP_COUNT; e++) {
    const float dl = k_tonemap[e].L - k_tonemap[cur].L;
    if (dl < 0.0f || (dl == 0.0f && ec[e] > ec[cur])) cur = e;
  }
  s_squeeze_hull_n = 0;
  for (;;) {
    s_squeeze_hull_l[s_squeeze_hull_n] = k_tonemap[cur].L;
    s_squeeze_hull_c[s_squeeze_hull_n] = ec[cur];
    s_squeeze_hull_n++;
    int next = -1;
    float best = 0.0f;
    for (unsigned q = 0; q < TONEMAP_COUNT; q++) {
      const float dl = k_tonemap[q].L - k_tonemap[cur].L;
      if (dl <= 0.0f) continue;
      const float m = (ec[q] - ec[cur]) / dl;
      if (next < 0 || m > best || (m == best && k_tonemap[q].L > k_tonemap[next].L)) {
        next = (int)q;
        best = m;
      }
    }
    if (next < 0) break;
    cur = (unsigned)next;
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
  const float c2 = share * squeeze_pan_at(L2);
  t[1] *= c2 / c;
  t[2] *= c2 / c;
}
