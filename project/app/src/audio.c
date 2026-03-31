#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/fs/fs.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "mp3.h"
#include "zephyr/random/random.h"

LOG_MODULE_REGISTER(audio, CONFIG_APP_LOG_LEVEL);

#define BLOCK_SIZE        (4096 * 2)
#define BLOCK_COUNT       8
#define DECODE_STACK_SIZE (4096 * 2)
#define I2S_STACK_SIZE    (4096 * 2)
#define DECODE_PRIO       (K_HIGHEST_APPLICATION_THREAD_PRIO + 1)
#define I2S_PRIO          K_HIGHEST_APPLICATION_THREAD_PRIO
#define PREQUEUE_BLOCKS   2

/* PCM pipe: stream of interleaved stereo int16_t pairs, ~4 frames of headroom */
// #define PCM_PIPE_SIZE (MP3_PCM_BUF_SAMPLES * sizeof(int16_t) * 2)

K_MEM_SLAB_DEFINE(i2s_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 64);
// K_PIPE_DEFINE(pcm_pipe, PCM_PIPE_SIZE, 64);

static const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s));

/* Volume: written from main thread, read from i2s thread */
static volatile uint16_t current_volume_q8 = 0;

/* File change: written from API, consumed by decode thread */
static struct sdcard_audio_info *volatile pending_info;
static struct k_sem file_change_sem;

/* Synchronized playback position (stereo samples per channel pushed into pipe) */
static uint64_t global_sample_counter;

/* --- Shared state --- */

static bool i2s_started;
static bool stopped;
static int queued;

/* --- Helpers --- */

static int configure_i2s()
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = 44100,
		.mem_slab = &i2s_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 10,
	};

	return i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
}

/* --- Decode thread --- */

static void decode_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	global_sample_counter = sys_rand32_get();

	static int16_t pcm_buf[MP3_PCM_BUF_SAMPLES];

	if (mp3_decoder_init() < 0) {
		return;
	}

	/* Wait for first file */
	k_sem_take(&file_change_sem, K_FOREVER);

	struct sdcard_audio_info *info = pending_info;
	pending_info = NULL;

	if (mp3_switch_file(info, global_sample_counter) < 0) {
		LOG_ERR("Failed to open initial file");
		return;
	}

	while (true) {
		int pcm_buf_avail = 0;
		// k_msleep(1)
		/* Check for file change (non-blocking) */
		if (k_sem_take(&file_change_sem, K_NO_WAIT) == 0) {
			uint32_t t_switch_start = k_uptime_ticks();

			info = pending_info;
			pending_info = NULL;
			if (mp3_switch_file(info, global_sample_counter) < 0) {
				LOG_ERR("Failed to switch file, continuing");
			}
			uint32_t t_opened = k_uptime_ticks();

			pcm_buf_avail = mp3_decode_frame(pcm_buf);
			uint32_t t_first_decode = k_uptime_ticks();

			LOG_INF("file switch: open=%u us, first_decode=%u us, total=%u us",
				k_ticks_to_us_near32(t_opened - t_switch_start),
				k_ticks_to_us_near32(t_first_decode - t_opened),
				k_ticks_to_us_near32(k_uptime_ticks() - t_switch_start));
		} else {
			pcm_buf_avail = mp3_decode_frame(pcm_buf);
		}

		if (pcm_buf_avail == 0) {
			LOG_ERR("No PCM Bytes");
			k_msleep(1);
			continue;
		}

		// Apply replaygain if negative
		// if (info->rg_multiplier_q16 < (0xFFFF)) {
		// 	for (int i = 0; i < pcm_buf_avail; i++) {
		// 		pcm_buf[i] = ((int32_t)pcm_buf[i] * info->rg_multiplier_q16) >> 16;
		// 	}
		// }

		// size_t bytes = pcm_buf_avail * sizeof(int16_t);
		// int wrote = k_pipe_write(&pcm_pipe, (uint8_t *)pcm_buf, bytes, K_FOREVER);
		// if (wrote != bytes) {
		// 	LOG_WRN("Wrote wrong number of bytes: %d != %d", wrote, bytes);
		// }

		/* Track decoded samples per channel for seek-on-switch.
		 * pcm_buf_avail is total interleaved int16_t (L+R), so /2 per channel. */
		global_sample_counter += pcm_buf_avail / 2;
		// k_msleep(20);
	}
}

