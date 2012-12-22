/* linux/include/asm-arm/arch-msm/hsusb.h
 *
 * Copyright (C) 2008 Google, Inc.
 * Author: Brian Swetland <swetland@google.com>
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

#ifndef __ASM_ARCH_MSM_HSUSB_H
#define __ASM_ARCH_MSM_HSUSB_H

#include <linux/types.h>

#define PHY_TYPE_MASK           0x0F
#define PHY_TYPE_MODE           0xF0
#define PHY_MODEL_MASK          0xFF00
#define PHY_TYPE(x)             ((x) & PHY_TYPE_MASK)
#define PHY_MODEL(x)            ((x) & PHY_MODEL_MASK)

#define USB_PHY_MODEL_65NM      0x100
#define USB_PHY_MODEL_180NM     0x200
#define USB_PHY_MODEL_45NM      0x400
#define USB_PHY_UNDEFINED       0x00
#define USB_PHY_INTEGRATED      0x01
#define USB_PHY_EXTERNAL        0x02
#define USB_PHY_SERIAL_PMIC     0x04

#define REQUEST_STOP            0
#define REQUEST_START           1
#define REQUEST_RESUME          2

enum hsusb_phy_type {
        UNDEFINED,
        INTEGRATED,
        EXTERNAL,
};


/* platform device data for msm_hsusb driver */

#ifdef CONFIG_USB_FUNCTION
/* matches a product ID to a list of enabled functions */
struct msm_hsusb_product {
	/* product ID for usb_device_descriptor.idProduct */
	__u16 product_id;

	/* bit mask of enabled usb_functions, matching ordering
	** in msm_hsusb_platform_data.functions
	*/
	__u32 functions;
};
#endif

#ifdef CONFIG_USB_GADGET_MSM_72K
enum chg_type {
        USB_CHG_TYPE__SDP,
        USB_CHG_TYPE__CARKIT,
        USB_CHG_TYPE__WALLCHARGER,
        USB_CHG_TYPE__INVALID,
#ifdef CONFIG_SUPPORT_ALIEN_USB_CHARGER
        USB_CHG_TYPE__MIGHT_BE_HOST_PC,
        USB_CHG_TYPE__ALIENCHARGER,
#endif /*  CONFIG_SUPPORT_ALIEN_USB_CHARGER */
};
#endif


struct msm_otg_platform_data {
        int (*rpc_connect)(int);
        int (*phy_reset)(void __iomem *);
        unsigned int core_clk;
        int pmic_vbus_irq;
        int pclk_required_during_lpm;

        /* pmic notfications apis */
        int (*pmic_notif_init) (void);
        void (*pmic_notif_deinit) (void);
        int (*pmic_register_vbus_sn) (void (*callback)(int online));
        void (*pmic_unregister_vbus_sn) (void (*callback)(int online));
        int (*pmic_enable_ldo) (int);
};

struct msm_hsusb_gadget_platform_data {
        int *phy_init_seq;
        void (*phy_reset)(void);

        u32 swfi_latency;
        int self_powered;
};


struct msm_hsusb_platform_data {
	/* hard reset the ULPI PHY */
	void (*phy_reset)(void);
	void (*phy_shutdown)(void);

	/* (de)assert the reset to the usb core */
	void (*hw_reset)(bool enable);

	/* for notification when USB is connected or disconnected */
	void (*usb_connected)(int);
	/* 1 : uart, 0 : usb */
	void (*usb_uart_switch)(int);
	void (*config_usb_id_gpios)(bool enable);
        void (*usb_hub_enable)(bool);
        void (*serial_debug_gpios)(int);
        int (*china_ac_detect)(void);
        void (*disable_usb_charger)(void);
        /* val, reg pairs terminated by -1 */
        int *phy_init_seq;
        void (*change_phy_voltage)(int);
        int (*ldo_init) (int init);
        int (*ldo_enable) (int enable);
        int (*rpc_connect)(int);
        /* 1 : mhl, 0 : usb */
        void (*usb_mhl_switch)(bool);

#ifdef CONFIG_USB_FUNCTION
	/* USB device descriptor fields */
	__u16 vendor_id;

	/* Default product ID.
	** This can be overridden dynamically based on the disabled
	** state of the functions using the product_table.
	*/
	__u16 product_id;

	__u16 version;
	char *product_name;
	char *manufacturer_name;

	/* list of function drivers to bind to this configuration */
	int num_functions;
	char **functions;

	/* if num_products is zero, then the default value in product_id
	** is used for the configuration descriptor.
	*/
	int num_products;
	struct msm_hsusb_product *products;
#endif
        char *serial_number;
        int usb_id_pin_gpio;
        int dock_pin_gpio;
        int id_pin_irq;
        bool enable_car_kit_detect;
        __u8 accessory_detect;
        bool dock_detect;

        int ac_9v_gpio;
};

int usb_get_connect_type(void);

struct msm_usb_host_platform_data {
        unsigned phy_info;
        int (*phy_reset)(void __iomem *addr);
        void (*config_gpio)(unsigned int config);
        void (*vbus_power) (unsigned phy_info, int on);
};

#endif
