/*
 * Gadget Driver for Android
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (c) 2009, Code Aurora Forum. All rights reserved.
 * Author: Mike Lockwood <lockwood@android.com>
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

/* #define DEBUG */
/* #define VERBOSE_DEBUG */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/miscdevice.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>

#include <linux/usb/android.h>
#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#if defined(CONFIG_USB_ANDROID_CDC_ECM) || defined(CONFIG_USB_ANDROID_RNDIS)
#include "u_ether.h"
#endif

#include "f_mass_storage.h"
#include "f_adb.h"
#include "u_serial.h"
#ifdef CONFIG_USB_ANDROID_DIAG
#include "f_diag.h"
#endif
#ifdef CONFIG_USB_ANDROID_RMNET
#include "f_rmnet.h"
#endif

#include "gadget_chips.h"

/*
 * Kbuild is not very cooperative with respect to linking separately
 * compiled library objects into one module.  So for now we won't use
 * separate compilation ... ensuring init/exit sections work to shrink
 * the runtime footprint, and giving us at least some parts of what
 * a "gcc --combine ... part1.c part2.c part3.c ... " build would.
 */
#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"
#include "composite.c"

#ifdef CONFIG_USB_AUTO_INSTALL
#include "usb_switch_huawei.h"

static struct delayed_work android_usb_switch_work;
/* destination PID when switch USB composition */
static u16 usb_switch_dest_pid = 0;
/* 0: USB sn is NULL
   1: USB sn is valid
*/
static u16 usb_sn_valid = 0;
#endif  /* CONFIG_USB_AUTO_INSTALL */

