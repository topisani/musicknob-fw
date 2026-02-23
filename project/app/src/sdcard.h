#pragma once

#include <zephyr/fs/fs.h>

struct sdcard_audio_info {
	char     path[64];
	uint32_t samplerate;
	uint16_t channels;
	uint32_t total_samples;   /* total PCM samples (for modulo seeking) */
	off_t    data_start;      /* byte offset of first MP3 frame (after ID3) */
	uint32_t frame_bytes_num; /* 144 * bitrate_bps — numerator for rational seek arithmetic */
};

int sdcard_init(void);
int sdcard_get_audio_count(void);
const struct sdcard_audio_info *sdcard_get_audio_info(int index);
