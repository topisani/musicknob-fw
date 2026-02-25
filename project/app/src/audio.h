#pragma once

#include "sdcard.h"

int audio_init(void);
int audio_set_file(struct sdcard_audio_info *info);
void audio_set_volume(uint16_t volume_q8);
