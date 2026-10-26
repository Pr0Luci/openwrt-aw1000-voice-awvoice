// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Line-chip pins: reset, interrupt and the register bus.
 *
 * The register bus is driven pin by pin: clock idles high, the data line changes while the clock is low
 * and is sampled when the clock goes high, most significant bit first, select active low. Only the pins
 * named in enum aw_pin can be requested.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/module.h>

#include "awfxs.h"

#define FIRST_BYTE_IS_READ	BIT(6)

static unsigned int half_ns = 520;
static bool allow_write;

module_param(half_ns, uint, 0444);
MODULE_PARM_DESC(half_ns, "register bus half clock period in ns (200..5000)");

/* board wiring of the register bus: data in, data out, clock, select; a permutation of pins 50..53 */
static int bus_pins[4] = { AW_PIN_BUS_MISO, AW_PIN_BUS_MOSI, AW_PIN_BUS_CLK, AW_PIN_BUS_SEL };
module_param_array(bus_pins, int, NULL, 0444);
MODULE_PARM_DESC(bus_pins, "pins of the register bus as miso,mosi,clk,sel (default 52,53,50,51)");

module_param(allow_write, bool, 0444);
MODULE_PARM_DESC(allow_write, "permit register writes (default: reads only)");

/*
 * Every requested pin: where the descriptor is kept, which pin, how it starts. The chip is held in reset
 * from the moment the module loads; the reset call releases it once the bit clock runs.
 */
static struct {
	size_t where;
	unsigned int pin;
	enum gpiod_flags start;
} pin_plan[] = {
	{ offsetof(struct awfxs, miso),  AW_PIN_BUS_MISO,   GPIOD_IN },
	{ offsetof(struct awfxs, mosi),  AW_PIN_BUS_MOSI,   GPIOD_OUT_HIGH },
	{ offsetof(struct awfxs, sck),   AW_PIN_BUS_CLK,    GPIOD_OUT_LOW },
	{ offsetof(struct awfxs, sel),   AW_PIN_BUS_SEL,    GPIOD_OUT_HIGH },
	{ offsetof(struct awfxs, irq),   AW_PIN_CHIP_IRQ,   GPIOD_IN },
	{ offsetof(struct awfxs, reset), AW_PIN_CHIP_RESET, GPIOD_OUT_LOW },
};

/* the four bus pins must be a permutation of 50..53 */
static int apply_bus_pins(void)
{
	unsigned int seen = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(bus_pins); i++) {
		if (bus_pins[i] < AW_PIN_BUS_CLK || bus_pins[i] > AW_PIN_BUS_MOSI || (seen & BIT(bus_pins[i])))
			return -EINVAL;
		seen |= BIT(bus_pins[i]);
		pin_plan[i].pin = bus_pins[i];
	}
	return 0;
}

static struct gpio_desc **slot_of(struct awfxs *z, size_t where)
{
	return (struct gpio_desc **)((char *)z + where);
}

void awfxs_gpio_put(struct awfxs *z)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(pin_plan); i++) {
		struct gpio_desc **slot = slot_of(z, pin_plan[i].where);

		if (*slot)
			gpiochip_free_own_desc(*slot);
		*slot = NULL;
	}
	z->state &= ~AWFXS_STATE_GPIO_OK;
}

int awfxs_gpio_get(struct awfxs *z)
{
	struct gpio_device *gdev;
	size_t i;

	if (apply_bus_pins() || half_ns < 200 || half_ns > 5000)
		return -EINVAL;
	gdev = gpio_device_find_by_label("1000000.pinctrl");
	if (!gdev)
		return -ENODEV;
	z->chip = gpio_device_get_chip(gdev);
	gpio_device_put(gdev);
	if (!z->chip)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(pin_plan); i++) {
		struct gpio_desc *d = gpiochip_request_own_desc(z->chip, pin_plan[i].pin, "awvoice-fxs",
								GPIO_ACTIVE_HIGH, pin_plan[i].start);

		if (IS_ERR(d)) {
			awfxs_gpio_put(z);
			return PTR_ERR(d);
		}
		*slot_of(z, pin_plan[i].where) = d;
	}
	z->state |= AWFXS_STATE_GPIO_OK;
	set_bit(AW_WD_TRIPPED, &z->wd_flags);	/* the chip starts in reset: a reset call comes before any watching */
	return 0;
}

int awfxs_chip_reset(struct awfxs *z, unsigned int low_ms)
{
	if (!(z->state & AWFXS_STATE_GPIO_OK))
		return -ENODEV;
	if (!(z->state & AWFXS_STATE_PCM_RUN))
		return -EAGAIN;		/* reset only while the bit clock runs */
	if (!low_ms || low_ms > 100)
		return -EINVAL;

	clear_bit(AW_WD_TRIPPED, &z->wd_flags);
	gpiod_set_raw_value(z->reset, 0);
	msleep(low_ms);
	/* a watchdog expiry during the pulse keeps the chip in reset */
	if (!test_bit(AW_WD_TRIPPED, &z->wd_flags))
		gpiod_set_raw_value(z->reset, 1);
	msleep(50);
	return test_bit(AW_WD_TRIPPED, &z->wd_flags) ? -EOWNERDEAD : 0;
}

/*
 * SPI mode 0: the clock idles low, data is set before the rising edge, taken on it, and changes after the
 * falling edge. The chip needs every access to be one frame of exactly four bytes (control, address, data,
 * pad); frames of other lengths leave it deaf until the next reset.
 */
static u8 exchange(struct awfxs *z, u8 out)
{
	u8 mask, got = 0;

	for (mask = 0x80; mask; mask >>= 1) {
		gpiod_set_raw_value(z->mosi, !!(out & mask));
		ndelay(half_ns);
		gpiod_set_raw_value(z->sck, 1);
		if (gpiod_get_raw_value(z->miso))
			got |= mask;
		ndelay(half_ns);
		gpiod_set_raw_value(z->sck, 0);
	}
	return got;
}

int awfxs_spi(struct awfxs *z, struct awfxs_spi *x)
{
	unsigned int n;

	if (!(z->state & AWFXS_STATE_GPIO_OK))
		return -ENODEV;
	if (x->len != AWFXS_SPI_FRAME)
		return -EINVAL;
	if (!(x->tx[0] & FIRST_BYTE_IS_READ)) {
		/* a write may start the line voltage or the ringer: only with the watchdog running */
		if (!allow_write || !test_bit(AW_WD_ARMED, &z->wd_flags))
			return -EPERM;
	}

	gpiod_set_raw_value(z->sel, 0);
	ndelay(half_ns);
	for (n = 0; n < x->len; n++)
		x->rx[n] = exchange(z, x->tx[n]);
	ndelay(half_ns);
	gpiod_set_raw_value(z->sel, 1);
	gpiod_set_raw_value(z->mosi, 1);
	ndelay(half_ns);
	return 0;
}