/* --- I2S thread --- */

static void i2s_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int volume = 0;
	uint32_t gain_q16 = 0;

	global_sample_counter = sys_rand32_get();

	static int16_t carry_buf[MP3_PCM_BUF_SAMPLES];
	static int carry_avail = 0;
	const int samples_per_block = BLOCK_SIZE / sizeof(int16_t);

	if (mp3_decoder_init() < 0) {
		return;
	}

	/* Wait for first file */
	k_sem_take(&file_change_sem, K_FOREVER);

	struct sdcard_audio_info *info = pending_info;
	pending_info = NULL;

	if (mp3_switch_file(info, global_sample_counter) < 0) {
		LOG_ERR("Failed to open initial file");
		return;
	}

	int rc = configure_i2s();
	if (rc < 0) {
		LOG_ERR("I2S configure failed: %d", rc);
	}

	while (true) {
		// static int16_t interbuf[BLOCK_SIZE / sizeof(int32_t)];
		//

		int16_t *buf;
		int rc = k_mem_slab_alloc(&i2s_mem_slab, (void **)&buf, K_MSEC(1000));
		if (rc < 0) {
			LOG_ERR("Buffer alloc failed: %d", rc);
			// k_msleep(1);
			continue;
		}

		// k_msleep(1)
		/* Check for file change (non-blocking) */
		if (k_sem_take(&file_change_sem, K_NO_WAIT) == 0) {
			uint32_t t_switch_start = k_uptime_ticks();

			info = pending_info;
			pending_info = NULL;
			if (mp3_switch_file(info, global_sample_counter) < 0) {
				LOG_ERR("Failed to switch file, continuing");
			}
			uint32_t t_opened = k_uptime_ticks();

			// pcm_buf_avail = mp3_decode_frame(carry_buf);
			// uint32_t t_first_decode = k_uptime_ticks();

			// LOG_INF("file switch: open=%u us, first_decode=%u us, total=%u us",
			// 	k_ticks_to_us_near32(t_opened - t_switch_start),
			// 	k_ticks_to_us_near32(t_first_decode - t_opened),
			// 	k_ticks_to_us_near32(k_uptime_ticks() - t_switch_start));
		}

		/* Fill slab block: copy carry from previous iteration, then decode
		 * directly into the slab buffer. Use carry_buf only for the boundary
		 * frame that would overflow the block. */
		int filled = 0;

		if (carry_avail > 0) {
			int to_copy = MIN(carry_avail, samples_per_block);
			memcpy(buf, carry_buf, to_copy * sizeof(int16_t));
			filled += to_copy;
			carry_avail -= to_copy;
			if (carry_avail > 0) {
				memmove(carry_buf, carry_buf + to_copy,
					carry_avail * sizeof(int16_t));
			}
		}

		while (filled < samples_per_block) {
			int remaining = samples_per_block - filled;
			if (remaining >= MP3_PCM_BUF_SAMPLES) {
				/* Decode directly into slab */
				int decoded = mp3_decode_frame(buf + filled);
				if (decoded == 0) {
					// k_msleep(1);
					continue;
				}
				filled += decoded;
			} else {
				/* Boundary: decode into temp, split */
				int decoded = mp3_decode_frame(carry_buf);
				if (decoded == 0) {
					// k_msleep(1);
					continue;
				}
				int to_copy = MIN(decoded, remaining);
				memcpy(buf + filled, carry_buf,
				       to_copy * sizeof(int16_t));
				filled += to_copy;
				carry_avail = decoded - to_copy;
				if (carry_avail > 0) {
					memmove(carry_buf, carry_buf + to_copy,
						carry_avail * sizeof(int16_t));
				}
			}
		}

		// if (pcm_buf_avail == 0) {
		// 	LOG_ERR("No PCM Bytes");
		// 	k_mem_slab_free(&i2s_mem_slab, &buf);
		// 	k_msleep(1);
		// 	continue;
		// }

		// Apply replaygain if negative
		// if (info->rg_multiplier_q16 < (0xFFFF)) {
		// 	for (int i = 0; i < pcm_buf_avail; i++) {
		// 		pcm_buf[i] = ((int32_t)pcm_buf[i] * info->rg_multiplier_q16) >> 16;
		// 	}
		// }

		// size_t bytes = pcm_buf_avail * sizeof(int16_t);
		// int wrote = k_pipe_write(&pcm_pipe, (uint8_t *)pcm_buf, bytes, K_FOREVER);
		// if (wrote != bytes) {
		// 	LOG_WRN("Wrote wrong number of bytes: %d != %d", wrote, bytes);
		// }

		/* Track decoded samples per channel for seek-on-switch.
		 * samples_per_block is total interleaved int16_t (L+R), so /2 per channel. */
		global_sample_counter += samples_per_block / 2;

		// Limit volume change rate
		const int vol_rate = 8;
		volume = volume + CLAMP(current_volume_q8 - volume, -vol_rate, vol_rate);
		uint32_t gain_q16 = (uint32_t)volume * volume;

		/* Read a full block of stereo int16 samples directly into the I2S buffer.
		 * Blocks until the decode thread has produced enough data. */
		// int bytes_read =
		// 	k_pipe_read(&pcm_pipe, (uint8_t *)interbuf, sizeof(interbuf), K_NO_WAIT);
		// int bytes_read =
		// 	k_pipe_read(&pcm_pipe, (uint8_t *)buf, BLOCK_SIZE, K_MSEC(1000));

		// if (bytes_read == sizeof(interbuf)) {
		// 	/* Apply volume scaling in-place: square for perceptual response */
		// 	for (size_t i = 0; i < BLOCK_SIZE / sizeof(int32_t); i++) {
		// 		buf[i] = ((int32_t)interbuf[i] * gain_q16);
		// 	}
		// } else {
		// 	if (bytes_read < 0) {
		// 		LOG_ERR("PCM pipe read failed: %d", rc);
		// 	}
		// 	memset(buf, 0, BLOCK_SIZE);
		// }

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

		// k_msleep(10);
	}
}

