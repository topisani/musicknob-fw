#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>

#include "sdcard.h"
#include "audio.h"
#include "zephyr/sys/util.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#define ENC_STEP_COUNT 100

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
		return 256; /* full volume on error */
	}
	int32_t val = CLAMP((int32_t)adc_raw_buf, 0, 4095);
	return (uint16_t)((val * 256) / 4095);
}

int main(void)
{

	LOG_INF("Hello world");
	sdcard_init();
	audio_init();

	int file_count = sdcard_get_audio_count();

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

	const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(qdec0));
	struct sensor_value val;

	if (!device_is_ready(dev)) {
		LOG_INF("Qdec device is not ready");
		return 0;
	}

	int last_position =  -1;

	while (true) {
		k_msleep(10);
		audio_set_volume(read_volume_q8());

		rc = sensor_sample_fetch(dev);
		if (rc != 0) {
			LOG_INF("Failed to fetch sample (%d)", rc);
			return 0;
		}

		rc = sensor_channel_get(dev, SENSOR_CHAN_ROTATION, &val);
		if (rc != 0) {
			LOG_INF("Failed to get data (%d)", rc);
			return 0;
		}

		if (file_count <= 0) {
			continue;
		}

		int position =
			(((val.val1 / 2) % ENC_STEP_COUNT) + ENC_STEP_COUNT) % ENC_STEP_COUNT;
		// position = (ENC_STEP_COUNT - position) % ENC_STEP_COUNT;
		if (position != last_position) {
			int i = position % file_count;
			struct sdcard_audio_info *info = sdcard_get_audio_info(i);
			LOG_INF("position %d, File %d: %s", position, i, info->path);
			audio_set_file(info);
			last_position = position;
		}
	}

	return 0;
}
