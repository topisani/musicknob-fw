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
#include "zephyr/random/random.h"
#include "libhelix-mp3/pub/mp3common.h"

LOG_MODULE_REGISTER(audio, CONFIG_APP_LOG_LEVEL);

#define BLOCK_SIZE        (4096)
#define BLOCK_COUNT       3
#define DECODE_STACK_SIZE (4096 * 2)
#define I2S_STACK_SIZE    (4096)
#define DECODE_PRIO       (K_HIGHEST_APPLICATION_THREAD_PRIO + 1)
#define I2S_PRIO          K_HIGHEST_APPLICATION_THREAD_PRIO
#define PREQUEUE_BLOCKS   2

/* MP3 input buffer: large enough to hold >1 full frame (max ~1441 bytes) */
#define MP3_BUF_SIZE    8192
/* PCM decode buffer: one MPEG1 stereo frame = 1152 samples × 2 channels */
#define PCM_BUF_SAMPLES (2304)
/* PCM pipe: stream of interleaved stereo int16_t pairs, ~4 frames of headroom */
#define PCM_PIPE_SIZE   (PCM_BUF_SAMPLES * sizeof(int16_t) * 8)

K_MEM_SLAB_DEFINE(i2s_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 8);
K_PIPE_DEFINE(pcm_pipe, PCM_PIPE_SIZE, 4);

static const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s));

/* Volume: written from main thread, read from i2s thread */
static volatile uint16_t current_volume_q8 = 256; /* default full volume */

/* File change: written from API, consumed by decode thread */
static const struct sdcard_audio_info *volatile pending_info;
static struct k_sem file_change_sem;

/* --- Decode thread private state --- */

static struct fs_file_t cur_file;
static bool file_open;
static bool i2s_configured;

static off_t mp3_data_start;
static uint32_t mp3_total_samples;
static uint32_t mp3_frame_bytes_num;
static uint32_t mp3_samplerate;

/* Synchronized playback position (stereo samples per channel pushed into pipe) */
static uint64_t global_sample_counter;

static HMP3Decoder mp3_dec;
static uint8_t mp3_buf[MP3_BUF_SIZE];
static int mp3_buf_len;

/* --- I2S thread private state --- */

static bool i2s_started;

/* --- Helpers --- */

/* Clear the bit reservoir so stale data from a previous file/position
 * does not corrupt decoding of the new stream. */
static void mp3_clear_bit_reservoir(void)
{
	MP3DecInfo *dec = (MP3DecInfo *)mp3_dec;
	memset(dec->mainBuf, 0, MAINBUF_SIZE);
	dec->mainDataBytes = 0;
}

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

/* --- Decode thread --- */

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
		/* EOF — discard stale data and loop with a clean bit reservoir */
		mp3_buf_len = 0;
		mp3_clear_bit_reservoir();
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

/* Decode one MP3 frame into pcm_buf; handles sync search and underflow.
 * Sets pcm_buf_avail to the number of valid int16_t samples produced. */
static int16_t fill_pcm_buf(int16_t *buf)
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

		err = MP3Decode(mp3_dec, &read_ptr, &bytes_left, buf, 0);

		/* Update buffer: consume bytes that the decoder advanced past */
		int consumed = mp3_buf_len - bytes_left;
		if (consumed > 0) {
			memmove(mp3_buf, mp3_buf + consumed, bytes_left);
			mp3_buf_len = bytes_left;
		}

		if (err == ERR_MP3_NONE) {
			MP3FrameInfo fi;
			MP3GetLastFrameInfo(mp3_dec, &fi);

			uint32_t elapsed_us = k_ticks_to_us_near32(k_uptime_ticks()) - t0_us;
			if (skipped_bytes || maindata_underflows || indata_underflows ||
			    other_errors || elapsed_us > 10000) {
				LOG_WRN("fill_pcm_buf: %u us, skipped=%d maindata_uflow=%d "
					"indata_uflow=%d other_err=%d samps=%d",
					elapsed_us, skipped_bytes, maindata_underflows,
					indata_underflows, other_errors, fi.outputSamps);
			}
			return fi.outputSamps;
		} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
			maindata_underflows++;
			continue;
		} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
			indata_underflows++;
			continue;
		} else {
			LOG_ERR("MP3Decode: other err: %d", err);
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
		elapsed_us, mp3_buf_len, skipped_bytes, maindata_underflows, indata_underflows,
		other_errors);
	return 0;
}

static int open_mp3(const struct sdcard_audio_info *info)
{
	uint32_t t_start = k_uptime_ticks();
	uint32_t t_close, t_open, t_seek;

	if (file_open) {
		fs_close(&cur_file);
		file_open = false;
	}
	t_close = k_uptime_ticks();

	fs_file_t_init(&cur_file);
	int rc = fs_open(&cur_file, info->path, FS_O_READ);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", info->path, rc);
		return rc;
	}
	file_open = true;
	t_open = k_uptime_ticks();

	mp3_data_start = info->data_start;
	mp3_total_samples = info->total_samples;
	mp3_frame_bytes_num = info->frame_bytes_num;
	mp3_samplerate = info->samplerate;

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
		uint32_t seek_frame = seek_sample / 1152;
		/* Rounded rational arithmetic: avoids floor-division accumulation error */
		off_t byte_offset =
			mp3_data_start +
			(off_t)(((uint64_t)seek_frame * mp3_frame_bytes_num + mp3_samplerate / 2) /
				mp3_samplerate);
		fs_seek(&cur_file, byte_offset, FS_SEEK_SET);
	} else {
		fs_seek(&cur_file, mp3_data_start, FS_SEEK_SET);
	}
	t_seek = k_uptime_ticks();

	/* Discard stale compressed data and clear the bit reservoir */
	mp3_buf_len = 0;
	mp3_clear_bit_reservoir();

	LOG_INF("open_mp3 %s: close=%u us, open=%u us, seek=%u us, total=%u us",
		info->path,
		k_ticks_to_us_near32(t_close - t_start),
		k_ticks_to_us_near32(t_open - t_close),
		k_ticks_to_us_near32(t_seek - t_open),
		k_ticks_to_us_near32(t_seek - t_start));

	return 0;
}

