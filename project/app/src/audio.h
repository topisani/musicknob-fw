#pragma once

#include "sdcard.h"

int audio_init(void);
int audio_set_file(const struct sdcard_wav_info *info);
