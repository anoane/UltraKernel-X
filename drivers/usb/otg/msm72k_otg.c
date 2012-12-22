#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>

#include <linux/device.h>
#include <mach/msm_hsusb_hw.h>
#include <mach/msm72k_otg.h>
#include <mach/msm_hsusb.h>
#include <mach/rpc_pmapp.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

#define MSM_USB_BASE	(dev->regs)
#define is_host()	((OTGSC_ID & readl(USB_OTGSC)) ? 0 : 1)
#define is_b_sess_vld()	((OTGSC_BSV & readl(USB_OTGSC)) ? 1 : 0)
#define DRIVER_NAME	"msm_otg"

static void otg_reset(struct msm_otg *dev);
static void msm_otg_set_vbus_state(int online);
struct msm_otg *the_msm_otg;

static unsigned ulpi_read(struct msm_otg *dev, unsigned reg)
{
	unsigned timeout = 100000;

	/* initiate read operation */
	writel(ULPI_RUN | ULPI_READ | ULPI_ADDR(reg),
	       USB_ULPI_VIEWPORT);

	/* wait for completion */
	while ((readl(USB_ULPI_VIEWPORT) & ULPI_RUN) && (--timeout))
		cpu_relax();

	if (timeout == 0) {
		printk(KERN_ERR "ulpi_read: timeout %08x\n",
			readl(USB_ULPI_VIEWPORT));
		return 0xffffffff;
	}
	return ULPI_DATA_READ(readl(USB_ULPI_VIEWPORT));
}

static int ulpi_write(struct msm_otg *dev, unsigned val, unsigned reg)
{
	unsigned timeout = 10000;

	/* initiate write operation */
	writel(ULPI_RUN | ULPI_WRITE |
	       ULPI_ADDR(reg) | ULPI_DATA(val),
	       USB_ULPI_VIEWPORT);

	/* wait for completion */
	while ((readl(USB_ULPI_VIEWPORT) & ULPI_RUN) && (--timeout))
		;

	if (timeout == 0) {
		printk(KERN_ERR "ulpi_write: timeout\n");
		return -1;
	}

	return 0;
}

static void enable_idgnd(struct msm_otg *dev)
{
	ulpi_write(dev, (1<<4), 0x0E);
	ulpi_write(dev, (1<<4), 0x11);
	writel(readl(USB_OTGSC) | OTGSC_IDIE, USB_OTGSC);
}

static void disable_idgnd(struct msm_otg *dev)
{
	ulpi_write(dev, (1<<4), 0x0F);
	ulpi_write(dev, (1<<4), 0x12);
	writel(readl(USB_OTGSC) & ~OTGSC_IDIE, USB_OTGSC);
}

static void enable_sess_valid(struct msm_otg *dev)
{
	ulpi_write(dev, (1<<2), 0x0E);
	ulpi_write(dev, (1<<2), 0x11);
	writel(readl(USB_OTGSC) | OTGSC_BSVIE, USB_OTGSC);
}

static void disable_sess_valid(struct msm_otg *dev)
{
	ulpi_write(dev, (1<<2), 0x0F);
	ulpi_write(dev, (1<<2), 0x12);
	writel(readl(USB_OTGSC) & ~OTGSC_BSVIE, USB_OTGSC);
}


static int msm_otg_set_clk(struct otg_transceiver *xceiv, int on)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (on)
		/* enable clocks */
		clk_enable(dev->clk);
	else
		clk_disable(dev->clk);

	return 0;
}
static void msm_otg_start_peripheral(struct otg_transceiver *xceiv, int on)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	if (!xceiv->gadget)
		return;

	if (on) {
		/* increment the clk reference count so that
		 * it would be still on when disabled from
		 * low power mode routine
		 */
		if (dev->pclk_required_during_lpm)
			clk_enable(dev->pclk);

		usb_gadget_vbus_connect(xceiv->gadget);
	} else {
		atomic_set(&dev->chg_type, USB_CHG_TYPE__INVALID);

		usb_gadget_vbus_disconnect(xceiv->gadget);
		/* decrement the clk reference count so that
		 * it would be off when disabled from
		 * low power mode routine
		 */
		if (dev->pclk_required_during_lpm)
			clk_disable(dev->pclk);
	}
}