MODULE_AUTHOR("Mike Lockwood");
MODULE_DESCRIPTION("Android Composite USB Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

/* product id */
#ifdef CONFIG_USB_AUTO_INSTALL
static u16 product_id =PID_NORMAL; //; PID_ONLY_CDROM
/* < google_usb_drv */	
//static u16 product_id = 0x4E12; /* 0x9018; */
/* google_usb_drv > */	
#else
static u16 product_id = 0x9018;
#endif  /* CONFIG_USB_AUTO_INSTALL */
static int android_set_pid(const char *val, struct kernel_param *kp);
static int android_get_pid(char *buffer, struct kernel_param *kp);
module_param_call(product_id, android_set_pid, android_get_pid,
					&product_id, 0664);
MODULE_PARM_DESC(product_id, "USB device product id");

/* serial number */
#define MAX_SERIAL_LEN 256
static char serial_number[MAX_SERIAL_LEN] = "1234567890ABCDEF";
static struct kparam_string kps = {
	.string			= serial_number,
	.maxlen			= MAX_SERIAL_LEN,
};
static int android_set_sn(const char *kmessage, struct kernel_param *kp);
module_param_call(serial_number, android_set_sn, param_get_string,
						&kps, 0664);
MODULE_PARM_DESC(serial_number, "SerialNumber string");

static const char longname[] = "Gadget Android";
#if defined(CONFIG_USB_ANDROID_CDC_ECM) || defined(CONFIG_USB_ANDROID_RNDIS)
static u8 hostaddr[ETH_ALEN];
#endif

extern usb_switch_stru usb_switch_para;

/* Default vendor ID, overridden by platform data */
#define VENDOR_ID		0x18D1

struct android_dev {
	struct usb_gadget *gadget;
	struct usb_composite_dev *cdev;

	int version;

	int adb_enabled;
	int nluns;
	struct mutex lock;
	struct android_usb_platform_data *pdata;
	unsigned long functions;
};

static struct android_dev *_android_dev;

/* string IDs are assigned dynamically */

#define STRING_MANUFACTURER_IDX		0
#define STRING_PRODUCT_IDX		1
#define STRING_SERIAL_IDX		2

/* String Table */
static struct usb_string strings_dev[] = {
	/* These dummy values should be overridden by platform data */
	[STRING_MANUFACTURER_IDX].s = "Android",
	[STRING_PRODUCT_IDX].s = "Android",
	{  }			/* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_device_descriptor device_desc = {
	.bLength              = sizeof(device_desc),
	.bDescriptorType      = USB_DT_DEVICE,
	.bcdUSB               = __constant_cpu_to_le16(0x0200),
	.bDeviceClass         = USB_CLASS_PER_INTERFACE,
	.idVendor             = __constant_cpu_to_le16(VENDOR_ID),
	.bcdDevice            = __constant_cpu_to_le16(0xffff),
	.bNumConfigurations   = 1,
};

#define android_func_attr(function, index)				\
static ssize_t  show_##function(struct device *dev,			\
		struct device_attribute *attr, char *buf)		\
{									\
									\
	unsigned long n = _android_dev->functions;			\
	int val = 0;							\
									\
	while (n) {							\
		if ((n & 0x0F) == index)				\
			val = 1;					\
		n = n >> 4;						\
	}								\
	return sprintf(buf, "%d\n", val);				\
									\
}									\
									\
static DEVICE_ATTR(function, S_IRUGO, show_##function, NULL);

android_func_attr(adb, ANDROID_ADB);
android_func_attr(mass_storage, ANDROID_MSC);
android_func_attr(acm_modem, ANDROID_ACM_MODEM);
android_func_attr(acm_nmea, ANDROID_ACM_NMEA);
android_func_attr(diag, ANDROID_DIAG);
android_func_attr(modem, ANDROID_GENERIC_MODEM);
android_func_attr(nmea, ANDROID_GENERIC_NMEA);
android_func_attr(cdc_ecm, ANDROID_CDC_ECM);
android_func_attr(rmnet, ANDROID_RMNET);
android_func_attr(rndis, ANDROID_RNDIS);

static struct attribute *android_func_attrs[] = {
	&dev_attr_adb.attr,
	&dev_attr_mass_storage.attr,
	&dev_attr_acm_modem.attr,
	&dev_attr_acm_nmea.attr,
	&dev_attr_diag.attr,
	&dev_attr_modem.attr,
	&dev_attr_nmea.attr,
	&dev_attr_cdc_ecm.attr,
	&dev_attr_rmnet.attr,
	&dev_attr_rndis.attr,
	NULL,
};

static struct attribute_group android_func_attr_grp = {
	.name  = "functions",
	.attrs = android_func_attrs,
};

static int  android_bind_config(struct usb_configuration *c)
{
	struct android_dev *dev = _android_dev;
	int ret = -EINVAL;
	unsigned long n;
	pr_debug("android_bind_config c = 0x%x dev->cdev=0x%x\n",
		(unsigned int) c, (unsigned int) dev->cdev);
	n = dev->functions;
	while (n) {
		switch (n & 0x0F) {
		case ANDROID_ADB:
			ret = adb_function_add(dev->cdev, c);
			if (ret)
				return ret;
			break;
		case ANDROID_MSC:
#ifdef CONFIG_USB_AUTO_INSTALL
            USB_PR("%s, dev->nluns=%d\n", __func__, dev->nluns);
#endif  /* CONFIG_USB_AUTO_INSTALL */
			ret = mass_storage_function_add(dev->cdev, c,
								dev->nluns);
			if (ret)
				return ret;
			break;
		case ANDROID_ACM_MODEM:
			ret = acm_bind_config(c, 0);
			if (ret)
				return ret;
			break;
		case ANDROID_ACM_NMEA:
			ret = acm_bind_config(c, 1);
			if (ret)
				return ret;
			break;
#ifdef CONFIG_USB_ANDROID_DIAG
		case ANDROID_DIAG:
			ret = diag_function_add(c, serial_number);
			if (ret)
				return ret;
			break;
#endif
#ifdef CONFIG_USB_F_SERIAL
		case ANDROID_GENERIC_MODEM:
			ret = gser_bind_config(c, 0);
			if (ret)
				return ret;
			break;
		case ANDROID_GENERIC_NMEA:
			ret = gser_bind_config(c, 1);
			if (ret)
				return ret;
			break;
#endif
#ifdef CONFIG_USB_ANDROID_CDC_ECM
		case ANDROID_CDC_ECM:
			ret = ecm_bind_config(c, hostaddr);
			if (ret)
				return ret;
			break;
#endif
#ifdef CONFIG_USB_ANDROID_RMNET
		case ANDROID_RMNET:
			ret = rmnet_function_add(c);
			if (ret) {
				pr_err("failed to add rmnet function\n");
				return ret;
			}
			break;
#endif
#ifdef CONFIG_USB_ANDROID_RNDIS
		case ANDROID_RNDIS:
			ret = rndis_bind_config(c, hostaddr);
			if (ret)
				return ret;
			break;
#endif
		default:
			ret = -EINVAL;
			return ret;
		}
		n = n >> 4;
	}
	return ret;

}

