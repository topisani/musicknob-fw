#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);


int main(void)
{
	struct sensor_value val;
	int rc;
	const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(qdec0));

	if (!device_is_ready(dev)) {
		LOG_INF("Qdec device is not ready");
		return 0;
	}

	LOG_INF("Quadrature decoder sensor test");

	int last_position = -1;

	while (true) {
		k_msleep(10);

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

		int position = (((val.val1 / 2) % 100) + 100) % 100;
		if (position != last_position) {
			LOG_INF("Position = %d", position);
			last_position = position;
		}
	}
	return 0;
}
