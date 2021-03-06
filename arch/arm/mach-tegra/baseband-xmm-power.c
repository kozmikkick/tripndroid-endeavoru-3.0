/*
 * arch/arm/mach-tegra/baseband-xmm-power.c
 *
 * Copyright (C) 2011 NVIDIA Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wakelock.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/pm_qos_params.h>
#include <linux/htc_hostdbg.h>
#include <linux/regulator/consumer.h>
#include <mach/usb_phy.h>
#include <mach/board_htc.h>

#include "board-endeavoru.h"
#include "gpio-names.h"
#include "baseband-xmm-power.h"
#include "board.h"
#include "devices.h"

MODULE_LICENSE("GPL");

unsigned long modem_ver = XMM_MODEM_VER_1121;

#define MODULE_NAME "[XMM_v10]"

EXPORT_SYMBOL(modem_ver);

unsigned long modem_flash;
EXPORT_SYMBOL(modem_flash);

unsigned long modem_pm = 1;
EXPORT_SYMBOL(modem_pm);

unsigned long enum_delay_ms = 1000; /* ignored if !modem_flash */

module_param(modem_ver, ulong, 0644);
MODULE_PARM_DESC(modem_ver,
	"baseband xmm power - modem software version");
module_param(modem_flash, ulong, 0644);
MODULE_PARM_DESC(modem_flash,
	"baseband xmm power - modem flash (1 = flash, 0 = flashless)");
module_param(modem_pm, ulong, 0644);
MODULE_PARM_DESC(modem_pm,
	"baseband xmm power - modem power management (1 = pm, 0 = no pm)");
module_param(enum_delay_ms, ulong, 0644);
MODULE_PARM_DESC(enum_delay_ms,
	"baseband xmm power - delay in ms between modem on and enumeration");

static struct usb_device_id xmm_pm_ids[] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID),
	.driver_info = 0 },
	{}
};

//for power on modem
static struct gpio tegra_baseband_gpios[] = {
	{ -1, GPIOF_OUT_INIT_LOW,  "BB_RSTn" },
	{ -1, GPIOF_OUT_INIT_LOW,  "BB_ON"   },
	{ -1, GPIOF_OUT_INIT_LOW,  "IPC_BB_WAKE" },
	{ -1, GPIOF_IN,            "IPC_AP_WAKE" },
	{ -1, GPIOF_OUT_INIT_HIGH, "IPC_HSIC_ACTIVE" },
	{ -1, GPIOF_IN,            "IPC_HSIC_SUS_REQ" },
#ifdef BB_XMM_OEM1
	{ BB_VDD_EN, GPIOF_OUT_INIT_LOW, "BB_VDD_EN" },
#endif
};


//for power consumation , power off modem
static struct gpio tegra_baseband_gpios_power_off_modem[] = {
	{ -1, GPIOF_OUT_INIT_LOW,  "BB_RSTn" },
	{ -1, GPIOF_OUT_INIT_LOW,  "BB_ON"   },
	{ -1, GPIOF_OUT_INIT_LOW,  "IPC_BB_WAKE" },
	{ -1, GPIOF_OUT_INIT_LOW,   "IPC_AP_WAKE" },
	{ -1, GPIOF_OUT_INIT_LOW, "IPC_HSIC_ACTIVE" },
	{ -1, GPIOF_IN,            "IPC_HSIC_SUS_REQ" },
#ifdef BB_XMM_OEM1
	{ BB_VDD_EN, GPIOF_OUT_INIT_LOW, "BB_VDD_EN" },
#endif
};

static enum {
	IPC_AP_WAKE_UNINIT,
	IPC_AP_WAKE_IRQ_READY,
	IPC_AP_WAKE_INIT1,
	IPC_AP_WAKE_INIT2,
	IPC_AP_WAKE_L,
	IPC_AP_WAKE_H,
} ipc_ap_wake_state = IPC_AP_WAKE_INIT2;

enum baseband_xmm_powerstate_t baseband_xmm_powerstate;
static struct workqueue_struct *workqueue;
static struct work_struct init1_work;
static struct work_struct init2_work;
static struct work_struct L2_resume_work;
static struct work_struct autopm_resume_work;
static struct baseband_power_platform_data *baseband_power_driver_data;
static bool register_hsic_device;
static struct wake_lock wakelock;
static struct usb_device *usbdev;
static bool CP_initiated_L2toL0;
static bool modem_power_on;
static bool first_time = true;
static int power_onoff;
static void baseband_xmm_power_L2_resume(void);
static DEFINE_MUTEX(baseband_xmm_onoff_lock);
static int baseband_xmm_power_driver_handle_resume(
			struct baseband_power_platform_data *data);

static bool wakeup_pending;
static int uart_pin_pull_state=1; // 1 for UART, 0 for GPIO
static bool modem_sleep_flag = false;
static spinlock_t xmm_lock;
static bool system_suspending;

static int reenable_autosuspend; //ICS only
static int htcpcbid=0;

static struct workqueue_struct *workqueue_susp;

//static struct delayed_work pm_qos_work;

int gpio_config_only_one(unsigned gpio, unsigned long flags, const char *label)
{
	int err=0;


	if (flags & GPIOF_DIR_IN)
		err = gpio_direction_input(gpio);
	else
		err = gpio_direction_output(gpio,
				(flags & GPIOF_INIT_HIGH) ? 1 : 0);

	return err;
}

int gpio_config_only_array(struct gpio *array, size_t num)
{
	int i, err=0;

	for (i = 0; i < num; i++, array++) {
		err = gpio_config_only_one(array->gpio, array->flags, array->label);
		if (err)
			goto err_free;
	}
	return 0;

err_free:

	return err;
}


int gpio_request_only_one(unsigned gpio,const char *label)
{
	int err=0;

	err = gpio_request(gpio, label);
	return err;
}


int gpio_request_only_array(struct gpio *array, size_t num)
{
	int i, err=0;

	for (i = 0; i < num; i++, array++) {
		err = gpio_request_only_one(array->gpio, array->label);
		if (err)
			goto err_free;
	}
	return 0;

err_free:
	while (i--)
		gpio_free((--array)->gpio);
	return err;
}