static void msm_otg_start_host(struct otg_transceiver *xceiv, int on)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!xceiv->host)
		return;

	if (dev->start_host)
		dev->start_host(xceiv->host, on);
}

static int msm_otg_suspend(struct msm_otg *dev)
{
	unsigned long timeout;
	int vbus = 0;
	enum chg_type chg_type = atomic_read(&dev->chg_type);

	disable_irq(dev->irq);
	if (dev->in_lpm)
		goto out;

	/* Don't reset if mini-A cable is connected */
	if (!is_host())
		otg_reset(dev);

	ulpi_read(dev, 0x14);/* clear PHY interrupt latch register */
	/* If there is no pmic notify support turn on phy comparators. */
	if (!dev->pmic_notif_supp || chg_type == USB_CHG_TYPE__WALLCHARGER)
		ulpi_write(dev, 0x01, 0x30);
	ulpi_write(dev, 0x08, 0x09);/* turn off PLL on integrated phy */

	timeout = jiffies + msecs_to_jiffies(500);
	disable_phy_clk();
	mdelay(1);
	while (!is_phy_clk_disabled()) {
		if (time_after(jiffies, timeout)) {
			pr_err("%s: Unable to suspend phy\n", __func__);
			otg_reset(dev);
			goto out;
		}
		msleep(1);
		if (((readl(USB_OTGSC) & OTGSC_INTR_MASK) >> 8) &
				readl(USB_OTGSC)) {
			goto out;
		}
	}

	writel(readl(USB_USBCMD) | ASYNC_INTR_CTRL | ULPI_STP_CTRL, USB_USBCMD);

        if (dev->otgclk)
                clk_disable(dev->otgclk);
        clk_disable(dev->pclk);
        clk_disable(dev->clk);
        if (dev->cclk)
                clk_disable(dev->cclk);
        clk_set_rate(dev->ebi1clk, 0);

	if (device_may_wakeup(dev->otg.dev)) {
		enable_irq_wake(dev->irq);
		if (dev->vbus_on_irq)
			enable_irq_wake(dev->vbus_on_irq);
	}

	dev->in_lpm = 1;

	if (!vbus && dev->pmic_notif_supp)
		dev->pmic_enable_ldo(0);

	USB_INFO("%s: usb in low power mode\n", __func__);

out:
	enable_irq(dev->irq);

	/* TBD: as there is no bus suspend implemented as of now
	 * it should be dummy check
	 */

	return 0;
}

static int msm_otg_resume(struct msm_otg *dev)
{
	unsigned temp;

	if (!dev->in_lpm)
		return 0;

        if (dev->cclk)
                clk_enable(dev->cclk);
        clk_enable(dev->clk);
        clk_enable(dev->pclk);
        if (dev->otgclk)
                clk_enable(dev->otgclk);


	temp = readl(USB_USBCMD);
	temp &= ~ASYNC_INTR_CTRL;
	temp &= ~ULPI_STP_CTRL;
	writel(temp, USB_USBCMD);

	/* If resume signalling finishes before lpm exit, PCD is not set in
	 * USBSTS register. Drive resume signal to the downstream device now
	 * so that host driver can process the upcoming port change interrupt.*/
	if (is_host())
		writel(readl(USB_PORTSC) | PORTSC_FPR, USB_PORTSC);

	if (device_may_wakeup(dev->otg.dev)) {
		disable_irq_wake(dev->irq);
		if (dev->vbus_on_irq)
			disable_irq_wake(dev->vbus_on_irq);
	}

	dev->in_lpm = 0;
	USB_INFO("%s: usb exited from low power mode\n", __func__);

	return 0;
}

static int msm_otg_set_suspend(struct otg_transceiver *xceiv, int suspend)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (suspend == dev->in_lpm)
		return 0;

	if (suspend)
		msm_otg_suspend(dev);
	else {
		unsigned long timeout;

		disable_irq(dev->irq);
		if (dev->pmic_notif_supp)
			dev->pmic_enable_ldo(1);

		msm_otg_resume(dev);

		if (!is_phy_clk_disabled())
			goto out;

		timeout = jiffies + usecs_to_jiffies(100);
		enable_phy_clk();
		while (is_phy_clk_disabled()) {
			if (time_after(jiffies, timeout)) {
				pr_err("%s: Unable to wakeup phy\n", __func__);
				otg_reset(dev);
				break;
			}
			udelay(10);
		}
out:
		enable_irq(dev->irq);

	}

	return 0;
}

