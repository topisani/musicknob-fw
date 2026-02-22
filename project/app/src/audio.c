#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "audio.h"

LOG_MODULE_REGISTER(audio, CONFIG_APP_LOG_LEVEL);

#define BLOCK_SIZE	(4096)
#define BLOCK_COUNT	8
#define STACK_SIZE	4096
#define THREAD_PRIO	5
#define PREQUEUE_BLOCKS 4

K_MEM_SLAB_DEFINE(i2s_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s));

static const struct adc_dt_spec volume_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static int16_t adc_raw_buf;
static struct adc_sequence adc_seq = {
	.buffer = &adc_raw_buf,
	.buffer_size = sizeof(adc_raw_buf),
};

/* Inter-thread communication */
static const struct sdcard_wav_info *volatile pending_info;
static struct k_sem file_change_sem;

/* Thread state */
static struct fs_file_t cur_file;
static off_t data_start;
static uint32_t data_size;
static uint16_t framesize;
static uint16_t wav_bitdepth;
static uint16_t wav_channels;
static uint16_t out_framesize; /* wav_channels * 4 */
static uint32_t nframes;
static uint64_t global_frame_counter;
static bool i2s_started;
static bool i2s_configured;
static bool file_open;

static int configure_i2s(uint32_t samplerate, uint16_t bitdepth, uint16_t channels)
{
	struct i2s_config cfg = {
		.word_size = bitdepth,
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

static int open_wav(const struct sdcard_wav_info *info)
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

	/* Use cached metadata — no header parsing */
	framesize     = info->framesize;
	wav_bitdepth  = info->bitdepth;
	wav_channels  = info->channels;
	out_framesize = info->channels * 4;
	data_size     = info->data_size;
	nframes       = info->nframes;
	data_start    = info->data_start;

	LOG_INF("Opened %s: %u Hz, %u ch, %u bit, %u frames",
		info->path, info->samplerate, info->channels, info->bitdepth, nframes);

	if (!i2s_configured) {
		rc = configure_i2s(info->samplerate, 32, info->channels);
		if (rc < 0) {
			LOG_ERR("I2S configure failed: %d", rc);
			return rc;
		}
		i2s_configured = true;
	}

	if (nframes > 0) {
		uint32_t frame_offset = global_frame_counter % nframes;
		fs_seek(&cur_file, data_start + frame_offset * framesize, FS_SEEK_SET);
	}

	return 0;
}

static int read_looping(void *buf, size_t len)
{
	size_t filled = 0;

	while (filled < len) {
		int rc = fs_read(&cur_file, (uint8_t *)buf + filled, len - filled);
		if (rc < 0) {
			return rc;
		}
		filled += rc;

		if ((size_t)rc < len - filled || filled < len) {
			/* Hit EOF or short read — loop back */
			off_t pos = fs_tell(&cur_file);
			if (pos >= data_start + (off_t)data_size || rc == 0) {
				fs_seek(&cur_file, data_start, FS_SEEK_SET);
			}
		}
	}

	return filled;
}

static uint16_t read_volume_q8(void)
{
	int rc = adc_read_dt(&volume_adc, &adc_seq);
	if (rc < 0) {
		return 256; /* full volume on error */
	}
	int32_t val = CLAMP((int32_t)adc_raw_buf, 0, 4095);
	return (uint16_t)((val * 256) / 4095);
}

static void expand_and_apply_volume(void *buf, uint32_t frames, uint16_t volume_q8)
{
	uint8_t *raw = (uint8_t *)buf;
	int32_t *out = (int32_t *)buf;
	int bps = wav_bitdepth / 8;
	int total = (int)(frames * wav_channels);

	for (int i = total - 1; i >= 0; i--) {
		int32_t sample;
		int ri = i * bps;

		if (wav_bitdepth == 8) {
			/* 8-bit WAV is unsigned */
			sample = (int32_t)((int8_t)((int)raw[ri] - 128)) << 24;
		} else if (wav_bitdepth == 16) {
			int16_t s;
			memcpy(&s, raw + ri, 2);
			sample = (int32_t)s << 16;
		} else if (wav_bitdepth == 24) {
			int32_t s = (int32_t)raw[ri] |
				    ((int32_t)raw[ri + 1] << 8) |
				    ((int32_t)raw[ri + 2] << 16);
			if (s & 0x800000) {
				s |= (int32_t)0xFF000000;
			}
			sample = s << 8;
		} else {
			/* 32-bit: read as-is */
			memcpy(&sample, raw + ri, 4);
		}

		/* Square the volume for a logarithmic perceptual response */
		uint32_t gain_q16 = (uint32_t)volume_q8 * volume_q8;
		out[i] = (int32_t)(((int64_t)sample * gain_q16) >> 16);
	}
}

static void audio_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Wait for first file */
	k_sem_take(&file_change_sem, K_FOREVER);

	const struct sdcard_wav_info *info = pending_info;
	pending_info = NULL;

	if (open_wav(info) < 0) {
		LOG_ERR("Failed to open initial file");
		return;
	}

	int queued = 0;

	while (true) {
		/* Check for file change (non-blocking) */
		if (k_sem_take(&file_change_sem, K_NO_WAIT) == 0) {
			info = pending_info;
			pending_info = NULL;
			if (open_wav(info) < 0) {
				LOG_ERR("Failed to switch file, continuing");
				/* continue with old file still open */
			}
		}

		void *buf;
		int rc = k_mem_slab_alloc(&i2s_mem_slab, &buf, K_MSEC(1000));
		if (rc < 0) {
			LOG_ERR("Buffer alloc failed: %d", rc);
			continue;
		}

		uint32_t frames = BLOCK_SIZE / out_framesize;
		uint32_t raw_bytes = frames * framesize;
		uint16_t volume = read_volume_q8();

		rc = read_looping(buf, raw_bytes);
		if (rc < 0) {
			k_mem_slab_free(&i2s_mem_slab, buf);
			LOG_ERR("Read failed: %d", rc);
			continue;
		}

		expand_and_apply_volume(buf, frames, volume);

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

		global_frame_counter += frames;
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

	if (!adc_is_ready_dt(&volume_adc)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	int rc = adc_channel_setup_dt(&volume_adc);
	if (rc < 0) {
		LOG_ERR("ADC channel setup failed: %d", rc);
		return rc;
	}

	adc_sequence_init_dt(&volume_adc, &adc_seq);

	k_sem_init(&file_change_sem, 0, 1);

	k_thread_create(&audio_thread_data, audio_thread_stack,
			K_THREAD_STACK_SIZEOF(audio_thread_stack),
			audio_thread_fn, NULL, NULL, NULL,
			THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&audio_thread_data, "audio");

	LOG_INF("Audio initialized");
	return 0;
}

int audio_set_file(const struct sdcard_wav_info *info)
{
	pending_info = info;
	k_sem_give(&file_change_sem);
	return 0;
}
