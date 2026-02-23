#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/fs/fs.h>
#include <stdlib.h>
#include <string.h>

#include <mp3dec.h>
#include <mp3common.h>

#include "audio.h"

LOG_MODULE_REGISTER(audio, CONFIG_APP_LOG_LEVEL);

#define BLOCK_SIZE	(4096)
#define BLOCK_COUNT	4
#define STACK_SIZE	(4096 * 2)
#define THREAD_PRIO	5
#define PREQUEUE_BLOCKS 4

/* MP3 input buffer: large enough to hold >1 full frame (max ~1441 bytes) */
#define MP3_BUF_SIZE	8192
/* PCM output buffer: one MPEG1 stereo frame = 1152 samples × 2 channels */
#define PCM_BUF_SAMPLES	(1024 * 5) // 2304

K_MEM_SLAB_DEFINE(i2s_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s));

/* Volume: written from main thread, read from audio thread */
static volatile uint16_t current_volume_q8 = 256; /* default full volume */

/* Inter-thread communication */
static const struct sdcard_audio_info *volatile pending_info;
static struct k_sem file_change_sem;

/* Thread state */
static struct fs_file_t cur_file;
static bool file_open;
static bool i2s_started;
static bool i2s_configured;

/* MP3 metadata for current file */
static off_t    mp3_data_start;
static uint32_t mp3_total_samples;
static uint32_t mp3_frame_bytes_num;
static uint16_t mp3_channels;
static uint32_t mp3_samplerate;

/* MP3 decode state */
static HMP3Decoder mp3_dec;
static uint8_t  mp3_buf[MP3_BUF_SIZE];
static int      mp3_buf_len;
static int16_t  pcm_buf[PCM_BUF_SAMPLES];
static int      pcm_buf_pos;
static int      pcm_buf_avail;

/* Synchronized playback position counter (in PCM samples) */
static uint64_t global_sample_counter;

static int configure_i2s(uint32_t samplerate, uint16_t channels)
{
	struct i2s_config cfg = {
		.word_size = 32,
		.channels = channels,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = samplerate,
		.mem_slab = &i2s_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};

	return i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
}

/* Refill mp3_buf from file up to MP3_BUF_SIZE bytes */
static int mp3_buf_fill(void)
{
	int space = MP3_BUF_SIZE - mp3_buf_len;
	if (space <= 0) {
		return 0;
	}

	int rc = fs_read(&cur_file, mp3_buf + mp3_buf_len, space);
	if (rc < 0) {
		LOG_WRN("SD read error: %d", rc);
		return rc;
	}
	if (rc == 0) {
		/* EOF — discard buffered data and reset decoder to clear the bit
		 * reservoir before looping. Mixing end-of-file and start-of-file
		 * data corrupts joint-stereo (MS) decoding independently in L/R. */
		MP3FreeDecoder(mp3_dec);
		mp3_dec = MP3InitDecoder();
		mp3_buf_len = 0;
		fs_seek(&cur_file, mp3_data_start, FS_SEEK_SET);
		rc = fs_read(&cur_file, mp3_buf, MP3_BUF_SIZE);
		if (rc < 0) {
			LOG_WRN("SD read error after loop: %d", rc);
			return rc;
		}
		LOG_DBG("EOF loop: decoder reset");
	}
	mp3_buf_len += rc;
	return 0;
}