static int gpio_o_l_uart(int gpio, char* name)
{
	int ret=0;
	pr_debug(MODULE_NAME "%s ,name=%s gpio=%d\n", __func__,name,gpio);
	ret = gpio_direction_output(gpio, 0);
	if (ret < 0) {
		pr_err(" %s: gpio_direction_output failed %d\n", __func__, ret);
		gpio_free(gpio);
	}
	tegra_gpio_enable(gpio);
	gpio_export(gpio, true);
	
	return ret;
}

void modem_on_for_uart_config(void)
{
	pr_debug(MODULE_NAME "%s ,first_time=%s uart_pin_pull_low=%d\n", __func__,first_time?"true":"false",uart_pin_pull_state);
	if(uart_pin_pull_state == 0) {
	//if uart pin pull low, then we put back to normal
	pr_debug(MODULE_NAME "%s tegra_gpio_disable for UART\n", __func__);
	tegra_gpio_disable(TEGRA_GPIO_PJ7);
	tegra_gpio_disable(TEGRA_GPIO_PK7);
	tegra_gpio_disable(TEGRA_GPIO_PB0);
	tegra_gpio_disable(TEGRA_GPIO_PB1);
	uart_pin_pull_state=1;//set back to UART
	}
}

int modem_off_for_uart_config(void)
{
	int err = 0;

	pr_debug(MODULE_NAME "%s uart_pin_pull_low=%d\n", __func__,uart_pin_pull_state);
	if(uart_pin_pull_state==1){
	//if uart pin not pull low yet, then we pull them low+enable
	err=gpio_o_l_uart(TEGRA_GPIO_PJ7, "IMC_UART_TX");
	err=gpio_o_l_uart(TEGRA_GPIO_PK7, "IMC_UART_RTS");
	err=gpio_o_l_uart(TEGRA_GPIO_PB0  ,"IMC_UART_RX");
	err=gpio_o_l_uart(TEGRA_GPIO_PB1, "IMC_UART_CTS");
	uart_pin_pull_state=0;//change to gpio
	}

	return err;
}

int modem_off_for_usb_config(struct gpio *array, size_t num)
{
	int err = 0;
	pr_debug(MODULE_NAME "%s 1219_01\n", __func__);
	
	err = gpio_config_only_array(tegra_baseband_gpios_power_off_modem,
		ARRAY_SIZE(tegra_baseband_gpios_power_off_modem));
	if (err < 0) {
		pr_err("%s - gpio_config_array gpio(s) for modem off failed\n", __func__);
		return -ENODEV;
	}
	return err;
}

int modem_on_for_usb_config(struct gpio *array, size_t num)
{
	int err = 0;
	pr_debug(MODULE_NAME "%s \n", __func__);
	
	err = gpio_config_only_array(tegra_baseband_gpios,
		ARRAY_SIZE(tegra_baseband_gpios));
	if (err < 0) {
		pr_err("%s - gpio_config_array gpio(s) for modem off failed\n", __func__);
		return -ENODEV;
	}

	return err;

}

int config_gpio_for_power_off(void)
{
	int err=0;

	pr_debug(MODULE_NAME "%s for power consumation 4st \n", __func__);

		/* config  baseband gpio(s) for modem off */
		err = modem_off_for_usb_config(tegra_baseband_gpios_power_off_modem,
			ARRAY_SIZE(tegra_baseband_gpios_power_off_modem));
		if (err < 0) {
			pr_err("%s - gpio_config_array gpio(s) for modem off failed\n", __func__);
			return -ENODEV;
		}

		/* config  uart gpio(s) for modem off */
		err=modem_off_for_uart_config();
		if (err < 0) {
			pr_err("%s - modem_off_for_uart_config gpio(s)\n", __func__);
			return -ENODEV;
		}


	return err;
}

int config_gpio_for_power_on(void)
{
	int err=0;

	pr_debug(MODULE_NAME "%s for power consumation 4st \n", __func__);

		/* config  baseband gpio(s) for modem off */

		err = modem_on_for_usb_config(tegra_baseband_gpios,
			ARRAY_SIZE(tegra_baseband_gpios));
		if (err < 0) {
			pr_err("%s - gpio_config_array gpio(s) for modem off failed\n", __func__);
			return -ENODEV;
		}

		/* config  uart gpio(s) for modem off */
		modem_on_for_uart_config();

	return err;
}

extern void platfrom_set_flight_mode_onoff(bool mode_on);

static int baseband_modem_power_on(struct baseband_power_platform_data *data)
{
	/* HTC: called in atomic context */
	int i=0;

	/* reset / power on sequence */
	gpio_set_value(BB_VDD_EN, 1); /* give modem power */
	mdelay(20);

	for (i = 0; i < 7; i++)
		udelay(1000);

	gpio_set_value(data->modem.xmm.bb_rst, 1);
	mdelay(1);
	gpio_set_value(data->modem.xmm.bb_on, 1);
	udelay(40);
	gpio_set_value(data->modem.xmm.bb_on, 0);
	/* Device enumeration should happen in 1 sec however in any case
	 * we want to request it back to normal so schedule work to restore
	 * CPU freq after 2 seconds */
//	schedule_delayed_work(&pm_qos_work, msecs_to_jiffies(2000));

	return 0;
}

