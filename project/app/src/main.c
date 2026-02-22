#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "sdcard.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

static void qdec_loop(void)
{
	const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(qdec0));
	struct sensor_value val;
	int rc;

	if (!device_is_ready(dev)) {
		LOG_INF("Qdec device is not ready");
		return;
	}

	LOG_INF("Quadrature decoder sensor test");

	int last_position = -1;

	while (true) {
		k_msleep(10);

		rc = sensor_sample_fetch(dev);
		if (rc != 0) {
			LOG_INF("Failed to fetch sample (%d)", rc);
			return;
		}

		rc = sensor_channel_get(dev, SENSOR_CHAN_ROTATION, &val);
		if (rc != 0) {
			LOG_INF("Failed to get data (%d)", rc);
			return;
		}

		int position = (((val.val1 / 2) % 100) + 100) % 100;
		if (position != last_position) {
			LOG_INF("Position = %d", position);
			last_position = position;
		}
	}
}

int main(void)
{
	sdcard_init();
	qdec_loop();
	return 0;
}