/* Decode one MP3 frame into pcm_buf; handles sync search and underflow */
static void fill_pcm_buf(void)
{
	int err;
	int skipped_bytes = 0;
	int maindata_underflows = 0;
	int indata_underflows = 0;
	int other_errors = 0;

	uint32_t t0_us = k_ticks_to_us_near32(k_uptime_ticks());

	for (int attempts = 0; attempts < 8; attempts++) {
		/* Top up compressed buffer */
		mp3_buf_fill();

		/* Find next sync word */
		int offset = MP3FindSyncWord(mp3_buf, mp3_buf_len);
		if (offset < 0) {
			/* No sync found — discard buffer and retry */
			skipped_bytes += mp3_buf_len;
			mp3_buf_len = 0;
			continue;
		}
		if (offset > 0) {
			skipped_bytes += offset;
			memmove(mp3_buf, mp3_buf + offset, mp3_buf_len - offset);
			mp3_buf_len -= offset;
		}

		/* Decode one frame */
		unsigned char *read_ptr = mp3_buf;
		int bytes_left = mp3_buf_len;

		err = MP3Decode(mp3_dec, &read_ptr, &bytes_left, pcm_buf, 0);

		/* Update buffer: consume bytes that the decoder advanced past */
		int consumed = mp3_buf_len - bytes_left;
		if (consumed > 0) {
			memmove(mp3_buf, mp3_buf + consumed, bytes_left);
			mp3_buf_len = bytes_left;
		}

		if (err == ERR_MP3_NONE) {
			MP3FrameInfo fi;
			MP3GetLastFrameInfo(mp3_dec, &fi);
			pcm_buf_pos   = 0;
			pcm_buf_avail = fi.outputSamps;

			uint32_t elapsed_us = k_ticks_to_us_near32(k_uptime_ticks()) - t0_us;
			if (skipped_bytes || maindata_underflows || indata_underflows ||
			    other_errors || elapsed_us > 10000) {
				LOG_WRN("fill_pcm_buf: %u us, skipped=%d maindata_uflow=%d "
					"indata_uflow=%d other_err=%d samps=%d",
					elapsed_us, skipped_bytes, maindata_underflows,
					indata_underflows, other_errors, fi.outputSamps);
			}
			return;
		} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
			maindata_underflows++;
			continue;
		} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
			indata_underflows++;
			continue;
		} else {
			other_errors++;
			/* Sync error or bad frame: skip 1 byte and resync */
			if (mp3_buf_len > 0) {
				memmove(mp3_buf, mp3_buf + 1, mp3_buf_len - 1);
				mp3_buf_len--;
			}
		}
	}

	/* Fallback: output silence for this iteration */
	uint32_t elapsed_us = k_ticks_to_us_near32(k_uptime_ticks()) - t0_us;
	LOG_WRN("fill_pcm_buf: decode failed after 8 attempts in %u us, "
		"buf_len=%d skipped=%d maindata_uflow=%d indata_uflow=%d other_err=%d",
		elapsed_us, mp3_buf_len, skipped_bytes,
		maindata_underflows, indata_underflows, other_errors);
	memset(pcm_buf, 0, sizeof(pcm_buf));
	pcm_buf_pos   = 0;
	pcm_buf_avail = PCM_BUF_SAMPLES;
}

static int open_mp3(const struct sdcard_audio_info *info)
{
	if (file_open) {
		fs_close(&cur_file);
		file_open = false;
	}

	fs_file_t_init(&cur_file);
	int rc = fs_open(&cur_file, info->path, FS_O_READ);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", info->path, rc);
		return rc;
	}
	file_open = true;

	mp3_data_start      = info->data_start;
	mp3_total_samples   = info->total_samples;
	mp3_frame_bytes_num = info->frame_bytes_num;
	mp3_channels        = info->channels;
	mp3_samplerate      = info->samplerate;

	LOG_INF("Opened %s: %u Hz, %u ch, ~%u kbps",
		info->path, info->samplerate, info->channels,
		info->frame_bytes_num / (144 * 1000));

	if (!i2s_configured) {
		rc = configure_i2s(info->samplerate, info->channels);
		if (rc < 0) {
			LOG_ERR("I2S configure failed: %d", rc);
			return rc;
		}
		i2s_configured = true;
	}

	/* Seek to position matching global_sample_counter */
	if (mp3_total_samples > 0) {
		uint32_t seek_sample = (uint32_t)(global_sample_counter % mp3_total_samples);
		uint32_t seek_frame  = seek_sample / 1152;
		/* Rounded rational arithmetic: avoids floor-division accumulation error */
		off_t byte_offset = mp3_data_start +
			(off_t)(((uint64_t)seek_frame * mp3_frame_bytes_num + mp3_samplerate / 2)
				/ mp3_samplerate);
		fs_seek(&cur_file, byte_offset, FS_SEEK_SET);
		LOG_INF("Seeked to frame %u (byte %lld)", seek_frame, (long long)byte_offset);
	} else {
		fs_seek(&cur_file, mp3_data_start, FS_SEEK_SET);
	}

	/* Reset decoder state (clears bit reservoir) */
	MP3FreeDecoder(mp3_dec);
	mp3_dec = MP3InitDecoder();
	mp3_buf_len   = 0;
	pcm_buf_pos   = 0;
	pcm_buf_avail = 0;

	return 0;
}