static int msm_otg_set_peripheral(struct otg_transceiver *xceiv,
			struct usb_gadget *gadget)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (!gadget) {
		msm_otg_start_peripheral(xceiv, 0);
		dev->otg.gadget = 0;
		disable_sess_valid(dev);
		if (dev->pmic_notif_supp && dev->pmic_unregister_vbus_sn)
			dev->pmic_unregister_vbus_sn(&msm_otg_set_vbus_state);
		return 0;
	}
	dev->otg.gadget = gadget;
	enable_sess_valid(dev);
	if (dev->pmic_notif_supp && dev->pmic_register_vbus_sn)
		dev->pmic_register_vbus_sn(&msm_otg_set_vbus_state);
	USB_INFO("peripheral driver registered w/ tranceiver\n");

	if (is_host())
		msm_otg_start_host(&dev->otg, 1);
	else if (is_b_sess_vld())
		msm_otg_start_peripheral(&dev->otg, 1);
	else
		msm_otg_suspend(dev);

	return 0;
}

static int msm_otg_set_host(struct otg_transceiver *xceiv, struct usb_bus *host)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (!dev->start_host)
		return -ENODEV;

	if (!host) {
		msm_otg_start_host(xceiv, 0);
		dev->otg.host = 0;
		dev->start_host = 0;
		disable_idgnd(dev);
		return 0;
	}
	dev->otg.host = host;
	enable_idgnd(dev);
	USB_INFO("host driver registered w/ tranceiver\n");

#ifndef CONFIG_USB_GADGET_MSM_72K
	if (is_host())
		msm_otg_start_host(&dev->otg, 1);
	else
		msm_otg_suspend(dev);
#endif
	return 0;
}

static void msm_otg_set_vbus_state(int online)
{
	struct msm_otg *dev = the_msm_otg;

	pr_info("%s: vbus state: %d.\n", __func__,online);
	if (online)
		msm_otg_set_suspend(&dev->otg, 0);
}

/* pmic irq handlers are called from thread context and
 * are allowed to sleep
 */
static irqreturn_t pmic_vbus_on_irq(int irq, void *data)
{
	struct msm_otg *dev = the_msm_otg;

	if (!dev->otg.gadget)
		return IRQ_HANDLED;

	USB_INFO("%s: vbus notification from pmic\n", __func__);

	msm_otg_set_suspend(&dev->otg, 0);

	return IRQ_HANDLED;
}

static irqreturn_t msm_otg_irq(int irq, void *data)
{
	struct msm_otg *dev = data;
	u32 otgsc = 0;

	//return IRQ_NONE;

	if (dev->in_lpm) {
		msm_otg_resume(dev);
		return IRQ_HANDLED;
	}

	otgsc = readl(USB_OTGSC);
	if (!(otgsc & OTGSC_INTR_STS_MASK))
		return IRQ_HANDLED;

	USB_INFO("-== OTG IRQ HANDLE ==-\n");

	if ((otgsc & OTGSC_IDIS) && (otgsc & OTGSC_IDIE)) {
		pr_info("ID -> (%s)\n", (otgsc & OTGSC_ID) ? "B" : "A");
		msm_otg_start_host(&dev->otg, is_host());
	} else if ((otgsc & OTGSC_BSVIS) && (otgsc & OTGSC_BSVIE)) {
		pr_info("VBUS - (%s)\n", otgsc & OTGSC_BSV ? "ON" : "OFF");
		if (!is_host())
			msm_otg_start_peripheral(&dev->otg, is_b_sess_vld());
	}
	writel(otgsc, USB_OTGSC);

	return IRQ_HANDLED;
}

