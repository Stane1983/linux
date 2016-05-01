#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kszaq");
MODULE_DESCRIPTION("Amlogic WiFi power on and SDIO rescan module");

extern void wifi_setup_dt(void);
extern void extern_wifi_set_enable(int);
extern void sdio_reinit(void);

static int run_trigger(const char *val, struct kernel_param *kp)
{
	if (!strncmp(val, "1", 1))
	{
		printk(KERN_INFO "Triggered SDIO WiFi power on and bus rescan.\n");
		extern_wifi_set_enable(1);
		msleep(300);
		sdio_reinit();
	}
	return 0;
}

module_param_call(trigger, run_trigger, NULL, NULL, 0200);

static int __init wifi_dummy_init(void)
{
	wifi_setup_dt();
	msleep(300);
	extern_wifi_set_enable(0);
	return 0;
}

static void __exit wifi_dummy_cleanup(void)
{
    printk(KERN_INFO "Cleaning up module.\n");
}

module_init(wifi_dummy_init);
module_exit(wifi_dummy_cleanup);
