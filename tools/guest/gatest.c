/* Pixel-check the production GA routines; restore the test area on success. */
#include <stdio.h>
#include "../../ga/RadeonNIGA.c"

#define W 128
#define H 96
static UInt32 expected[H][W], saved[H][W];

static int check(struct ga *ga, int x, int y)
{
	int row;
	if (ga_flush(ga, 0)) return 0;
	for (row = 0; row < H; row++)
		if (memcmp(ga_pixel(ga, x, y + row), expected[row], W * 4)) {
			fprintf(stderr, "pixel mismatch, row %d\n", row);
			return 0;
		}
	return 1;
}

int main(void)
{
	struct ga ga;
	int x, y, i, row, col, ok = 1;
	static const int copies[][6] = {
		{0,0,7,9,90,60}, {7,9,0,0,90,60},
		{0,0,9,0,90,60}, {9,0,0,0,90,60},
		{0,0,0,7,90,60}, {0,7,0,0,90,60},
	};
	memset(&ga, 0, sizeof(ga));
	if (ga_start(&ga, NULL, 0) || !ga.gpu) return 1;
	x = ga.info.fb_width - W; y = ga.info.fb_height - H;
	if (x < 0 || y < 0) return 1;
	for (row = 0; row < H; row++)
		memcpy(saved[row], ga_pixel(&ga, x, y + row), W * 4);
	for (row = 0; row < H; row++) {
		UInt32 color = 0xff000000u | (row * 0x020305u);
		for (col = 0; col < W; col++) expected[row][col] = color;
		if (ga_fill(&ga, x, y + row, W, 1, color)) return 1;
	}
	ok = check(&ga, x, y);
	for (i = 0; ok && i < sizeof(copies)/sizeof(copies[0]); i++) {
		const int *r = copies[i];
		UInt32 temp[H][W];
		for (row = 0; row < r[5]; row++)
			memcpy(temp[row], &expected[r[1]+row][r[0]], r[4]*4);
		for (row = 0; row < r[5]; row++)
			memcpy(&expected[r[3]+row][r[2]], temp[row], r[4]*4);
		ok = !ga_copy(&ga,x+r[0],y+r[1],x+r[2],y+r[3],r[4],r[5]);
		if (ok) ok = check(&ga, x, y);
	}
	/* Clip a fill against the lower-right scanout edge. */
	if (ok) {
		for (row = H-4; row < H; row++)
			for (col = W-5; col < W; col++) expected[row][col]=0xff317be2;
		ok = !ga_fill(&ga,x+W-5,y+H-4,20,20,0xff317be2) && check(&ga,x,y);
	}
	if (!ga_flush(&ga, 0))
		for (row = 0; row < H; row++)
			memcpy(ga_pixel(&ga, x, y + row), saved[row], W * 4);
	printf("GA GPU fill, overlap copies and clipping: %s\n", ok ? "PASS" : "FAIL");
	ga_stop(&ga);
	return ok ? 0 : 1;
}