static void audio_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	// global_sample_counter = rand();

	/* Wait for first file */
	k_sem_take(&file_change_sem, K_FOREVER);

	const struct sdcard_audio_info *info = pending_info;
	pending_info = NULL;

	if (open_mp3(info) < 0) {
		LOG_ERR("Failed to open initial file");
		return;
	}

	int queued = 0;

	while (true) {
		/* Check for file change (non-blocking) */
		if (k_sem_take(&file_change_sem, K_NO_WAIT) == 0) {
			info = pending_info;
			pending_info = NULL;
			if (open_mp3(info) < 0) {
				LOG_ERR("Failed to switch file, continuing");
			}
		}

		void *buf;
		int rc = k_mem_slab_alloc(&i2s_mem_slab, &buf, K_MSEC(1000));
		if (rc < 0) {
			LOG_ERR("Buffer alloc failed: %d", rc);
			continue;
		}

		int32_t *out = (int32_t *)buf;
		uint16_t volume = current_volume_q8;

		/* Number of I2S output frames (one frame = channels × 32-bit words) */
		uint32_t out_frames = BLOCK_SIZE / (mp3_channels * 4);

		for (uint32_t f = 0; f < out_frames; f++) {
			if (pcm_buf_pos >= pcm_buf_avail) {
				fill_pcm_buf();
			}

			/* Scale volume: square for perceptual response */
			uint32_t gain_q16 = (uint32_t)volume * volume;

			for (uint16_t ch = 0; ch < mp3_channels; ch++) {
				int16_t s16 = (pcm_buf_pos < pcm_buf_avail)
						? pcm_buf[pcm_buf_pos++]
						: 0;
				int32_t sample = (int32_t)s16 << 16;
				*out++ = (int32_t)(((int64_t)sample * gain_q16) >> 16);
			}
		}

		rc = i2s_write(i2s_dev, buf, BLOCK_SIZE);
		if (rc < 0) {
			k_mem_slab_free(&i2s_mem_slab, buf);
			LOG_WRN("I2S write failed (%d), recovering", rc);
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
			i2s_started = false;
			queued = 0;
			continue;
		}

		global_sample_counter += out_frames;
		queued++;

		if (!i2s_started && queued == PREQUEUE_BLOCKS) {
			rc = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			if (rc < 0) {
				LOG_ERR("I2S start failed: %d", rc);
				return;
			}
			i2s_started = true;
			LOG_INF("I2S playback started");
		}
	}
}

K_THREAD_STACK_DEFINE(audio_thread_stack, STACK_SIZE);
static struct k_thread audio_thread_data;

int audio_init(void)
{
	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}

	mp3_dec = MP3InitDecoder();
	if (!mp3_dec) {
		LOG_ERR("MP3InitDecoder failed");
		return -ENOMEM;
	}

	k_sem_init(&file_change_sem, 0, 1);

	k_thread_create(&audio_thread_data, audio_thread_stack,
			K_THREAD_STACK_SIZEOF(audio_thread_stack),
			audio_thread_fn, NULL, NULL, NULL,
			THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&audio_thread_data, "audio");

	LOG_INF("Audio initialized");
	return 0;
}

int audio_set_file(const struct sdcard_audio_info *info)
{
	pending_info = info;
	k_sem_give(&file_change_sem);
	return 0;
}

void audio_set_volume(uint16_t volume_q8)
{
	current_volume_q8 = volume_q8;
}