static int is_usb_networking_on(void)
{
	/* Android user space allows USB tethering only when usb0 is listed
	 * in network interfaces. Setup network link though RNDIS/CDC-ECM
	 * is not listed in current composition. Network links is not setup
	 * for every composition switch. It is setup one time and teared down
	 * during module removal.
	 */
#if defined(CONFIG_USB_ANDROID_CDC_ECM) || defined(CONFIG_USB_ANDROID_RNDIS)
	return 1;
#else
	return 0;
#endif
}

static int get_num_of_serial_ports(void)
{
	struct android_dev *dev = _android_dev;
	unsigned long n = dev->functions;
	unsigned ports = 0;

	while (n) {
		switch (n & 0x0F) {
		case ANDROID_ACM_MODEM:
		case ANDROID_ACM_NMEA:
		case ANDROID_GENERIC_MODEM:
		case ANDROID_GENERIC_NMEA:
			ports++;
		}
		n = n >> 4;
	}

	return ports;
}

static int is_iad_enabled(void)
{
	struct android_dev *dev = _android_dev;
	unsigned long n = dev->functions;

	while (n) {
		switch (n & 0x0F) {
		case ANDROID_ACM_MODEM:
		case ANDROID_ACM_NMEA:
#ifdef CONFIG_USB_ANDROID_RNDIS
		case ANDROID_RNDIS:
#endif
			return 1;
		}
		n = n >> 4;
	}

	return 0;
}

static struct usb_configuration android_config_driver = {
	.label		= "android",
	.bind		= android_bind_config,
	.bConfigurationValue = 1,
	.bMaxPower	= 0xFA, /* 500ma */
};

static int android_unbind(struct usb_composite_dev *cdev)
{
	if (get_num_of_serial_ports())
		gserial_cleanup();

	return 0;
}

static int  android_bind(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = _android_dev;
	struct usb_gadget	*gadget = cdev->gadget;
	int			gcnum;
	int			id;
	int			ret;
	int                     num_ports;

	pr_debug("android_bind\n");

	/* Allocate string descriptor numbers ... note that string
	 * contents can be overridden by the composite_dev glue.
	 */
	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_MANUFACTURER_IDX].id = id;
	device_desc.iManufacturer = id;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_PRODUCT_IDX].id = id;
	device_desc.iProduct = id;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_SERIAL_IDX].id = id;
#ifdef CONFIG_USB_AUTO_INSTALL
    if(0 == usb_sn_valid)
    {
	    device_desc.iSerialNumber = 0;
    }
    else
    {
	    device_desc.iSerialNumber = id;
    }
#else
	device_desc.iSerialNumber = id;