static int baseband_xmm_power_on(struct platform_device *device)
{
	struct baseband_power_platform_data *data
		= (struct baseband_power_platform_data *)
			device->dev.platform_data;
	int ret;

	pr_debug("%s {\n", __func__);

	/* check for platform data */
	if (!data) {
		pr_err("%s: !pdata\n", __func__);
		return -EINVAL;
	}
	if (baseband_xmm_powerstate != BBXMM_PS_UNINIT)
		return -EINVAL;

	/*set Radio fatal Pin to Iput*/
//	ret=gpio_direction_input(TEGRA_GPIO_PN2);
//	if (ret < 0)
//				pr_err("%s: set Radio fatal Pin to Iput error\n", __func__);

	/*set BB2AP_SUSPEND_REQ Pin (TEGRA_GPIO_PV0) to Iput*/
//	ret=gpio_direction_input(TEGRA_GPIO_PV0);
//	if (ret < 0)
//				pr_err("%s: set BB2AP_SUSPEND_REQ Pin to Iput error\n", __func__);

	/*config back the uart pin*/
	config_gpio_for_power_on();

	/* reset the state machine */
	baseband_xmm_powerstate = BBXMM_PS_INIT;
	modem_sleep_flag = false;
	ipc_ap_wake_state = IPC_AP_WAKE_INIT2;

	/* register usb host controller */
	if (!modem_flash) {
		pr_debug("%s - %d\n", __func__, __LINE__);
		/* register usb host controller only once */
		if (register_hsic_device) {
			pr_debug("%s: register usb host controller\n",
				__func__);
			modem_power_on = true;
			if (data->hsic_register)
				data->modem.xmm.hsic_device = data->hsic_register();
			else
				pr_err("%s: hsic_register is missing\n",
					__func__);
			register_hsic_device = false;
		} else {
			/* register usb host controller */
			if (data->hsic_register)
				data->modem.xmm.hsic_device = data->hsic_register();
			/* turn on modem */
			pr_debug("%s call baseband_modem_power_on\n", __func__);
			baseband_modem_power_on(data);
		}
	}
	ret = enable_irq_wake(gpio_to_irq(data->modem.xmm.ipc_ap_wake));
	if (ret < 0)
		pr_err("%s: enable_irq_wake error\n", __func__);
	pr_debug("%s }\n", __func__);

	return 0;
}

static int baseband_xmm_power_off(struct platform_device *device)
{
	struct baseband_power_platform_data *data
		= (struct baseband_power_platform_data *)
			device->dev.platform_data;

	int ret;
	unsigned long flags;

	pr_debug("%s {\n", __func__);

	if (baseband_xmm_powerstate == BBXMM_PS_UNINIT)
		return -EINVAL;
	/* check for device / platform data */
	if (!device) {
		pr_err("%s: !device\n", __func__);
		return -EINVAL;
	}
	if (!data) {
		pr_err("%s: !pdata\n", __func__);
		return -EINVAL;
	}
	
	ipc_ap_wake_state = IPC_AP_WAKE_UNINIT;
	ret = disable_irq_wake(gpio_to_irq(data->modem.xmm.ipc_ap_wake));
	if (ret < 0)
		pr_err("%s: disable_irq_wake error\n", __func__);

	/* unregister usb host controller */
	if (data->hsic_unregister)
		data->hsic_unregister(data->modem.xmm.hsic_device);
	else
		pr_err("%s: hsic_unregister is missing\n", __func__);

	/* set IPC_HSIC_ACTIVE low */
	gpio_set_value(data->modem.xmm.ipc_hsic_active, 0);

	/* wait 20 ms */
	mdelay(20);

	/* drive bb_rst low */
	gpio_set_value(data->modem.xmm.bb_rst, 0);
	mdelay(1);

	baseband_xmm_powerstate = BBXMM_PS_UNINIT;
	modem_sleep_flag = false;
	CP_initiated_L2toL0 = false;
	spin_lock_irqsave(&xmm_lock, flags);
	wakeup_pending = false;
	system_suspending = false;
	spin_unlock_irqrestore(&xmm_lock, flags);
	/* start registration process once again on xmm on */
	register_hsic_device = true;

	config_gpio_for_power_off();
	pr_debug("%s }\n", __func__);

	return 0;
}

