#pragma once

#include <stdint.h>

/* Turns an RGB565 map image into pure black and white for e-paper, which
 * would otherwise dither the map's pale colours into speckles: dark pixels
 * (text, railways) and the edges between areas (road casings, buildings,
 * parks, shores) turn black, water gets a sparse dot pattern, the rest white. */
void ink__convert(uint16_t *pixels, int width, int height);