#endif  /* CONFIG_USB_AUTO_INSTALL */

	device_desc.idProduct = __constant_cpu_to_le16(product_id);
	/* Supporting remote wakeup for mass storage only function
	 * does n't make sense, since there are no notifications that
	 * can be sent from mass storage during suspend */
	if ((gadget->ops->wakeup) && (dev->functions != ANDROID_MSC))
		android_config_driver.bmAttributes |= USB_CONFIG_ATT_WAKEUP;
	else
		android_config_driver.bmAttributes &= ~USB_CONFIG_ATT_WAKEUP;

	if (dev->pdata->self_powered && !usb_gadget_set_selfpowered(gadget)) {
		android_config_driver.bmAttributes |= USB_CONFIG_ATT_SELFPOWER;
		android_config_driver.bMaxPower	= 0x32; /* 100 mA */
	}
	dev->cdev = cdev;
	pr_debug("android_bind assigned dev->cdev\n");
	dev->gadget = gadget;

	num_ports = get_num_of_serial_ports();
	if (num_ports) {
		ret = gserial_setup(cdev->gadget, num_ports);
		if (ret < 0)
			return ret;
	}

	if (is_usb_networking_on()) {
		/* set up network link layer */
		ret = gether_setup(cdev->gadget, hostaddr);
		if (ret && (ret != -EBUSY)) {
			gserial_cleanup();
			return ret;
		}
	}

	/* register our configuration */
	ret = usb_add_config(cdev, &android_config_driver);
	if (ret) {
		pr_err("usb_add_config failed\n");
		return ret;
	}

	gcnum = usb_gadget_controller_number(gadget);
	if (gcnum >= 0)
		device_desc.bcdDevice = cpu_to_le16(0x0200 + gcnum);
	else {
		/* gadget zero is so simple (for now, no altsettings) that
		 * it SHOULD NOT have problems with bulk-capable hardware.
		 * so just warn about unrcognized controllers -- don't panic.
		 *
		 * things like configuration and altsetting numbering
		 * can need hardware-specific attention though.
		 */
		pr_warning("%s: controller '%s' not recognized\n",
			longname, gadget->name);
		device_desc.bcdDevice = __constant_cpu_to_le16(0x9999);
	}

	if (is_iad_enabled()) {
        /* new requirement: usb tethering */
        device_desc.bDeviceClass = USB_CLASS_COMM;
        device_desc.bDeviceSubClass      = 0;
        device_desc.bDeviceProtocol      = 0;
	} else {
		device_desc.bDeviceClass         = USB_CLASS_PER_INTERFACE;
		device_desc.bDeviceSubClass      = 0;
		device_desc.bDeviceProtocol      = 0;
	}
	pr_debug("android_bind done\n");
	return 0;
}

static struct usb_composite_driver android_usb_driver = {
	.name		= "android_usb",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.bind		= android_bind,
	.unbind		= android_unbind,
};

struct usb_composition *android_validate_product_id(unsigned short pid)
{
	struct android_dev *dev = _android_dev;
	struct usb_composition *fi;
	int i;

	for (i = 0; i < dev->pdata->num_compositions; i++) {
		fi = &dev->pdata->compositions[i];
		pr_debug("pid=0x%x apid=0x%x\n",
		       fi->product_id, fi->adb_product_id);
		if ((fi->product_id == pid) || (fi->adb_product_id == pid))
			return fi;
	}
	return NULL;
}

#ifdef CONFIG_USB_AUTO_INSTALL
/* 0: for Normal, 1: switch state, not clear req at fsg_function_disable*/
int g_android_switch_flag = 0;
#endif

static int android_switch_composition(u16 pid)
{
	struct android_dev *dev = _android_dev;
	struct usb_composition *func;
	int ret;

#ifdef CONFIG_USB_AUTO_INSTALL
    u16 old_pid = product_id;
    USB_PR("%s\n", __func__);
#endif  /* CONFIG_USB_AUTO_INSTALL */
    
	/* Validate the prodcut id */
	func = android_validate_product_id(pid);
	if (!func) {
		pr_err("%s: invalid product id %x\n", __func__, pid);
		usb_switch_para.inprogress = 0;
		return -EINVAL;
	}

	/* Honour adb users */
	if (dev->adb_enabled) {
		product_id = func->adb_product_id;
		dev->functions = func->adb_functions;
	} else {
		product_id = func->product_id;
		dev->functions = func->functions;
	}

#ifdef CONFIG_USB_AUTO_INSTALL
    USB_PR("dev->adb_enabled=%d, dev->nluns=%d, dev->functions=0x%x\n", 
            dev->adb_enabled, dev->nluns, (unsigned int)dev->functions);
    USB_PR("old_pid=0x%x  ----> product_id=0x%x\n", old_pid, product_id);
	if( product_id == PID_GOOGLE ){
		printk("%s: switch to %x, disable ep reset\n", __func__,  product_id);
		g_android_switch_flag = 1;
	}else{
		printk("%s: Not switch, reset flag to zero, pid=%x\n", __func__, product_id);
		g_android_switch_flag = 0;
	}
#endif  /* CONFIG_USB_AUTO_INSTALL */
    
	usb_composite_unregister(&android_usb_driver);
	ret = usb_composite_register(&android_usb_driver);
	usb_switch_para.inprogress = 0;
	return ret;
}

