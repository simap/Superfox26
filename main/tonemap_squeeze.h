// The image's colour range onto the panel's, before the nearest entry search
// of the Oklab mapping. The tone table says what the panel shows, so an
// image colour beyond it would take the entry on the table's edge and every
// stronger colour the same one. Here a colour is brought inside first:
//   0 its chroma is cut to the part along the hue of sRGB red, the one hue
//     the panel has
//   1 TONEMAP_GAMMA on its lightness
//   2 its chroma keeps its share of the most chroma there is at its
//     lightness: the image's edge, sRGB black to red to white, maps to the
//     panel's edge, the upper hull of the entries over lightness
//   3 its lightness moves toward the lightness of the panel's strongest
//     red, by TONEMAP_COMPRESS of what the flat stretch of the image
//     triangle onto the panel's would move it
#pragma once

#include <math.h>

#include "tonemap.h"

#define SQUEEZE_STEPS 64

static float s_squeeze_img[SQUEEZE_STEPS + 1];   // most chroma at L = i / SQUEEZE_STEPS, image
static float s_squeeze_pan[SQUEEZE_STEPS + 1];   // and panel
static float s_squeeze_red_l, s_squeeze_red_c;   // sRGB red
static float s_squeeze_hue[2];                   // its hue, a unit vector in a and b
static float s_squeeze_cusp_l;                   // lightness of the panel's strongest red

static float squeeze_at(const float *edge, float L)
{
  float x = L * SQUEEZE_STEPS;
  if (x <= 0.0f) return edge[0];
  if (x >= (float)SQUEEZE_STEPS) return edge[SQUEEZE_STEPS];
  const int i = (int)x;
  return edge[i] + (x - (float)i) * (edge[i + 1] - edge[i]);
}

// oklab is the caller's sRGB to Oklab.
static void tonemap_squeeze_begin(void (*oklab)(uint8_t, uint8_t, uint8_t, float[3]))
{
  // The image's edge: (v, 0, 0) then (255, v, v), lightness rising all the
  // way, read at each step between the two points around it.
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

  // The panel's edge: at each step the highest line between two entries
  // that span it. Ink and paper are entries, at no chroma.
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

// t is the image colour in Oklab, changed in place.
static void tonemap_squeeze(float t[3])
{
  const float L = t[0] < 0.0f ? 0.0f : t[0];
  const float L1 = TONEMAP_GAMMA == 1.0f ? L : powf(L, 1.0f / TONEMAP_GAMMA);
  // The panel has one hue. A colour keeps the part of its chroma that lies
  // along sRGB red's hue, and none when it lies the other way: an orange
  // draws as a duller red, a blue or a green as the grey of its lightness.
  float c = t[1] * s_squeeze_hue[0] + t[2] * s_squeeze_hue[1];
  if (c < 0.0f) c = 0.0f;
  t[0] = L1;
  t[1] = c * s_squeeze_hue[0];
  t[2] = c * s_squeeze_hue[1];
  if (c < 1e-6f || TONEMAP_COMPRESS < 0.0f) return;
  const float edge = squeeze_at(s_squeeze_img, L);
  float share = edge > 1e-6f ? c / edge : 1.0f;
  if (share > 1.0f) share = 1.0f;
  float wr = share * squeeze_at(s_squeeze_img, L1) / s_squeeze_red_c;
  if (wr > 1.0f) wr = 1.0f;
  float rest = L1 - wr * s_squeeze_red_l;
  if (rest < 0.0f) rest = 0.0f;
  const float flat = rest + wr * s_squeeze_cusp_l;
  const float L2 = L1 + TONEMAP_COMPRESS * (flat - L1);
  const float c2 = share * squeeze_at(s_squeeze_pan, L2);
  t[0] = L2;
  t[1] *= c2 / c;
  t[2] *= c2 / c;
}