static void decode_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	global_sample_counter = sys_rand32_get();

	static int16_t pcm_buf[PCM_BUF_SAMPLES];

	/* Wait for first file */
	k_sem_take(&file_change_sem, K_FOREVER);

	const struct sdcard_audio_info *info = pending_info;
	pending_info = NULL;

	if (open_mp3(info) < 0) {
		LOG_ERR("Failed to open initial file");
		return;
	}

	while (true) {
		/* Check for file change (non-blocking) */
		if (k_sem_take(&file_change_sem, K_NO_WAIT) == 0) {
			uint32_t t_switch_start = k_uptime_ticks();

			info = pending_info;
			pending_info = NULL;
			if (open_mp3(info) < 0) {
				LOG_ERR("Failed to switch file, continuing");
			}
			uint32_t t_opened = k_uptime_ticks();

			int pcm_buf_avail = fill_pcm_buf(pcm_buf);
			uint32_t t_first_decode = k_uptime_ticks();

			k_pipe_reset(&pcm_pipe);

			size_t bytes = pcm_buf_avail * sizeof(int16_t);
			int wrote = k_pipe_write(&pcm_pipe, (uint8_t *)pcm_buf,
						 bytes, K_FOREVER);
			if (wrote != bytes) {
				LOG_WRN("Wrote wrong number of bytes: %d != %d",
					wrote, (int)bytes);
			}
			global_sample_counter += pcm_buf_avail / 2;

			LOG_INF("file switch: open=%u us, first_decode=%u us, total=%u us",
				k_ticks_to_us_near32(t_opened - t_switch_start),
				k_ticks_to_us_near32(t_first_decode - t_opened),
				k_ticks_to_us_near32(k_uptime_ticks() - t_switch_start));
			continue;
		}

		int pcm_buf_avail = fill_pcm_buf(pcm_buf);

		size_t bytes = pcm_buf_avail * sizeof(int16_t);
		int wrote = k_pipe_write(&pcm_pipe, (uint8_t *)pcm_buf, bytes, K_FOREVER);
		if (wrote != bytes) {
			LOG_WRN("Wrote wrong number of bytes: %d != %d", wrote, bytes);
		}

		/* Track decoded samples per channel for seek-on-switch.
		 * pcm_buf_avail is total interleaved int16_t (L+R), so /2 per channel. */
		global_sample_counter += pcm_buf_avail / 2;
	}
}

/* --- I2S thread --- */

static void i2s_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int queued = 0;

	while (true) {
		int32_t *buf;
		int rc = k_mem_slab_alloc(&i2s_mem_slab, (void **)&buf, K_MSEC(1000));
		if (rc < 0) {
			LOG_ERR("Buffer alloc failed: %d", rc);
			continue;
		}

		static int16_t interbuf[BLOCK_SIZE / sizeof(int32_t)];

		/* Read a full block of stereo int16 samples directly into the I2S buffer.
		 * Blocks until the decode thread has produced enough data. */
		int bytes_read = k_pipe_read(&pcm_pipe, (uint8_t *)interbuf, sizeof(interbuf),
					     queued < PREQUEUE_BLOCKS ? K_FOREVER : K_NO_WAIT);
		if (bytes_read == sizeof(interbuf)) {
			/* Apply volume scaling in-place: square for perceptual response */
			uint32_t gain_q16 = (uint32_t)current_volume_q8 * current_volume_q8;
			for (size_t i = 0; i < BLOCK_SIZE / sizeof(int32_t); i++) {
				buf[i] = ((int32_t)interbuf[i] * gain_q16);
			}
		} else {
			if (bytes_read < 0) {
				LOG_ERR("PCM pipe read failed: %d", rc);
			}
			memset(buf, 0, sizeof(buf));
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

/* --- Thread definitions --- */

K_THREAD_STACK_DEFINE(decode_thread_stack, DECODE_STACK_SIZE);
static struct k_thread decode_thread_data;

K_THREAD_STACK_DEFINE(i2s_thread_stack, I2S_STACK_SIZE);
static struct k_thread i2s_thread_data;

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

	k_thread_create(&decode_thread_data, decode_thread_stack,
			K_THREAD_STACK_SIZEOF(decode_thread_stack), decode_thread_fn, NULL, NULL,
			NULL, DECODE_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&decode_thread_data, "mp3_decode");

	k_thread_create(&i2s_thread_data, i2s_thread_stack, K_THREAD_STACK_SIZEOF(i2s_thread_stack),
			i2s_thread_fn, NULL, NULL, NULL, I2S_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&i2s_thread_data, "i2s_out");

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
