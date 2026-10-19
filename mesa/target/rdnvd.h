/*
 * rdnvd: the card's video decoder (UVD) for programs. Plain C, no Mesa or OpenGL types.
 *
 * A session takes H.264 NAL units (Annex B, or length prefixed as in an MP4/QuickTime sample), finds the
 * pictures, decodes them on the card and hands them back in display order, each with the time stamp of
 * the data it came from. The pictures stay in video memory; rdnvd_frame_read() copies one into the
 * caller's memory (the card copies it into system memory first: the CPU reads video memory at under
 * 10 MB/s). Not thread safe: one thread per session.
 *
 *   struct rdnvd *d = rdnvd_open();                       NULL: no card, no kext, or no firmware in it
 *   struct rdnvd_session *s = rdnvd_h264_open(d, avcc, n, output, user);
 *   rdnvd_h264_feed(s, sample, size, pts, nal_length_size, 1);     (a QuickTime sample: one access unit)
 *   ... output(user, frame) is called, in display order, inside feed and flush
 *   rdnvd_h264_flush(s, 0);  rdnvd_h264_close(s);  rdnvd_close(d);
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef RDNVD_H
#define RDNVD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The results are the RDNVD_H264_* of rdnvd_h264.h: 0 is success, UNSUPPORTED means "decode it in software". */
#define RDNVD_OK		0
#define RDNVD_UNSUPPORTED	(-1)
#define RDNVD_BAD_STREAM	(-2)
#define RDNVD_NO_MEMORY		(-3)
#define RDNVD_FAILED		(-4)

struct rdnvd;
struct rdnvd_session;

/* A decoded picture. Valid until rdnvd_frame_release() (the session may be closed first). */
struct rdnvd_frame {
	uint64_t pts;
	/* The picture shown: after the cropping of the stream. */
	unsigned width, height;
	/* What the decoder produced. */
	unsigned coded_width, coded_height;
	unsigned crop_left, crop_top;
	void *priv;
};

typedef void (*rdnvd_output_fn)(void *user, struct rdnvd_frame *frame);

struct rdnvd *rdnvd_open(void);
void rdnvd_close(struct rdnvd *d);

/*
 * `avcc`: the configuration record of an MP4/QuickTime 'avcC' box (its SPS and PPS), or NULL for a stream that
 * carries them. Returns the session; *nal_length_size gets the length-prefix size to pass to feed (0 for Annex B).
 * Returns NULL if the stream is one the card does not decode (with an avcC record that is known here: other
 * than 4:2:0 at 8 bits, interlaced coding, more than 8704 macroblocks): decode it in software.
 */
struct rdnvd_session *rdnvd_h264_open(struct rdnvd *d, const uint8_t *avcc, size_t avcc_size,
				      rdnvd_output_fn output, void *user, unsigned *nal_length_size);
/*
 * Data: Annex B (nal_length_size 0) or length prefixed. `end_of_au`: the data ends an access unit, so the
 * picture in it is decoded now (a QuickTime sample, or what a VLC packetizer hands over). `pts` goes with the
 * pictures that start in this data.
 */
int rdnvd_h264_feed(struct rdnvd_session *s, const uint8_t *data, size_t size, uint64_t pts,
		    unsigned nal_length_size, int end_of_au);
/*
 * Decode one access unit (a QuickTime sample) and give back the picture it holds, as soon as it is decoded
 * and in decoding order, not display order: for a consumer that shows pictures by time stamp itself
 * (QuickTime draws them in display order, later). *frame is NULL if the data held no picture (parameter
 * sets only, or not a point to start from); release it with rdnvd_frame_release(). The `output` callback of
 * rdnvd_h264_open may be NULL when this is used.
 */
int rdnvd_h264_decode_au(struct rdnvd_session *s, const uint8_t *data, size_t size, unsigned nal_length_size,
			 struct rdnvd_frame **frame);
/*
 * How many pictures a picture can be decoded before it is displayed: the decoder's picture buffer, or
 * what the stream says it reorders. For a consumer that has to say how far ahead it decodes.
 */
unsigned rdnvd_h264_reorder_delay(struct rdnvd_session *s);
/* The end of the stream or a seek: decode what is pending and output everything (drop: forget it instead). */
int rdnvd_h264_flush(struct rdnvd_session *s, int drop);
void rdnvd_h264_close(struct rdnvd_session *s);

/*
 * The picture's samples as NV12 (a plane of luma, then one of interleaved U and V at half the size in
 * both directions) cropped to width x height, into the caller's planes.
 */
int rdnvd_frame_read(struct rdnvd_frame *frame, uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride);
/*
 * The same picture as packed 4:2:2 ('2vuy': U, Y, V, Y for every two pixels; chroma rows repeated), into a
 * buffer of rows `stride` bytes apart, cropped to width x height.
 */
int rdnvd_frame_read_2vuy(struct rdnvd_frame *frame, uint8_t *dst, size_t stride);
void rdnvd_frame_release(struct rdnvd_frame *frame);

#ifdef __cplusplus
}
#endif

#endif /* RDNVD_H */