static ssize_t android_remote_wakeup(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct usb_gadget *gadget = _android_dev->gadget;

	if (!gadget)
		return -ENODEV;

	pr_debug("Calling remote wakeup....\n");
	usb_gadget_wakeup(gadget);

	return count;
}
static DEVICE_ATTR(remote_wakeup, S_IWUSR, 0, android_remote_wakeup);

static struct attribute *android_attrs[] = {
	&dev_attr_remote_wakeup.attr,
	NULL,
};

static struct attribute_group android_attr_grp = {
	.attrs = android_attrs,
};

static int android_set_sn(const char *kmessage, struct kernel_param *kp)
{
	int len = strlen(kmessage);

	if (len >= MAX_SERIAL_LEN) {
		pr_err("serial number string too long\n");
		return -ENOSPC;
	}

	strlcpy(serial_number, kmessage, MAX_SERIAL_LEN);
	/* Chop out \n char as a result of echo */
	if (serial_number[len - 1] == '\n')
		serial_number[len - 1] = '\0';

	return 0;
}

static int android_set_pid(const char *val, struct kernel_param *kp)
{
	int ret = 0;
	unsigned long tmp;

	ret = strict_strtoul(val, 16, &tmp);
	if (ret)
		goto out;

	/* We come here even before android_probe, when product id
	 * is passed via kernel command line.
	 */
	if (!_android_dev) {
		product_id = tmp;
		goto out;
	}

	mutex_lock(&_android_dev->lock);
	ret = android_switch_composition(tmp);
	mutex_unlock(&_android_dev->lock);
out:
	return ret;
}

static int android_get_pid(char *buffer, struct kernel_param *kp)
{
	int ret;

	mutex_lock(&_android_dev->lock);
	ret = sprintf(buffer, "%x", product_id);
	mutex_unlock(&_android_dev->lock);

	return ret;
}

static int adb_enable_open(struct inode *ip, struct file *fp)
{
	struct android_dev *dev = _android_dev;
	int ret = 0;

	mutex_lock(&dev->lock);

	if (dev->adb_enabled)
		goto out;

	dev->adb_enabled = 1;
	pr_debug("enabling adb\n");
#ifdef CONFIG_USB_AUTO_INSTALL
    /* if the user is using the usb tethering, don't switch the composition */
    if((product_id != curr_usb_pid_ptr->wlan_pid)  
        && ((GOOGLE_INDEX == usb_para_info.usb_pid_index)
            ||(product_id == curr_usb_pid_ptr->norm_pid)  
            ||(product_id == curr_usb_pid_ptr->auth_pid))
       )
    {
        USB_PR("%s permitted, product_id = 0x%x\n", __func__, product_id);
		ret = android_switch_composition(product_id);
    }
    else
    {
        USB_PR("%s disallowed, product_id = 0x%x\n", __func__, product_id);
    }
#else
	if (product_id)
		ret = android_switch_composition(product_id);
#endif  /* CONFIG_USB_AUTO_INSTALL */
out:
	mutex_unlock(&dev->lock);

	return ret;
}

