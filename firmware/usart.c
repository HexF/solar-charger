#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include "usart.h"

on_line_recv_cb on_line_recv;

char rx_buf[255];
unsigned int rx_head;

void usart_write(const char* c, unsigned int length)
{
  for (unsigned int i=0; i<length; i++)
    usart_send_blocking(USART1, c[i]);
}

void usart_print(const char* c)
{
  for (const char* i = c; i != 0; i++)
    usart_send_blocking(USART1, *i);
}

void configure_usart(void)
{
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_USART1EN);
  gpio_set_af(GPIOA, GPIO_AF7, GPIO9 | GPIO10);
  gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO9 | GPIO10);

  usart_enable(USART1);
  usart_set_databits(USART1, 8);
  usart_set_stopbits(USART1, USART_STOPBITS_1);
  usart_set_parity(USART1, USART_PARITY_NONE);
  usart_set_mode(USART1, USART_MODE_TX_RX);
  usart_set_baudrate(USART1, 115200);
  usart_enable_rx_interrupt(USART1);
}

void usart1_isr(void)
{
  if (usart_get_interrupt_source(USART1, USART_SR_RXNE)) {
    char c = usart_recv(USART1);
    if (c == '\n') {
      rx_buf[rx_head] = 0;
      on_line_recv(rx_buf, rx_head);
      rx_head = 0;
    } else {
      rx_buf[rx_head] = c;
      rx_head++;
    }
  }
}
