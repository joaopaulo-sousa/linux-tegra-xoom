// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB cable detection for the Motorola CPCAP PMIC on Tegra20 devices
 *
 * The micro-USB ID and VBUS lines of the Motorola Xoom are only sensed by
 * the CPCAP. Report a grounded ID pin (OTG adapter) as a USB host cable and
 * a valid VBUS as a USB device cable, like the stock "whisper" driver did,
 * so the ChipIdea controller can switch roles.
 */

#include <linux/bits.h>
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/mfd/motorola-cpcap.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CPCAP_BIT_VBUSPD	BIT(5)	/* CPCAP_REG_USBC1: VBUS pull-down */

#define CPCAP_EXTCON_DEBOUNCE_MS	100

static const unsigned int cpcap_extcon_cables[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

struct cpcap_extcon {
	struct device *dev;
	struct regmap *reg;
	struct extcon_dev *edev;
	struct delayed_work work;
	int irq_id_ground;
	int irq_vbusvld;
	bool host;
	bool usb;
};

static void cpcap_extcon_detect(struct work_struct *work)
{
	struct cpcap_extcon *ddata = container_of(to_delayed_work(work),
						  struct cpcap_extcon, work);
	int id_ground, vbusvld;
	bool host, usb;

	id_ground = cpcap_sense_virq(ddata->reg, ddata->irq_id_ground);
	vbusvld = cpcap_sense_virq(ddata->reg, ddata->irq_vbusvld);
	if (id_ground < 0 || vbusvld < 0) {
		dev_err(ddata->dev, "failed to read sense bits: %d %d\n",
			id_ground, vbusvld);
		return;
	}

	/*
	 * An external VBUS (PC, charger) wins over a grounded ID pin. Once in
	 * host mode we drive VBUS ourselves, so only the ID pin matters.
	 */
	if (ddata->host)
		host = id_ground;
	else
		host = id_ground && !vbusvld;
	usb = !host && vbusvld;

	if (host == ddata->host && usb == ddata->usb)
		return;

	dev_info(ddata->dev, "id_ground=%d vbusvld=%d -> %s\n", id_ground,
		 vbusvld, host ? "host" : usb ? "device" : "none");

	/* The VBUS pull-down fights our own VBUS and an external host */
	regmap_update_bits(ddata->reg, CPCAP_REG_USBC1, CPCAP_BIT_VBUSPD,
			   (host || usb) ? 0 : CPCAP_BIT_VBUSPD);

	/* Drop the old role before reporting the new one */
	if (!host && ddata->host)
		extcon_set_state_sync(ddata->edev, EXTCON_USB_HOST, false);
	if (!usb && ddata->usb)
		extcon_set_state_sync(ddata->edev, EXTCON_USB, false);
	if (host && !ddata->host)
		extcon_set_state_sync(ddata->edev, EXTCON_USB_HOST, true);
	if (usb && !ddata->usb)
		extcon_set_state_sync(ddata->edev, EXTCON_USB, true);

	ddata->host = host;
	ddata->usb = usb;
}

static irqreturn_t cpcap_extcon_irq(int irq, void *data)
{
	struct cpcap_extcon *ddata = data;

	mod_delayed_work(system_wq, &ddata->work,
			 msecs_to_jiffies(CPCAP_EXTCON_DEBOUNCE_MS));

	return IRQ_HANDLED;
}

static void cpcap_extcon_cancel_work(void *data)
{
	struct cpcap_extcon *ddata = data;

	cancel_delayed_work_sync(&ddata->work);
}

static int cpcap_extcon_request_irq(struct platform_device *pdev,
				    struct cpcap_extcon *ddata,
				    const char *name)
{
	int irq, error;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0)
		return irq;

	error = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					  cpcap_extcon_irq, IRQF_ONESHOT,
					  name, ddata);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				     "failed to request irq %s\n", name);

	return irq;
}

static int cpcap_extcon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cpcap_extcon *ddata;
	int irq, error;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->dev = dev;
	ddata->reg = dev_get_regmap(dev->parent, NULL);
	if (!ddata->reg)
		return -ENODEV;

	ddata->edev = devm_extcon_dev_allocate(dev, cpcap_extcon_cables);
	if (IS_ERR(ddata->edev))
		return PTR_ERR(ddata->edev);

	error = devm_extcon_dev_register(dev, ddata->edev);
	if (error)
		return dev_err_probe(dev, error, "failed to register extcon\n");

	INIT_DELAYED_WORK(&ddata->work, cpcap_extcon_detect);
	error = devm_add_action_or_reset(dev, cpcap_extcon_cancel_work, ddata);
	if (error)
		return error;

	irq = cpcap_extcon_request_irq(pdev, ddata, "id_ground");
	if (irq < 0)
		return irq;
	ddata->irq_id_ground = irq;

	irq = cpcap_extcon_request_irq(pdev, ddata, "id_float");
	if (irq < 0)
		return irq;

	irq = cpcap_extcon_request_irq(pdev, ddata, "vbusvld");
	if (irq < 0)
		return irq;
	ddata->irq_vbusvld = irq;

	platform_set_drvdata(pdev, ddata);
	schedule_delayed_work(&ddata->work, 0);

	return 0;
}

static const struct of_device_id cpcap_extcon_of_match[] = {
	{ .compatible = "motorola,cpcap-usb-extcon" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cpcap_extcon_of_match);

static struct platform_driver cpcap_extcon_driver = {
	.probe = cpcap_extcon_probe,
	.driver = {
		.name = "cpcap-usb-extcon",
		.of_match_table = cpcap_extcon_of_match,
	},
};
module_platform_driver(cpcap_extcon_driver);

MODULE_DESCRIPTION("Motorola CPCAP USB cable detection");
MODULE_LICENSE("GPL");
