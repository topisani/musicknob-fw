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
static const char *volatile pending_path;
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
static bool file_open;

struct wav_file_hdr {
	uint32_t type_id;
	uint32_t size;
	uint32_t fmt_id;
};

struct wav_fmt {
	uint32_t id;
	uint32_t size;
	uint16_t fmt;
	uint16_t channels;
	uint32_t samplerate;
	uint32_t byterate;
	uint16_t framesize;
	uint16_t bitdepth;
};

struct wav_data_hdr {
	uint32_t id;
	uint32_t size;
};

#define WAV_ID(s) \
	(uint32_t)((s)[0] | ((s)[1] << 8) | ((s)[2] << 16) | ((s)[3] << 24))

#define WAV_RIFF_ID WAV_ID("RIFF")
#define WAV_WAVE_ID WAV_ID("WAVE")
#define WAV_FMT_ID  WAV_ID("fmt ")
#define WAV_DATA_ID WAV_ID("data")

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

static int open_wav(const char *path)
{
	struct wav_file_hdr hdr;
	struct wav_fmt fmt;
	struct wav_data_hdr dhdr;
	int rc;

	if (file_open) {
		fs_close(&cur_file);
		file_open = false;
	}

	fs_file_t_init(&cur_file);
	rc = fs_open(&cur_file, path, FS_O_READ);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", path, rc);
		return rc;
	}
	file_open = true;

	rc = fs_read(&cur_file, &hdr, sizeof(hdr));
	if (rc < (int)sizeof(hdr) ||
	    hdr.type_id != WAV_RIFF_ID || hdr.fmt_id != WAV_WAVE_ID) {
		LOG_ERR("Invalid WAV file");
		return -EINVAL;
	}

	rc = fs_read(&cur_file, &fmt, sizeof(fmt));
	if (rc < (int)sizeof(fmt) || fmt.id != WAV_FMT_ID) {
		LOG_ERR("Invalid fmt chunk");
		return -EINVAL;
	}

	if (fmt.fmt != 1) {
		LOG_ERR("Only PCM format supported (got %u)", fmt.fmt);
		return -ENOTSUP;
	}

	/* Skip extra format bytes */
	if (fmt.size > sizeof(fmt) - 8) {
		fs_seek(&cur_file, fmt.size - (sizeof(fmt) - 8), FS_SEEK_CUR);
	}

	/* Find data chunk */
	while (true) {
		rc = fs_read(&cur_file, &dhdr, sizeof(dhdr));
		if (rc < (int)sizeof(dhdr)) {
			LOG_ERR("Data chunk not found");
			return -EINVAL;
		}
		if (dhdr.id == WAV_DATA_ID) {
			break;
		}
		fs_seek(&cur_file, dhdr.size, FS_SEEK_CUR);
	}

	framesize = fmt.framesize;
	wav_bitdepth = fmt.bitdepth;
	wav_channels = fmt.channels;
	out_framesize = fmt.channels * 4;
	data_size = dhdr.size;
	nframes = data_size / framesize;
	data_start = fs_tell(&cur_file);

	LOG_INF("Opened %s: %u Hz, %u ch, %u bit, %u frames",
		path, fmt.samplerate, fmt.channels, fmt.bitdepth, nframes);

	/* Configure I2S on first file */
	if (!i2s_started) {
		rc = configure_i2s(fmt.samplerate, 32, fmt.channels);
		if (rc < 0) {
			LOG_ERR("I2S configure failed: %d", rc);
			return rc;
		}
	}

	/* Seek to position matching global counter */
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

	const char *path = pending_path;
	pending_path = NULL;

	if (open_wav(path) < 0) {
		LOG_ERR("Failed to open initial file");
		return;
	}

	int queued = 0;

	while (true) {
		/* Check for file change (non-blocking) */
		if (k_sem_take(&file_change_sem, K_NO_WAIT) == 0) {
			path = pending_path;
			pending_path = NULL;
			if (open_wav(path) < 0) {
				LOG_ERR("Failed to switch file");
				return;
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
			return;
		}

		expand_and_apply_volume(buf, frames, volume);

		rc = i2s_write(i2s_dev, buf, BLOCK_SIZE);
		if (rc < 0) {
			k_mem_slab_free(&i2s_mem_slab, buf);
			LOG_ERR("I2S write failed: %d", rc);
			return;
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

int audio_set_file(const char *path)
{
	pending_path = path;
	k_sem_give(&file_change_sem);
	return 0;
}
