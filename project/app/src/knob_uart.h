#pragma once

typedef void (*knob_uart_position_cb_t)(int raw_position);

/**
 * Initialize the STM32 knob UART listener.
 * @param position_cb Called with the raw encoder position on each 'E' message.
 */
void knob_uart_init(knob_uart_position_cb_t position_cb);
