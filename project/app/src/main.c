#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include "sdcard.h"
#include "audio.h"
#include "knob_uart.h"
#include "zephyr/sys/util.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#define ENC_STEP_COUNT 100

static int position = 0;
static int file_count = 0;

static void on_encoder_position(int p)
{
	position = p;
}

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#include <zephyr/drivers/adc.h>

static const struct adc_dt_spec volume_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static int16_t adc_raw_buf;
static struct adc_sequence adc_seq = {
	.buffer = &adc_raw_buf,
	.buffer_size = sizeof(adc_raw_buf),
};

static uint16_t read_volume_q8(void)
{
	int rc = adc_read_dt(&volume_adc, &adc_seq);
	if (rc < 0) {
		return 255; /* full volume on error */
	}
	int32_t val = CLAMP((int32_t)adc_raw_buf, 0, 4095);
	return (uint16_t)((val * 256) / 4095);
}

#else

static uint16_t read_volume_q8(void)
{
	return 256; /* full volume when no ADC */
}

#endif /* DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels) */

#if DT_HAS_ALIAS(qdec0)
#include <zephyr/drivers/sensor.h>
#endif

int main(void)
{
	LOG_INF("Hello world");
	knob_uart_init(on_encoder_position);
	sdcard_init();
	audio_init();

	file_count = sdcard_get_audio_count();

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
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
#endif

#if DT_HAS_ALIAS(qdec0)
	const struct device *const qdec = DEVICE_DT_GET(DT_ALIAS(qdec0));

	if (!device_is_ready(qdec)) {
		LOG_INF("Qdec device is not ready");
		return 0;
	}
#endif

	int last_position = -1;
	bool idle = false;
	int64_t zero_vol_since = 0;

	while (true) {
		k_msleep(idle ? 1000 : 10);

		uint16_t vol = read_volume_q8();
		audio_set_volume(vol);

		if (!idle) {
			/* Track how long volume has been zero */
			if (vol == 0) {
				if (zero_vol_since == 0) {
					zero_vol_since = k_uptime_get();
				} else if (k_uptime_get() - zero_vol_since >= 5000) {
					audio_stop();
					idle = true;
					LOG_INF("Entering idle mode");
					continue;
				}
			} else {
				zero_vol_since = 0;
			}
		} else {
			/* Idle: check if volume came back */
			if (vol > 0) {
				idle = false;
				zero_vol_since = 0;
				audio_start();
				/* Re-select current file */
				if (file_count > 0 && last_position >= 0) {
					int i = last_position % file_count;
					audio_set_file(sdcard_get_audio_info(i));
				}
				LOG_INF("Leaving idle mode");
			}
		}

#if DT_HAS_ALIAS(qdec0)
		int enc_rc = sensor_sample_fetch(qdec);
		if (enc_rc != 0) {
			LOG_INF("Failed to fetch sample (%d)", enc_rc);
			return 0;
		}

		struct sensor_value val;

		enc_rc = sensor_channel_get(qdec, SENSOR_CHAN_ROTATION, &val);
		if (enc_rc != 0) {
			LOG_INF("Failed to get data (%d)", enc_rc);
			return 0;
		}

 		int p =
 			(((val.val1 / 2) % ENC_STEP_COUNT) + ENC_STEP_COUNT) % ENC_STEP_COUNT;
		on_encoder_position(p);
#endif /* DT_HAS_ALIAS(qdec0) */

 		if (file_count <= 0) {
 			continue;
 		}
 
 		if (position != last_position) {
 			last_position = position;
			if (!idle) {
				int i = position % file_count;
				struct sdcard_audio_info *info = sdcard_get_audio_info(i);
				LOG_INF("position %d, File %d: %s", position, i, info->path);
				audio_set_file(info);
			}
 		}
	}

	return 0;
}