#define USB_LINK_RESET_TIMEOUT	(msecs_to_jiffies(10))
static void otg_reset(struct msm_otg *dev)
{
	unsigned long timeout;
	unsigned temp;

	clk_enable(dev->clk);

	if (dev->phy_reset)
		dev->phy_reset(dev->regs);

	/*disable all phy interrupts*/
	ulpi_write(dev, 0xFF, 0x0F);
	ulpi_write(dev, 0xFF, 0x12);
	msleep(100);

	writel(USBCMD_RESET, USB_USBCMD);
	timeout = jiffies + USB_LINK_RESET_TIMEOUT;
	do {
		if (time_after(jiffies, timeout)) {
			pr_err("msm_otg: usb link reset timeout\n");
			break;
		}
		msleep(1);
	} while (readl(USB_USBCMD) & USBCMD_RESET);

	/* select ULPI phy */
	writel(0x80000000, USB_PORTSC);

	temp = ulpi_read(dev, ULPI_CONFIG_REG);
	temp |= ULPI_AMPLITUDE_MAX;
	ulpi_write(dev, temp, ULPI_CONFIG_REG);

	writel(0x0, USB_AHB_BURST);
	writel(0x00, USB_AHB_MODE);
	clk_disable(dev->clk);

	if (dev->otg.gadget)
		enable_sess_valid(dev);
	if (dev->otg.host)
		enable_idgnd(dev);
}