static int adb_enable_release(struct inode *ip, struct file *fp)
{
	struct android_dev *dev = _android_dev;
	int ret = 0;

	mutex_lock(&dev->lock);

	if (!dev->adb_enabled)
		goto out;

	pr_debug("disabling adb\n");
	dev->adb_enabled = 0;
    
#ifdef CONFIG_USB_AUTO_INSTALL
    /* if the user is using the usb tethering, don't switch the composition */
    if((product_id != curr_usb_pid_ptr->wlan_pid)  
        && ((GOOGLE_INDEX == usb_para_info.usb_pid_index)
            ||(product_id == curr_usb_pid_ptr->norm_pid)  
            ||(product_id == curr_usb_pid_ptr->auth_pid))
       )
    {
        USB_PR("%s permitted, product_id = 0x%x\n", __func__, product_id);
		ret = android_switch_composition(product_id);
    }
    else
    {
        USB_PR("%s disallowed, product_id = 0x%x\n", __func__, product_id);
    }
#else
	if (product_id)
		ret = android_switch_composition(product_id);
#endif  /* CONFIG_USB_AUTO_INSTALL */
out:
	mutex_unlock(&dev->lock);

	return ret;
}
#ifdef CONFIG_USB_AUTO_INSTALL
u16 android_get_product_id(void)
{
    return product_id;
}

/* for initialization */
void android_set_product_id(u16 pid)
{
    product_id = pid;
}

/* set USB serial number
   typically called before USB switch
*/
void set_usb_sn(char *sn_ptr)
{
    int len = 0;

    if(sn_ptr == NULL)
    {
        usb_sn_valid = 0;
    }
    else
    {
        len = strlen(sn_ptr);
    	if (len >= MAX_SERIAL_LEN) 
        {
            USB_PR("serial number string too long\n");
    		return;
    	}
    	strncpy(serial_number, sn_ptr, len);
    	serial_number[len] = '\0';
        usb_sn_valid = 1;
    }
}

/* call back function for delay work */
static void android_usb_switch_func(struct work_struct *w)
{
    USB_PR("%s\n", __func__);
    
    if (!_android_dev) 
    {
        USB_PR("_android_dev is NULL.\n");
		usb_switch_para.inprogress = 0;
    	return;
	}

	mutex_lock(&_android_dev->lock);
	android_switch_composition(usb_switch_dest_pid);
	mutex_unlock(&_android_dev->lock);

}

/* Called in usb_prepare() */
void android_delay_work_init(int add_flag)
{
    static int delay_work_init_flag = 0;

    if(1 == add_flag)
    {
        if(0 == delay_work_init_flag)
        {
            USB_PR("%s, initialize OK.\n", __func__);
            delay_work_init_flag = 1;
            INIT_DELAYED_WORK(&android_usb_switch_work, android_usb_switch_func);
        }
        else
        {
            USB_PR("%s, already initialized.\n", __func__);
        }
    }
    else
    {
        if(1 == delay_work_init_flag)
        {
            USB_PR("%s, cancel OK.\n", __func__);
            delay_work_init_flag = 0;
        	cancel_delayed_work_sync(&android_usb_switch_work);
        }
        else
        {
            USB_PR("%s, already cancelled.\n", __func__);
        }
    }
}

/* Global interface for USB switch
   pid:             destination product id
   delay_tick:      delay time to initiate the switch, 1 tick = 10ms
*/

int usb_switch_composition(u16 pid, unsigned long delay_tick)
{
	int ret = 0;
    
    USB_PR("%s, pid=0x%x\n", __func__, pid);
    
    usb_switch_dest_pid = pid;
    
	schedule_delayed_work(&android_usb_switch_work, delay_tick);

	return ret;
}

void kernel_set_adb_enable(u8 enable)
{
    struct android_dev *dev = _android_dev;
    dev->adb_enabled = enable;
}
#endif  /* CONFIG_USB_AUTO_INSTALL */

static struct file_operations adb_enable_fops = {
	.owner =   THIS_MODULE,
	.open =    adb_enable_open,
	.release = adb_enable_release,
};

static struct miscdevice adb_enable_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "android_adb_enable",
	.fops = &adb_enable_fops,
};

