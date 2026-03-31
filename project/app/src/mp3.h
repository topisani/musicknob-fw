#pragma once

#include "sdcard.h"
#include <stdint.h>

/* PCM decode buffer: one MPEG1 stereo frame = 1152 samples x 2 channels */
#define MP3_PCM_BUF_SAMPLES (2304)

/* Parse MP3 header from an open file, populating info fields.
 * Called by sdcard.c during enumeration. */
int mp3_parse_header(struct sdcard_audio_info *info);

/* Initialize the MP3 decoder. Call once at startup. */
int mp3_decoder_init(void);

/* Switch to decoding a new file, seeking to match global_sample_counter. */
int mp3_switch_file(struct sdcard_audio_info *info, uint64_t global_sample_counter);

/* Decode one frame into pcm_buf. Returns number of int16_t samples produced. */
int16_t mp3_decode_frame(int16_t *pcm_buf);