/* --- Thread definitions --- */

// K_THREAD_STACK_DEFINE(decode_thread_stack, DECODE_STACK_SIZE);
// static struct k_thread decode_thread_data;

K_THREAD_STACK_DEFINE(i2s_thread_stack, I2S_STACK_SIZE);
static struct k_thread i2s_thread_data;

int audio_init(void)
{
	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}

	k_sem_init(&file_change_sem, 0, 1);

	// k_thread_create(&decode_thread_data, decode_thread_stack,
	// 		K_THREAD_STACK_SIZEOF(decode_thread_stack), decode_thread_fn, NULL, NULL,
	// 		NULL, DECODE_PRIO, 0, K_NO_WAIT);
	// k_thread_name_set(&decode_thread_data, "mp3_decode");

	k_thread_create(&i2s_thread_data, i2s_thread_stack, K_THREAD_STACK_SIZEOF(i2s_thread_stack),
			i2s_thread_fn, NULL, NULL, NULL, I2S_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&i2s_thread_data, "i2s_out");

	LOG_INF("Audio initialized");
	return 0;
}

int audio_set_file(struct sdcard_audio_info *info)
{
	pending_info = info;
	k_sem_give(&file_change_sem);
	return 0;
}

void audio_set_volume(uint16_t volume_q8)
{
	current_volume_q8 = volume_q8;
}

void audio_stop(void)
{
	stopped = true;
	k_thread_suspend(&decode_thread_data);
	k_thread_suspend(&i2s_thread_data);
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	i2s_started = false;

	/* Flush the PCM pipe */
	uint8_t trash[256];
	while (k_pipe_read(&pcm_pipe, trash, sizeof(trash), K_NO_WAIT) > 0) {
	}

	LOG_INF("Audio stopped");
}

void audio_start(void)
{
	stopped = false;

	/* Re-configure I2S after DROP */
	configure_i2s();

	/* Flush pipe and reset I2S state */
	k_pipe_reset(&pcm_pipe);
	i2s_started = false;
	queued = 0;

	k_thread_resume(&decode_thread_data);
	k_thread_resume(&i2s_thread_data);

	LOG_INF("Audio started");
}