static int __init android_probe(struct platform_device *pdev)
{
	struct android_usb_platform_data *pdata = pdev->dev.platform_data;
	struct android_dev *dev = _android_dev;
	int ret;

	pr_debug("android_probe pdata: %p\n", pdata);

	if (!pdata || !pdata->vendor_id || !pdata->product_name ||
		!pdata->manufacturer_name)
		return -ENODEV;

	device_desc.idVendor =	__constant_cpu_to_le16(pdata->vendor_id);
	dev->version = pdata->version;
	strings_dev[STRING_PRODUCT_IDX].s = pdata->product_name;
	strings_dev[STRING_MANUFACTURER_IDX].s = pdata->manufacturer_name;
	strings_dev[STRING_SERIAL_IDX].s = serial_number;
	dev->nluns = pdata->nluns;
	dev->pdata = pdata;
#ifdef CONFIG_USB_AUTO_INSTALL
    /* If google don't want the CDROM, delete the following codes, namely, don't change nluns */
    if(GOOGLE_INDEX == usb_para_info.usb_pid_index)
    {
        dev->nluns = 2;
        USB_PR("Add CDROM to LIGHTNING. nluns=%d\n", dev->nluns);
    }
#endif  /* CONFIG_USB_AUTO_INSTALL */

	ret = sysfs_create_group(&pdev->dev.kobj, &android_attr_grp);
	if (ret < 0) {
		pr_err("%s: Failed to create the sysfs entry \n", __func__);
		return ret;
	}
	ret = sysfs_create_group(&pdev->dev.kobj, &android_func_attr_grp);
	if (ret < 0) {
		pr_err("%s: Failed to create the functions sysfs entry \n",
				__func__);
		sysfs_remove_group(&pdev->dev.kobj, &android_attr_grp);
	}

	return ret;
}

static struct platform_driver android_platform_driver = {
	.driver = { .name = "android_usb", },
	.probe = android_probe,
};

static int __init init(void)
{
	struct android_dev *dev;
	struct usb_composition *func;
	int ret;

	pr_debug("android init\n");

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto out;
	}

	_android_dev = dev;
	mutex_init(&dev->lock);

	ret = adb_function_init();
	if (ret)
		goto free_dev;

	ret = platform_driver_register(&android_platform_driver);
	if (ret)
		goto adb_exit;

	ret = misc_register(&adb_enable_device);
	if (ret)
		goto pdrv_unregister;

	/* Defer composite driver registration till product id is available */
	mutex_lock(&dev->lock);
	if (!product_id) {
		mutex_unlock(&dev->lock);
		ret = 0; /* not failure */
		goto out;
	}

	func = android_validate_product_id(product_id);
	if (!func) {
		mutex_unlock(&dev->lock);
		pr_err("%s: invalid product id\n", __func__);
		ret = -EINVAL;
		goto misc_deregister;
	}
	
#ifdef CONFIG_USB_AUTO_INSTALL
	/* in google mode, the default adb status is disable */
	if (GOOGLE_INDEX != usb_para_info.usb_pid_index)
	{
		dev->adb_enabled = 1;
		dev->functions = func->adb_functions;
	}
	else
	{
		dev->adb_enabled = 0;
		dev->functions = func->functions;
	}
#else
	dev->functions = func->functions;
#endif	/* CONFIG_USB_AUTO_INSTALL */

	ret = usb_composite_register(&android_usb_driver);
	if (ret) {
		mutex_unlock(&dev->lock);
		goto misc_deregister;
	}
	mutex_unlock(&dev->lock);

	return 0;

misc_deregister:
	misc_deregister(&adb_enable_device);
pdrv_unregister:
	platform_driver_unregister(&android_platform_driver);
adb_exit:
	adb_function_exit();
free_dev:
	kfree(dev);
out:
	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	if (is_usb_networking_on())
		gether_cleanup();

	usb_composite_unregister(&android_usb_driver);
	misc_deregister(&adb_enable_device);
	platform_driver_unregister(&android_platform_driver);
	adb_function_exit();
	kfree(_android_dev);
	_android_dev = NULL;
}
module_exit(cleanup);