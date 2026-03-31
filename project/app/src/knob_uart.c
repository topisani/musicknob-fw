#include "knob_uart.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <stdlib.h>

LOG_MODULE_REGISTER(knob_uart, CONFIG_APP_LOG_LEVEL);

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, stm32_uart)

#define KNOB_UART_NODE DT_PROP(ZEPHYR_USER_NODE, stm32_uart)

#define RX_BUF_SIZE 64

static const struct device *const uart_dev = DEVICE_DT_GET(KNOB_UART_NODE);
static uint8_t rx_buf[RX_BUF_SIZE];
static uint8_t rx_pos;
static knob_uart_position_cb_t position_cb;

static K_SEM_DEFINE(rx_sem, 0, 1);

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			continue;
		}

		uint8_t buf[16];
		int len = uart_fifo_read(dev, buf, sizeof(buf));

		for (int i = 0; i < len; i++) {
			if (rx_pos < RX_BUF_SIZE - 1) {
				rx_buf[rx_pos++] = buf[i];
			}
			if (buf[i] == '\n' || rx_pos >= RX_BUF_SIZE - 1) {
				rx_buf[rx_pos] = '\0';
				k_sem_give(&rx_sem);
			}
		}
	}
}

static void knob_uart_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&rx_sem, K_FOREVER);
		switch (rx_buf[0]) {
		case 'E': {
			// Parse "E:<HEX>" — hex string is the encoder position
			if (rx_buf[1] == ':' && position_cb != NULL) {
				LOG_INF("STM: %s", rx_buf);
				position_cb((int)strtol((char *)&rx_buf[2], NULL, 16));
			} else if (rx_buf[1] != ':') {
				LOG_WRN("Malformed E msg: %s", rx_buf);
			}
			break;
		}

		default:
			LOG_WRN("Unknown msg %s", rx_buf);
		}
		rx_pos = 0;
	}
}

K_THREAD_STACK_DEFINE(knob_uart_stack, 1024);
static struct k_thread knob_uart_thread_data;

void knob_uart_init(knob_uart_position_cb_t cb)
{
	position_cb = cb;

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return;
	}

	uart_irq_callback_set(uart_dev, uart_isr);
	uart_irq_rx_enable(uart_dev);

	k_thread_create(&knob_uart_thread_data, knob_uart_stack,
			K_THREAD_STACK_SIZEOF(knob_uart_stack), knob_uart_thread, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&knob_uart_thread_data, "knob_uart");

	LOG_INF("Knob UART initialized");
}

#else

void knob_uart_init(knob_uart_position_cb_t cb)
{
	ARG_UNUSED(cb);
}

#endif /* DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, stm32_uart) */
