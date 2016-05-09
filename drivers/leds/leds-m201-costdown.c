/*
 * leds-m201-costdown.c -- m201 "costdown" board LED driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/amlogic/aml_gpio_consumer.h>
#include <linux/amlogic/gpio-amlogic.h>
#include <plat/io.h>
#include <mach/io.h>
#include <mach/register.h>

MODULE_AUTHOR("kszaq");
MODULE_DESCRIPTION("M201 \"Costdown\" board front LED driver");
MODULE_LICENSE("GPL");

#define LED_ON 1
#define LED_OFF 0

static void toggle_led(int status)
{
	switch (status)
	{
		case LED_OFF:
			aml_clr_reg32_mask(P_AO_GPIO_O_EN_N, 1 << 18);
			aml_set_reg32_mask(P_AO_GPIO_O_EN_N, 1 << 29);
			break;
		case LED_ON:
			aml_clr_reg32_mask(P_AO_GPIO_O_EN_N, 1 << 29);
			aml_set_reg32_mask(P_AO_GPIO_O_EN_N, 1 << 18);
			break;
	}
}

static void m201_costdown_led_set(struct led_classdev *led_cdev,
		enum led_brightness value)
{
	if (value == LED_OFF)
		toggle_led(LED_OFF);
	else
		toggle_led(LED_ON);
}

static int m201_costdown_led_reboot_notifier(struct notifier_block *nb, unsigned long code, void *unused)
{
	toggle_led(LED_OFF);
	return NOTIFY_DONE;
}

static struct led_classdev m201_costdown_led = {
	.name			= "m201_costdown::front",
	.brightness		= LED_OFF,
	.max_brightness	= LED_ON,
	.brightness_set	= m201_costdown_led_set,
	.flags			= LED_CORE_SUSPENDRESUME,
};

static struct notifier_block m201_costdown_led_reboot_nb = {
	.notifier_call = m201_costdown_led_reboot_notifier,
};

static int __init m201_costdown_led_init(void)
{
	toggle_led(LED_ON);
	m201_costdown_led.brightness = LED_ON;
	register_reboot_notifier(&m201_costdown_led_reboot_nb);
	return led_classdev_register(NULL, &m201_costdown_led);
}

static void __exit m201_costdown_led_exit(void)
{
	led_classdev_unregister(&m201_costdown_led);
	toggle_led(LED_OFF);
	m201_costdown_led.brightness = LED_OFF;
}

module_init(m201_costdown_led_init);
module_exit(m201_costdown_led_exit);