static int __init msm_otg_probe(struct platform_device *pdev)
{
	int ret = 0;
	int vbus_on_irq = 0;
	struct resource *res;
	struct msm_otg *dev;
	struct msm_otg_platform_data *pdata;

	USB_INFO("-== MSM7201 OTG DRIVER START PROBE ==-\n");

	dev = kzalloc(sizeof(struct msm_otg), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	USB_INFO("-== MSM7201 OTG DRIVER START INIT ==-\n");

	dev->otg.dev = &pdev->dev;
	pdata = pdev->dev.platform_data;

	if (pdev->dev.platform_data) {
		dev->phy_reset = pdata->phy_reset;
		dev->core_clk  = pdata->core_clk;
		dev->rpc_connect = pdata->rpc_connect;
		/* pmic apis */
		dev->pmic_notif_init = pdata->pmic_notif_init;
		dev->pmic_notif_deinit = pdata->pmic_notif_deinit;
		dev->pmic_register_vbus_sn = pdata->pmic_register_vbus_sn;
		dev->pmic_unregister_vbus_sn = pdata->pmic_unregister_vbus_sn;
		dev->pmic_enable_ldo = pdata->pmic_enable_ldo;
		dev->pclk_required_during_lpm = pdata->pclk_required_during_lpm;
	}

	if (pdata && pdata->pmic_vbus_irq) {
		vbus_on_irq = platform_get_irq_byname(pdev, "vbus_on");
		if (vbus_on_irq < 0) {
			pr_err("%s: unable to get vbus on irq\n", __func__);
			ret = vbus_on_irq;
			goto free_dev;
		}
	}

        if (dev->rpc_connect) {
                ret = dev->rpc_connect(1);
                pr_info("%s: rpc_connect(%d)\n", __func__, ret);
        }

	dev->clk = clk_get(NULL, "usb_hs_clk");
	if (IS_ERR(dev->clk)) {
		pr_err("%s: failed to get usb_hs_clk\n", __func__);
		ret = PTR_ERR(dev->clk);
		goto free_dev;
	}
	dev->pclk = clk_get(NULL, "usb_hs_pclk");
	if (IS_ERR(dev->pclk)) {
		pr_err("%s: failed to get usb_hs_pclk\n", __func__);
		ret = PTR_ERR(dev->pclk);
		goto put_clk;
	}

	dev->otgclk = clk_get(NULL, "usb_otg_clk");
	if (IS_ERR(dev->otgclk)) {
		dev->otgclk = NULL;
	}

        if (dev->core_clk) {
                dev->cclk = clk_get(&pdev->dev, "usb_hs_core_clk");
                if (IS_ERR(dev->cclk)) {
                        pr_err("%s: failed to get usb_hs_core_clk\n", __func__);
                        ret = PTR_ERR(dev->cclk);
                        goto put_pclk;
                }
        }

	dev->ebi1clk = clk_get(NULL, "ebi1_clk");
	if (IS_ERR(dev->ebi1clk)) {
                        pr_err("%s: failed to get ebil_clk\n", __func__);
                        ret = PTR_ERR(dev->ebi1clk);
                        goto put_cclk;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("%s: failed to get platform resource mem\n", __func__);
		ret = -ENODEV;
		goto put_eclk;
	}

	dev->regs = ioremap(res->start, resource_size(res));
	if (!dev->regs) {
		pr_err("%s: ioremap failed\n", __func__);
		ret = -ENOMEM;
		goto put_eclk;
	}

	dev->irq = platform_get_irq(pdev, 0);
	if (!dev->irq) {
		pr_err("%s: platform_get_irq failed\n", __func__);
		ret = -ENODEV;
		goto free_regs;
	}

	/* enable clocks */
	clk_enable(dev->pclk);
	if (dev->cclk)
		clk_enable(dev->cclk);

	/* To reduce phy power consumption and to avoid external LDO
	 * on the board, PMIC comparators can be used to detect VBUS
	 * session change.
	 */
	if (dev->pmic_notif_init) {
		ret = dev->pmic_notif_init();

		USB_INFO("%s: request pmapp init notify init = %d.f1=%p, f2=%p\n", __func__, ret, msm_pm_app_rpc_init,dev->pmic_notif_init);
		if (!ret) {
			dev->pmic_notif_supp = 1;
			dev->pmic_enable_ldo(1);

		} else if (ret != -ENOTSUPP) {

			pr_info("%s: request pmic notify failed\n", __func__);

			clk_disable(dev->pclk);
			if (dev->cclk)
				clk_disable(dev->cclk);
			goto free_regs;
		}
	}

	otg_reset(dev);

	ret = request_irq(dev->irq, msm_otg_irq, IRQF_SHARED,
					"msm_otg", dev);
	if (ret) {
		pr_info("%s: request irq failed\n", __func__);
		clk_disable(dev->pclk);
		if (dev->cclk)
			clk_disable(dev->cclk);
		goto free_regs;
	}

	the_msm_otg = dev;
	dev->vbus_on_irq = vbus_on_irq;
	dev->otg.set_peripheral = msm_otg_set_peripheral;
	dev->otg.set_host = msm_otg_set_host;
	dev->otg.set_suspend = msm_otg_set_suspend;
	dev->set_clk = msm_otg_set_clk;
	if (otg_set_transceiver(&dev->otg)) {
		WARN_ON(1);
		goto free_otg_irq;
	}

	device_init_wakeup(&pdev->dev, 1);


	if (vbus_on_irq) {
		ret = request_irq(vbus_on_irq, pmic_vbus_on_irq,
				IRQF_TRIGGER_RISING, "msm_otg_vbus_on", NULL);
		if (ret) {
			pr_info("%s: request_irq for vbus_on"
					"interrupt failed\n", __func__);
			goto free_otg_irq;
		}
	}

	USB_INFO("-== MSM7201 OTG DRIVER END PROBE ==-\n");
	return 0;
free_otg_irq:
	free_irq(dev->irq, dev);
free_regs:
	iounmap(dev->regs);
put_eclk:
	if(dev->ebi1clk)
		clk_put(dev->ebi1clk);
put_cclk:
	if (dev->cclk)
		clk_put(dev->cclk);
put_pclk:
	clk_put(dev->pclk);
put_clk:
	clk_put(dev->clk);
free_dev:
	kfree(dev);
	return ret;
}

static int __exit msm_otg_remove(struct platform_device *pdev)
{
	struct msm_otg *dev = the_msm_otg;

	if (dev->pmic_notif_supp)
		dev->pmic_notif_deinit();

	free_irq(dev->irq, pdev);
	if (dev->vbus_on_irq)
		free_irq(dev->irq, 0);
	iounmap(dev->regs);
	if (dev->cclk)
		clk_disable(dev->cclk);
	clk_disable(dev->pclk);
	if (dev->cclk)
		clk_put(dev->cclk);
	clk_put(dev->pclk);
	clk_put(dev->clk);
        if(dev->ebi1clk)
                clk_put(dev->ebi1clk);
        if(dev->otgclk)
                clk_put(dev->otgclk);

	kfree(dev);
	return 0;
}

static struct platform_driver msm_otg_driver = {
	.remove = __exit_p(msm_otg_remove),
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init msm_otg_init(void)
{
	return platform_driver_probe(&msm_otg_driver, msm_otg_probe);
}

static void __exit msm_otg_exit(void)
{
	platform_driver_unregister(&msm_otg_driver);
}

module_init(msm_otg_init);
module_exit(msm_otg_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("MSM usb transceiver driver");
MODULE_VERSION("1.00");
