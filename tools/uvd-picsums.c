/*
 * uvd-picsums: the checksum of every picture of a yuv420p file as the QuickTime component's DrawBand log gives it
 * ('2vuy' rows: U, Y, V, Y for every two pixels, each chroma row used for the two luma rows it serves; 32 bit FNV-1a
 * over the rows), so that a log of a real playback can be checked picture by picture (scripts/uvd-picsums.py).
 *
 *   uvd-picsums reference.yuv width height      one line per picture: number, sum (hex)
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	unsigned w, h, n = 0;
	size_t frame_bytes;
	uint8_t *buf;
	FILE *f;

	if (argc != 4)
		return 2;
	w = atoi(argv[2]);
	h = atoi(argv[3]);
	frame_bytes = (size_t)w * h * 3 / 2;
	f = fopen(argv[1], "rb");
	buf = malloc(frame_bytes);
	if (!f || !buf)
		return 1;
	while (fread(buf, 1, frame_bytes, f) == frame_bytes) {
		const uint8_t *y = buf, *u = y + (size_t)w * h, *v = u + (size_t)w * h / 4;
		uint32_t sum = 2166136261u;
		unsigned row, x;

		for (row = 0; row < h; row++)
			for (x = 0; x < w / 2; x++) {
				sum = (sum ^ u[(row / 2) * (w / 2) + x]) * 16777619u;
				sum = (sum ^ y[row * w + 2 * x]) * 16777619u;
				sum = (sum ^ v[(row / 2) * (w / 2) + x]) * 16777619u;
				sum = (sum ^ y[row * w + 2 * x + 1]) * 16777619u;
			}
		printf("%u %08x\n", n++, sum);
	}
	return 0;
}
