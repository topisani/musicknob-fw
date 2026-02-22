#pragma once

#include <zephyr/fs/fs.h>

struct sdcard_wav_info {
	char     path[64];
	uint32_t samplerate;
	uint16_t channels;
	uint16_t bitdepth;
	uint16_t framesize;   /* bytes per frame = channels * bitdepth/8 */
	uint32_t nframes;
	uint32_t data_size;
	off_t    data_start;  /* byte offset of first PCM sample in file */
};

int sdcard_init(void);
int sdcard_get_wav_count(void);
const struct sdcard_wav_info *sdcard_get_wav_info(int index);