static ssize_t baseband_xmm_onoff(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	int pwr;
	int size;
	struct platform_device *device = to_platform_device(dev);

	mutex_lock(&baseband_xmm_onoff_lock);

	pr_debug("%s\n", __func__);

	/* check input */
	if (buf == NULL) {
		pr_err("%s: buf NULL\n", __func__);
		mutex_unlock(&baseband_xmm_onoff_lock);
		return -EINVAL;
	}
	pr_debug("%s: count=%d\n", __func__, count);

	/* parse input */
	size = sscanf(buf, "%d", &pwr);
	if (size != 1) {
		pr_err("%s: size=%d -EINVAL\n", __func__, size);
		mutex_unlock(&baseband_xmm_onoff_lock);
		return -EINVAL;
	}

	if (power_onoff == pwr) {
		pr_err("%s: Ignored, due to same CP power state(%d)\n",
						__func__, power_onoff);
		mutex_unlock(&baseband_xmm_onoff_lock);
		return -EINVAL;
	}
	power_onoff = pwr;
	pr_debug("%s power_onoff=%d\n", __func__, power_onoff);

	if (power_onoff == 0)
		baseband_xmm_power_off(device);
	else if (power_onoff == 1)
		baseband_xmm_power_on(device);

	mutex_unlock(&baseband_xmm_onoff_lock);

	return count;
}
#if 0
static void pm_qos_worker(struct work_struct *work)
{
	pr_debug("%s - pm qos CPU back to normal\n", __func__);
	pm_qos_update_request(&boost_cpu_freq_req,
			(s32)PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
}
#endif
static DEVICE_ATTR(xmm_onoff, S_IRUSR | S_IWUSR | S_IRGRP,
		NULL, baseband_xmm_onoff);


void baseband_xmm_set_power_status(unsigned int status)
{
	struct baseband_power_platform_data *data = baseband_power_driver_data;
	int value = 0;
	unsigned long flags;

	if (baseband_xmm_powerstate == status)
		return;
	pr_debug("%s\n", __func__);
	switch (status) {
	case BBXMM_PS_L0:
		if (modem_sleep_flag) {
			pr_info("%s Resume from L3 without calling resume"
						"function\n",  __func__);
			baseband_xmm_power_driver_handle_resume(data);
		}
		pr_info("L0\n");
		baseband_xmm_powerstate = status;
		if (!wake_lock_active(&wakelock))
			wake_lock(&wakelock);
		value = gpio_get_value(data->modem.xmm.ipc_hsic_active);
		pr_debug("before L0 ipc_hsic_active=%d\n", value);
		if (!value) {
			pr_debug("before L0 gpio set ipc_hsic_active=1 ->\n");
			gpio_set_value(data->modem.xmm.ipc_hsic_active, 1);
		}
		if (modem_power_on) {
			modem_power_on = false;
			baseband_modem_power_on(data);
		}
		pr_debug("gpio host active high->\n");
		break;
	case BBXMM_PS_L2:
		pr_info("L2\n");
		baseband_xmm_powerstate = status;
		spin_lock_irqsave(&xmm_lock, flags);
		if (wakeup_pending) {
			spin_unlock_irqrestore(&xmm_lock, flags);
			baseband_xmm_power_L2_resume();
		 } else {
			spin_unlock_irqrestore(&xmm_lock, flags);
			if (wake_lock_active(&wakelock))
				wake_unlock(&wakelock);
			modem_sleep_flag = true;
		}
		break;
	case BBXMM_PS_L3:
		if (baseband_xmm_powerstate == BBXMM_PS_L2TOL0) {
			if (!data->modem.xmm.ipc_ap_wake) {
				spin_lock_irqsave(&xmm_lock, flags);
				wakeup_pending = true;
				spin_unlock_irqrestore(&xmm_lock, flags);
				pr_info("%s: L2 race condition-CP wakeup"
						" pending\n", __func__);
			}
		}
		pr_info("L3\n");
		/* system is going to suspend */
		if (baseband_xmm_powerstate == BBXMM_PS_L2)
			config_gpio_for_power_off();

		baseband_xmm_powerstate = status;
		spin_lock_irqsave(&xmm_lock, flags);
		system_suspending = false;
		spin_unlock_irqrestore(&xmm_lock, flags);
		if (wake_lock_active(&wakelock)) {
			pr_info("%s: releasing wakelock before L3\n",
				__func__);
			wake_unlock(&wakelock);
		}
		gpio_set_value(data->modem.xmm.ipc_hsic_active, 0);
		pr_debug("gpio host active low->\n");
		break;
	case BBXMM_PS_L2TOL0:
		spin_lock_irqsave(&xmm_lock, flags);
		system_suspending = false;
		wakeup_pending = false;
		spin_unlock_irqrestore(&xmm_lock, flags);
		/* do this only from L2 state */
		if (baseband_xmm_powerstate == BBXMM_PS_L2) {
			baseband_xmm_powerstate = status;
			pr_debug("BB XMM POWER STATE = %d\n", status);
			baseband_xmm_power_L2_resume();
		}
		baseband_xmm_powerstate = status;
		break;
	case BBXMM_PS_L3TOL0:
		/* poweron rail for L3 -> L0 (system resume) */
		pr_debug("L3 -> L0, turning on power rail.\n");
		config_gpio_for_power_on();
		baseband_xmm_powerstate = status;
		break;
	default:
		baseband_xmm_powerstate = status;
		break;
	}
	pr_debug("BB XMM POWER STATE = %d\n", status);
}
EXPORT_SYMBOL_GPL(baseband_xmm_set_power_status);

irqreturn_t baseband_xmm_power_ipc_ap_wake_irq(int irq, void *dev_id)
{
	struct baseband_power_platform_data *data = baseband_power_driver_data;
	int value;
	
	value = gpio_get_value(data->modem.xmm.ipc_ap_wake);
	pr_debug("%s g(%d), wake_st(%d)\n", __func__, value, ipc_ap_wake_state);

	/* modem initialization/bootup part*/
	if (unlikely(ipc_ap_wake_state < IPC_AP_WAKE_IRQ_READY)) {
		pr_err("%s - spurious irq\n", __func__);
		return IRQ_HANDLED;
	} else if (ipc_ap_wake_state == IPC_AP_WAKE_IRQ_READY) {
		if (!value) {
			pr_debug("%s - IPC_AP_WAKE_INIT1"
					" - got falling edge\n", __func__);
			/* go to IPC_AP_WAKE_INIT1 state */
			ipc_ap_wake_state = IPC_AP_WAKE_INIT1;
			/* queue work */
			queue_work(workqueue, &init1_work);
		} else
			pr_debug("%s - IPC_AP_WAKE_INIT1"
				" - wait for falling edge\n", __func__);
		return IRQ_HANDLED;
	} else if (ipc_ap_wake_state == IPC_AP_WAKE_INIT1) {
		if (!value) {
			pr_debug("%s - IPC_AP_WAKE_INIT2"
				" - wait for rising edge\n", __func__);
		} else {
			pr_debug("%s - IPC_AP_WAKE_INIT2"
					" - got rising edge\n",	__func__);
			/* go to IPC_AP_WAKE_INIT2 state */
			ipc_ap_wake_state = IPC_AP_WAKE_INIT2;
			/* queue work */
			queue_work(workqueue, &init2_work);
		}
		return IRQ_HANDLED;
	}

	/* modem wakeup part */
	if (!value) {
		pr_debug("%s - falling\n", __func__);
		/* First check it a CP ack or CP wake  */
		value = gpio_get_value(data->modem.xmm.ipc_bb_wake);
		if (value) {
			pr_debug("cp ack for bb_wake\n");
			ipc_ap_wake_state = IPC_AP_WAKE_L;
			return IRQ_HANDLED;
		}
		spin_lock(&xmm_lock);
		wakeup_pending = true;
		if (system_suspending) {
			spin_unlock(&xmm_lock);
			pr_info("Set wakeup_pending = 1 in system_"
					" suspending!!!\n");
		} else {
			if ((baseband_xmm_powerstate == BBXMM_PS_L3) ||
				(baseband_xmm_powerstate == BBXMM_PS_L3TOL0)) {
				spin_unlock(&xmm_lock);
				pr_info(" CP L3 -> L0\n");
			} else if (baseband_xmm_powerstate == BBXMM_PS_L2) {
				CP_initiated_L2toL0 = true;
				spin_unlock(&xmm_lock);
				baseband_xmm_set_power_status(BBXMM_PS_L2TOL0);
			} else {
				CP_initiated_L2toL0 = true;
				spin_unlock(&xmm_lock);
			}
		}
		/* save gpio state */
		ipc_ap_wake_state = IPC_AP_WAKE_L;
		} else {
		pr_debug("%s - rising\n", __func__);
		value = gpio_get_value(data->modem.xmm.ipc_hsic_active);
			if (!value) {
			pr_info("host active low: ignore request\n");
			ipc_ap_wake_state = IPC_AP_WAKE_H;
			return IRQ_HANDLED;
		}
		value = gpio_get_value(data->modem.xmm.ipc_bb_wake);
		if (value) {
			/* Clear the slave wakeup request */
			gpio_set_value(data->modem.xmm.ipc_bb_wake, 0);
			pr_debug("gpio slave wakeup done ->\n");
		}
		if (reenable_autosuspend && usbdev) {
			reenable_autosuspend = false;
			queue_work(workqueue, &autopm_resume_work);
		}
		modem_sleep_flag = false;
		baseband_xmm_set_power_status(BBXMM_PS_L0);
		/* save gpio state */
		ipc_ap_wake_state = IPC_AP_WAKE_H;
	}

	return IRQ_HANDLED;
}
EXPORT_SYMBOL(baseband_xmm_power_ipc_ap_wake_irq);

static void baseband_xmm_power_init1_work(struct work_struct *work)
{
	int value;

	pr_debug("%s {\n", __func__);

	/* check if IPC_HSIC_ACTIVE high */
	value = gpio_get_value(baseband_power_driver_data->
		modem.xmm.ipc_hsic_active);
	if (value != 1) {
		pr_err("%s - expected IPC_HSIC_ACTIVE high!\n", __func__);
		return;
	}

	/* wait 100 ms */
	msleep(100);

	/* set IPC_HSIC_ACTIVE low */
	gpio_set_value(baseband_power_driver_data->
		modem.xmm.ipc_hsic_active, 0);

	/* wait 10 ms */
	msleep(10);

	/* set IPC_HSIC_ACTIVE high */
	gpio_set_value(baseband_power_driver_data->
		modem.xmm.ipc_hsic_active, 1);

	/* wait 20 ms */
	msleep(20);

#ifdef BB_XMM_OEM1
	/* set IPC_HSIC_ACTIVE low */
	gpio_set_value(baseband_power_driver_data->
		modem.xmm.ipc_hsic_active, 0);
	printk(KERN_INFO"%s merge need check set IPC_HSIC_ACTIVE low\n", __func__);
#endif /* BB_XMM_OEM1 */

	pr_debug("%s }\n", __func__);
}

static void baseband_xmm_power_init2_work(struct work_struct *work)
{
	struct baseband_power_platform_data *data = baseband_power_driver_data;

	pr_debug("%s\n", __func__);

	/* check input */
	if (!data)
		return;

	/* register usb host controller only once */
	if (register_hsic_device) {
		if (data->hsic_register)
			data->modem.xmm.hsic_device = data->hsic_register();
		else
			pr_err("%s: hsic_register is missing\n", __func__);
		register_hsic_device = false;
	}
}

static void xmm_power_autopm_resume(struct work_struct *work)
{
	struct usb_interface *intf;

	pr_debug("%s\n", __func__);
	if (usbdev) {
		usb_lock_device(usbdev);
		intf = usb_ifnum_to_if(usbdev, 0);
		if (!intf) {
			usb_unlock_device(usbdev);
			return;
		}
		if (usb_autopm_get_interface(intf) == 0)
			usb_autopm_put_interface(intf);
		usb_unlock_device(usbdev);
	}
}

/* Do the work for AP/CP initiated L2->L0 */
static void baseband_xmm_power_L2_resume(void)
{
	struct baseband_power_platform_data *data = baseband_power_driver_data;
	int value;
	int delay = 10000; /* maxmum delay in msec */
	unsigned long flags;


	pr_debug("%s\n", __func__);

	if (!baseband_power_driver_data)
		return;
	/* claim the wakelock here to avoid any system suspend */
	if (!wake_lock_active(&wakelock))
		wake_lock(&wakelock);

	modem_sleep_flag = false;
	spin_lock_irqsave(&xmm_lock, flags);
	wakeup_pending = false;
	spin_unlock_irqrestore(&xmm_lock, flags);

	if (CP_initiated_L2toL0)  {
		pr_info("CP L2->L0\n");
		CP_initiated_L2toL0 = false;
		queue_work(workqueue, &L2_resume_work);
	} else {
		/* set the slave wakeup request */
		pr_info("AP L2->L0\n");
		value = gpio_get_value(data->modem.xmm.ipc_ap_wake);
		if (value) {
			pr_debug("waiting for host wakeup from CP...\n");
			/* wake bb */
			gpio_set_value(data->modem.xmm.ipc_bb_wake, 1);
			do {
				mdelay(1);
				value = gpio_get_value(
					data->modem.xmm.ipc_ap_wake);
				delay--;
			} while ((value) && (delay));
			if (delay)
				pr_debug("Get gpio host wakeup low <-\n");
			else
				pr_info("!!AP L2->L0 Failed\n");
		} else {
			pr_info("CP already ready\n");
		}
	}
}

/* Do the work for CP initiated L2->L0 */
static void baseband_xmm_power_L2_resume_work(struct work_struct *work)
{
	struct usb_interface *intf;

	pr_info("%s {\n", __func__);

	if (!usbdev) {
		pr_info("%s - !usbdev\n", __func__);
		return;
	}

	usb_lock_device(usbdev);
	intf = usb_ifnum_to_if(usbdev, 0);
	if (usb_autopm_get_interface(intf) == 0)
		usb_autopm_put_interface(intf);
	usb_unlock_device(usbdev);

	pr_info("} %s\n", __func__);
}

static void baseband_xmm_power_reset_on(void)
{
	/* reset / power on sequence */
	msleep(40);
	gpio_set_value(baseband_power_driver_data->modem.xmm.bb_rst, 1);
	msleep(1);
	gpio_set_value(baseband_power_driver_data->modem.xmm.bb_on, 1);
	udelay(40);
	gpio_set_value(baseband_power_driver_data->modem.xmm.bb_on, 0);
}

static struct baseband_xmm_power_work_t *baseband_xmm_power_work;

static void baseband_xmm_power_work_func(struct work_struct *work)
{
	struct baseband_xmm_power_work_t *bbxmm_work
		= (struct baseband_xmm_power_work_t *) work;

	pr_debug("%s - work->sate=%d\n", __func__, bbxmm_work->state);

	switch (bbxmm_work->state) {
	case BBXMM_WORK_UNINIT:
		pr_debug("BBXMM_WORK_UNINIT\n");
		break;
	case BBXMM_WORK_INIT:
		pr_debug("BBXMM_WORK_INIT\n");
		/* go to next state */
		bbxmm_work->state = (modem_flash && !modem_pm)
			? BBXMM_WORK_INIT_FLASH_STEP1
			: (modem_flash && modem_pm)
			? BBXMM_WORK_INIT_FLASH_PM_STEP1
			: (!modem_flash && modem_pm)
			? BBXMM_WORK_INIT_FLASHLESS_PM_STEP1
			: BBXMM_WORK_UNINIT;
		pr_debug("Go to next state %d\n", bbxmm_work->state);
		queue_work(workqueue, work);
		break;
	case BBXMM_WORK_INIT_FLASH_STEP1:
		pr_debug("BBXMM_WORK_INIT_FLASH_STEP1\n");
		/* register usb host controller */
		pr_debug("%s: register usb host controller\n", __func__);
		if (baseband_power_driver_data->hsic_register)
			baseband_power_driver_data->modem.xmm.hsic_device =
				baseband_power_driver_data->hsic_register();
		else
			pr_err("%s: hsic_register is missing\n", __func__);
		break;
	case BBXMM_WORK_INIT_FLASH_PM_STEP1:
		pr_debug("BBXMM_WORK_INIT_FLASH_PM_STEP1\n");
		pr_debug("%s: ipc_hsic_active -> 0\n", __func__);
		gpio_set_value(baseband_power_driver_data->modem.xmm.ipc_hsic_active, 1);
		/* reset / power on sequence */
		baseband_xmm_power_reset_on();
		/* set power status as on */
		power_onoff = 1;
		gpio_set_value(baseband_power_driver_data->modem.xmm.ipc_hsic_active, 0);

		/* expecting init2 performs register hsic to enumerate modem
		 * software directly.
		 */
		break;
	case BBXMM_WORK_INIT_FLASH_PM_VER_GE_1130_STEP1:
		pr_debug("BBXMM_WORK_INIT_FLASH_PM_VER_GE_1130_STEP1\n");
		break;
	case BBXMM_WORK_INIT_FLASHLESS_PM_STEP1:
		pr_debug("BBXMM_WORK_INIT_FLASHLESS_PM_STEP1\n");
		pr_info("%s: flashless is not supported here\n", __func__);
		break;
	default:
		break;
	}

}

static void baseband_xmm_device_add_handler(struct usb_device *udev)
{
	struct usb_interface *intf = usb_ifnum_to_if(udev, 0);
	const struct usb_device_id *id;

	pr_info("%s \n",__func__);

	if (intf == NULL)
		return;

	id = usb_match_id(intf, xmm_pm_ids);

	if (id) {
		pr_debug("persist_enabled: %u\n", udev->persist_enabled);
		pr_info("Add device %d <%s %s>\n", udev->devnum,
			udev->manufacturer, udev->product);
		usbdev = udev;
		usb_enable_autosuspend(udev);
		pr_info("enable autosuspend\n");
	}
}

static void baseband_xmm_device_remove_handler(struct usb_device *udev)
{
	if (usbdev == udev) {
		pr_info("Remove device %d <%s %s>\n", udev->devnum,
			udev->manufacturer, udev->product);
		usbdev = 0;
	}

}

static int usb_xmm_notify(struct notifier_block *self, unsigned long action,
			void *blob)
{
	switch (action) {
	case USB_DEVICE_ADD:
		baseband_xmm_device_add_handler(blob);
		break;
	case USB_DEVICE_REMOVE:
		baseband_xmm_device_remove_handler(blob);
		break;
	}

	return NOTIFY_OK;
}


static struct notifier_block usb_xmm_nb = {
	.notifier_call = usb_xmm_notify,
};

static int baseband_xmm_power_pm_notifier_event(struct notifier_block *this,
					unsigned long event, void *ptr)
{
    struct baseband_power_platform_data *data = baseband_power_driver_data;
	unsigned long flags;

	if (!data)
		return NOTIFY_DONE;

	pr_debug("%s: event %ld\n", __func__, event);
	switch (event) {
	case PM_SUSPEND_PREPARE:
		pr_debug("%s : PM_SUSPEND_PREPARE\n", __func__);
		if (wake_lock_active(&wakelock)) {
			pr_info("%s: wakelock was active, aborting suspend\n",__func__);
			return NOTIFY_STOP;
		}
		spin_lock_irqsave(&xmm_lock, flags);
		if (wakeup_pending) {
			wakeup_pending = false;
			spin_unlock_irqrestore(&xmm_lock, flags);
			pr_info("%s : XMM busy : Abort system suspend\n",
				 __func__);
			return NOTIFY_STOP;
		}
		system_suspending = true;
		spin_unlock_irqrestore(&xmm_lock, flags);
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		pr_debug("%s : PM_POST_SUSPEND\n", __func__);
		spin_lock_irqsave(&xmm_lock, flags);
		system_suspending = false;
		if (wakeup_pending &&
			(baseband_xmm_powerstate == BBXMM_PS_L2)) {
			wakeup_pending = false;
			spin_unlock_irqrestore(&xmm_lock, flags);
			pr_info("%s : Service Pending CP wakeup\n", __func__);
			CP_initiated_L2toL0 = true;
			baseband_xmm_set_power_status(BBXMM_PS_L2TOL0);
			return NOTIFY_OK;
		}
		wakeup_pending = false;
		spin_unlock_irqrestore(&xmm_lock, flags);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct notifier_block baseband_xmm_power_pm_notifier = {
	.notifier_call = baseband_xmm_power_pm_notifier_event,
};

static int baseband_xmm_power_driver_probe(struct platform_device *device)
{
	struct baseband_power_platform_data *data
		= (struct baseband_power_platform_data *)
			device->dev.platform_data;
	struct device *dev = &device->dev;
	unsigned long flags;
	int err, ret=0;

	pr_debug("%s\n", __func__);
	pr_debug("[XMM] enum_delay_ms=%ld\n", enum_delay_ms);

	 htcpcbid=htc_get_pcbid_info();

	/* check for platform data */
	if (!data)
		return -ENODEV;

	/* check if supported modem */
	if (data->baseband_type != BASEBAND_XMM) {
		pr_err("unsuppported modem\n");
		return -ENODEV;
	}

	/* save platform data */
	baseband_power_driver_data = data;

	/* create device file */
	err = device_create_file(dev, &dev_attr_xmm_onoff);
	if (err < 0) {
		pr_err("%s - device_create_file failed\n", __func__);
		return -ENODEV;
	}

	/* init wake lock */
	wake_lock_init(&wakelock, WAKE_LOCK_SUSPEND, "baseband_xmm_power");

	/* init spin lock */
	spin_lock_init(&xmm_lock);
	/* request baseband gpio(s) */
	tegra_baseband_gpios[0].gpio = data->modem.xmm.bb_rst;
	tegra_baseband_gpios[1].gpio = data->modem.xmm.bb_on;
	tegra_baseband_gpios[2].gpio = data->modem.xmm.ipc_bb_wake;
	tegra_baseband_gpios[3].gpio = data->modem.xmm.ipc_ap_wake;
	tegra_baseband_gpios[4].gpio = data->modem.xmm.ipc_hsic_active;
	tegra_baseband_gpios[5].gpio = data->modem.xmm.ipc_hsic_sus_req;
	err = gpio_request_array(tegra_baseband_gpios,
				ARRAY_SIZE(tegra_baseband_gpios));
	if (err < 0) {
		pr_err("%s - request gpio(s) failed\n", __func__);
		return -ENODEV;
	}

	/* request baseband irq(s) */
	if (modem_flash && modem_pm) {
		pr_debug("%s: request_irq IPC_AP_WAKE_IRQ\n", __func__);
		ipc_ap_wake_state = IPC_AP_WAKE_UNINIT;
		err = request_threaded_irq(
				gpio_to_irq(data->modem.xmm.ipc_ap_wake),
				NULL, baseband_xmm_power_ipc_ap_wake_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				"IPC_AP_WAKE_IRQ", NULL);
		if (err < 0) {
			pr_err("%s - request irq IPC_AP_WAKE_IRQ failed\n",
				__func__);
			return err;
		}
		err = enable_irq_wake(gpio_to_irq(
					data->modem.xmm.ipc_ap_wake));
		if (err < 0)
			pr_err("%s: enable_irq_wake error\n", __func__);

		pr_debug("%s: AP_WAKE_INIT1\n", __func__);
		/* ver 1130 or later starts in INIT1 state */
		ipc_ap_wake_state = IPC_AP_WAKE_INIT1;
	}

	/* init work queue */
	workqueue = create_singlethread_workqueue("baseband_xmm_power_workqueue");
	if (!workqueue) {
		pr_err("cannot create workqueue\n");
		return -ENOMEM;
	}

	baseband_xmm_power_work = (struct baseband_xmm_power_work_t *)
		kmalloc(sizeof(struct baseband_xmm_power_work_t), GFP_KERNEL);
	if (!baseband_xmm_power_work) {
		pr_err("cannot allocate baseband_xmm_power_work\n");
		return -ENOMEM;
	}

	INIT_WORK((struct work_struct *) baseband_xmm_power_work, baseband_xmm_power_work_func);
	baseband_xmm_power_work->state = BBXMM_WORK_INIT;
	queue_work(workqueue, (struct work_struct *) baseband_xmm_power_work);

	/* init work objects */
	INIT_WORK(&init1_work, baseband_xmm_power_init1_work);
	INIT_WORK(&init2_work, baseband_xmm_power_init2_work);
	INIT_WORK(&L2_resume_work, baseband_xmm_power_L2_resume_work);
	INIT_WORK(&autopm_resume_work, xmm_power_autopm_resume);

	/* init state variables */
	register_hsic_device = true;
	CP_initiated_L2toL0 = false;
	baseband_xmm_powerstate = BBXMM_PS_UNINIT;
	spin_lock_irqsave(&xmm_lock, flags);
	wakeup_pending = false;
	system_suspending = false;
	spin_unlock_irqrestore(&xmm_lock, flags);

	usb_register_notify(&usb_xmm_nb);
	register_pm_notifier(&baseband_xmm_power_pm_notifier);

	/*set Radio fatal Pin PN2 to OutPut Low*/
//	ret=gpio_direction_output(TEGRA_GPIO_PN2,0);
//	if (ret < 0)
//		pr_err("%s: set Radio fatal Pin to Output error\n", __func__);

	/*set BB2AP_SUSPEND_REQ Pin (TEGRA_GPIO_PV0) to OutPut Low*/
//	ret=gpio_direction_output(TEGRA_GPIO_PV0,0);
//	if (ret < 0)
//		pr_err("%s: set BB2AP_SUSPEND_REQ Pin to Output error\n", __func__);

	//Request SIM det to wakeup Source wahtever in flight mode on/off
	/*For SIM det*/
	pr_info("%s: request enable irq wake SIM det to wakeup source\n", __func__);
	ret = enable_irq_wake(gpio_to_irq(TEGRA_GPIO_PI5));
	if (ret < 0)
		pr_err("%s: enable_irq_wake error\n", __func__);

	pr_debug("%s }\n", __func__);

	return 0;
}

static int baseband_xmm_power_driver_remove(struct platform_device *device)
{
	struct baseband_power_platform_data *data
		= (struct baseband_power_platform_data *)
			device->dev.platform_data;
	struct device *dev = &device->dev;

	pr_debug("%s\n", __func__);

	/* check for platform data */
	if (!data)
		return 0;

	unregister_pm_notifier(&baseband_xmm_power_pm_notifier);
	usb_unregister_notify(&usb_xmm_nb);

	/* free work structure */
	kfree(baseband_xmm_power_work);
	baseband_xmm_power_work = (struct baseband_xmm_power_work_t *) 0;

	/* free baseband irq(s) */
	if (modem_flash && modem_pm) {
		free_irq(gpio_to_irq(baseband_power_driver_data
			->modem.xmm.ipc_ap_wake), NULL);
	}

	/* free baseband gpio(s) */
	gpio_free_array(tegra_baseband_gpios,
		ARRAY_SIZE(tegra_baseband_gpios));

	/* destroy wake lock */
	wake_lock_destroy(&wakelock);

	/* delete device file */
	device_remove_file(dev, &dev_attr_xmm_onoff);

	 /* destroy wake lock */
	  destroy_workqueue(workqueue_susp);
	  destroy_workqueue(workqueue);

	/* unregister usb host controller */
	if (data->hsic_unregister)
		data->hsic_unregister(data->modem.xmm.hsic_device);
	else
		pr_err("%s: hsic_unregister is missing\n", __func__);

	return 0;
}

static int baseband_xmm_power_driver_handle_resume(
			struct baseband_power_platform_data *data)
{
	int value;
	int delay = 10000; /* maxmum delay in msec */
	unsigned long flags;

	/* check for platform data */
	if (!data)
		return 0;

	/* check if modem is on */
	if (power_onoff == 0) {
		pr_info("%s - flight mode - nop\n", __func__);
		return 0;
	}

	modem_sleep_flag = false;
	spin_lock_irqsave(&xmm_lock, flags);
	wakeup_pending = false;
	spin_unlock_irqrestore(&xmm_lock, flags);

	/* L3->L0 */
	baseband_xmm_set_power_status(BBXMM_PS_L3TOL0);
	value = gpio_get_value(data->modem.xmm.ipc_ap_wake);
	if (value) {
		pr_info("AP L3 -> L0\n");
		pr_debug("waiting for host wakeup...\n");
		/* wake bb */
		gpio_set_value(data->modem.xmm.ipc_bb_wake, 1);
		do {
			mdelay(1);
			value = gpio_get_value(data->modem.xmm.ipc_ap_wake);
			delay--;
		} while ((value) && (delay));
		if (delay)
			pr_debug("gpio host wakeup low <-\n");
		else
			pr_info("!!AP L3->L0 Failed\n");

	} else {
		pr_info("CP L3 -> L0\n");
	}
	reenable_autosuspend = true;
	
	return 0;

}

#ifdef CONFIG_PM
static int baseband_xmm_power_driver_suspend(struct device *dev)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int baseband_xmm_power_driver_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct baseband_power_platform_data *data
		= (struct baseband_power_platform_data *)
			pdev->dev.platform_data;

	pr_debug("%s\n", __func__);
	baseband_xmm_power_driver_handle_resume(data);

	return 0;
}

static int baseband_xmm_power_suspend_noirq(struct device *dev)
{
	unsigned long flags;

	pr_debug("%s\n", __func__);
	spin_lock_irqsave(&xmm_lock, flags);
	system_suspending = false;
	if (wakeup_pending) {
		wakeup_pending = false;
		spin_unlock_irqrestore(&xmm_lock, flags);
		pr_info("%s:**Abort Suspend: reason CP WAKEUP**\n", __func__);
		return -EBUSY;
	}
	spin_unlock_irqrestore(&xmm_lock, flags);
	return 0;
}

static int baseband_xmm_power_resume_noirq(struct device *dev)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static const struct dev_pm_ops baseband_xmm_power_dev_pm_ops = {
	.suspend_noirq = baseband_xmm_power_suspend_noirq,
	.resume_noirq = baseband_xmm_power_resume_noirq,
	.suspend = baseband_xmm_power_driver_suspend,
	.resume = baseband_xmm_power_driver_resume,
};
#endif

static void xmm_power_driver_shutdown(struct platform_device *device)
{
	struct baseband_power_platform_data *pdata = device->dev.platform_data;

	pr_debug("%s\n", __func__);
	disable_irq(gpio_to_irq(pdata->modem.xmm.ipc_ap_wake));
	/* bb_on is already down, to make sure set 0 again */
	gpio_set_value(pdata->modem.xmm.bb_on, 0);
	gpio_set_value(pdata->modem.xmm.bb_rst, 0);
	return;
}

static struct platform_driver baseband_power_driver = {
	.probe = baseband_xmm_power_driver_probe,
	.remove = baseband_xmm_power_driver_remove,
	.shutdown = xmm_power_driver_shutdown,
	.driver = {
		.name = "baseband_xmm_power",
#ifdef CONFIG_PM
	.pm   = &baseband_xmm_power_dev_pm_ops,
#endif
	},
};

static int __init baseband_xmm_power_init(void)
{
	pr_debug(MODULE_NAME "%s 0x%lx\n", __func__, modem_ver);

	pr_debug(MODULE_NAME "%s - platfrom_set_flight_mode_onoff - on\n", __func__);

	return platform_driver_register(&baseband_power_driver);
}

static void __exit baseband_xmm_power_exit(void)
{
	pr_debug("%s\n", __func__);
	platform_driver_unregister(&baseband_power_driver);
}

module_init(baseband_xmm_power_init)
module_exit(baseband_xmm_power_exit)
