// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * W1 master driver for the internal OWR controller of NVIDIA Tegra20.
 *
 * Copyright (C) 2010 Motorola, Inc
 * Author: Andrei Warkentin <andreiw@motorola.com>
 *
 * Ported from the Motorola Xoom (stingray) downstream kernel.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/w1.h>

/* OWR_CONTROL_0 is the main control register, written last. */
#define OWR_CONTROL		0x0
#define OC_RD_BIT		BIT(31)
#define OC_WR0_BIT		BIT(30)
#define OC_RD_SCLK_SHIFT	23
#define OC_RD_SCLK_MASK		0xf
#define OC_P_SCLK_SHIFT		15
#define OC_P_SCLK_MASK		0xff
#define OC_BIT_XMODE		BIT(2)
#define OC_GO			BIT(0)

/* OWR_WR_RD_TCTL_0 controls read/write timings. */
#define OWR_WR_RD_TCTL		0xc
#define ORWT_TSU_SHIFT		28
#define ORWT_TSU_MASK		0x3
#define ORWT_TRELEASE_SHIFT	22
#define ORWT_TRELEASE_MASK	0x3f
#define ORWT_TRDV_SHIFT		18
#define ORWT_TRDV_MASK		0xf
#define ORWT_TLOW0_SHIFT	11
#define ORWT_TLOW0_MASK		0x7f
#define ORWT_TLOW1_SHIFT	7
#define ORWT_TLOW1_MASK		0xf
#define ORWT_TSLOT_SHIFT	0
#define ORWT_TSLOT_MASK		0x7f

/* OWR_RST_PRES_TCTL_0 controls reset presence timings. */
#define OWR_RST_PRES_TCTL	0x10
#define ORPT_TPDL_SHIFT		24
#define ORPT_TPDL_MASK		0xff
#define ORPT_TPDH_SHIFT		18
#define ORPT_TPDH_MASK		0x3f
#define ORPT_TRSTL_SHIFT	9
#define ORPT_TRSTL_MASK		0x1ff
#define ORPT_TRSTH_SHIFT	0
#define ORPT_TRSTH_MASK		0x1ff

#define OWR_INTR_MASK		0x24
#define OI_BIT_XFER_DONE	BIT(13)
#define OI_PRESENCE_DONE	BIT(5)
#define OI_PRESENCE_ERR		BIT(0)

#define OWR_INTR_STATUS		0x28

#define OWR_STATUS		0x34
#define OS_READ_BIT_SHIFT	23

#define BIT_XFER_TIMEOUT_MS	5000
#define BIT_XFER_ERRORS		OI_PRESENCE_ERR

/* OWR requires a 1MHz clock; timings below are in OWR clock cycles. */
#define OWR_CLOCK		1000000

struct tegra_w1_timings {
	u32 tsu;
	u32 trelease;
	u32 trdv;
	u32 tlow0;
	u32 tlow1;
	u32 tslot;
	u32 tpdl;
	u32 tpdh;
	u32 trstl;
	u32 trsth;
	u32 rdsclk;
	u32 psclk;
};

/* Values used by the Motorola stingray/wingray downstream kernel. */
static const struct tegra_w1_timings tegra20_w1_timings = {
	.tsu = 0x1,
	.trelease = 0xf,
	.trdv = 0xf,
	.tlow0 = 0x3c,
	.tlow1 = 0x1,
	.tslot = 0x78,
	.tpdl = 0x3c,
	.tpdh = 0x1e,
	.trstl = 0x1ea,
	.trsth = 0x1df,
	.rdsclk = 0x7,
	.psclk = 0x50,
};

struct tegra_w1 {
	struct device *dev;
	struct w1_bus_master bus_master;
	struct clk *clk;
	void __iomem *base;
	struct mutex lock;
	spinlock_t irq_lock;
	struct completion *xfer_done;
	u32 intr_status;
	const struct tegra_w1_timings *timings;
	bool ready;
};

static inline u32 owr_readl(struct tegra_w1 *w1, unsigned int reg)
{
	return readl(w1->base + reg);
}

static inline void owr_writel(struct tegra_w1 *w1, u32 val, unsigned int reg)
{
	writel(val, w1->base + reg);
}

static u32 tegra_w1_control(struct tegra_w1 *w1)
{
	const struct tegra_w1_timings *t = w1->timings;

	return ((t->rdsclk & OC_RD_SCLK_MASK) << OC_RD_SCLK_SHIFT) |
	       ((t->psclk & OC_P_SCLK_MASK) << OC_P_SCLK_SHIFT) |
	       OC_BIT_XMODE;
}

/* Wait for a transfer and check the status against errors and @mask. */
static int tegra_w1_wait(struct tegra_w1 *w1, u32 mask)
{
	unsigned long flags;
	u32 status;

	if (!wait_for_completion_timeout(w1->xfer_done,
					 msecs_to_jiffies(BIT_XFER_TIMEOUT_MS))) {
		dev_dbg(w1->dev, "transfer timeout\n");
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&w1->irq_lock, flags);
	status = w1->intr_status;
	w1->intr_status = 0;
	spin_unlock_irqrestore(&w1->irq_lock, flags);

	if ((status & BIT_XFER_ERRORS) || !(status & mask))
		return -EIO;

	return 0;
}

/* Program the timing registers and put the controller in a known state. */
static void tegra_w1_setup(struct tegra_w1 *w1)
{
	const struct tegra_w1_timings *t = w1->timings;
	u32 val;

	val = ((t->tslot & ORWT_TSLOT_MASK) << ORWT_TSLOT_SHIFT) |
	      ((t->tlow1 & ORWT_TLOW1_MASK) << ORWT_TLOW1_SHIFT) |
	      ((t->tlow0 & ORWT_TLOW0_MASK) << ORWT_TLOW0_SHIFT) |
	      ((t->trdv & ORWT_TRDV_MASK) << ORWT_TRDV_SHIFT) |
	      ((t->trelease & ORWT_TRELEASE_MASK) << ORWT_TRELEASE_SHIFT) |
	      ((t->tsu & ORWT_TSU_MASK) << ORWT_TSU_SHIFT);
	owr_writel(w1, val, OWR_WR_RD_TCTL);

	val = ((t->trsth & ORPT_TRSTH_MASK) << ORPT_TRSTH_SHIFT) |
	      ((t->trstl & ORPT_TRSTL_MASK) << ORPT_TRSTL_SHIFT) |
	      ((t->tpdh & ORPT_TPDH_MASK) << ORPT_TPDH_SHIFT) |
	      ((t->tpdl & ORPT_TPDL_MASK) << ORPT_TPDL_SHIFT);
	owr_writel(w1, val, OWR_RST_PRES_TCTL);

	owr_writel(w1, 0, OWR_INTR_MASK);
	owr_writel(w1, 0xffffffff, OWR_INTR_STATUS);
}

static irqreturn_t tegra_w1_irq(int irq, void *data)
{
	struct tegra_w1 *w1 = data;
	unsigned long flags;
	u32 status;

	status = owr_readl(w1, OWR_INTR_STATUS);
	if (!status)
		return IRQ_NONE;

	spin_lock_irqsave(&w1->irq_lock, flags);
	owr_writel(w1, status, OWR_INTR_STATUS);
	if (w1->xfer_done) {
		w1->intr_status = status;
		complete(w1->xfer_done);
	} else {
		dev_warn_ratelimited(w1->dev, "spurious interrupt 0x%x\n",
				     status);
	}
	spin_unlock_irqrestore(&w1->irq_lock, flags);

	return IRQ_HANDLED;
}

/* Perform a write-0 cycle if bit == 0, otherwise a read cycle. */
static u8 tegra_w1_touch_bit(void *data, u8 bit)
{
	struct tegra_w1 *w1 = data;
	DECLARE_COMPLETION_ONSTACK(done);
	u8 ret = 0;
	int err;

	mutex_lock(&w1->lock);
	if (!w1->ready || clk_enable(w1->clk))
		goto unlock;

	owr_writel(w1, OI_BIT_XFER_DONE, OWR_INTR_MASK);
	w1->xfer_done = &done;

	if (bit) {
		owr_writel(w1, tegra_w1_control(w1) | OC_RD_BIT, OWR_CONTROL);
		err = tegra_w1_wait(w1, OI_BIT_XFER_DONE);
		if (err)
			dev_err(w1->dev, "write-1/read failed: %d\n", err);
		else
			ret = (owr_readl(w1, OWR_STATUS) >> OS_READ_BIT_SHIFT) & 1;
	} else {
		owr_writel(w1, tegra_w1_control(w1) | OC_WR0_BIT, OWR_CONTROL);
		err = tegra_w1_wait(w1, OI_BIT_XFER_DONE);
		if (err)
			dev_err(w1->dev, "write-0 failed: %d\n", err);
	}

	owr_writel(w1, 0, OWR_INTR_MASK);
	w1->xfer_done = NULL;
	clk_disable(w1->clk);
unlock:
	mutex_unlock(&w1->lock);
	return ret;
}

/* Perform a bus reset cycle; returns 0 if slaves are present. */
static u8 tegra_w1_reset_bus(void *data)
{
	struct tegra_w1 *w1 = data;
	DECLARE_COMPLETION_ONSTACK(done);
	u8 presence = 1;

	mutex_lock(&w1->lock);
	if (!w1->ready || clk_enable(w1->clk))
		goto unlock;

	owr_writel(w1, OI_PRESENCE_DONE, OWR_INTR_MASK);
	w1->xfer_done = &done;
	owr_writel(w1, tegra_w1_control(w1) | OC_GO, OWR_CONTROL);

	if (!tegra_w1_wait(w1, OI_PRESENCE_DONE))
		presence = 0;

	dev_dbg(w1->dev, "%s\n", presence ? "no slaves present" :
					    "slaves present");

	owr_writel(w1, 0, OWR_INTR_MASK);
	w1->xfer_done = NULL;
	clk_disable(w1->clk);
unlock:
	mutex_unlock(&w1->lock);
	return presence;
}

static int tegra_w1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rst;
	struct tegra_w1 *w1;
	int irq, err;

	w1 = devm_kzalloc(dev, sizeof(*w1), GFP_KERNEL);
	if (!w1)
		return -ENOMEM;

	w1->dev = dev;
	w1->timings = of_device_get_match_data(dev);
	mutex_init(&w1->lock);
	spin_lock_init(&w1->irq_lock);

	w1->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w1->base))
		return PTR_ERR(w1->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	w1->clk = devm_clk_get_prepared(dev, NULL);
	if (IS_ERR(w1->clk))
		return dev_err_probe(dev, PTR_ERR(w1->clk), "failed to get clock\n");

	err = clk_set_rate(w1->clk, OWR_CLOCK);
	if (err)
		return dev_err_probe(dev, err, "failed to set clock rate\n");

	rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get reset\n");

	err = clk_enable(w1->clk);
	if (err)
		return err;

	reset_control_deassert(rst);
	tegra_w1_setup(w1);
	clk_disable(w1->clk);

	err = devm_request_irq(dev, irq, tegra_w1_irq, 0, dev_name(dev), w1);
	if (err)
		return err;

	w1->bus_master.data = w1;
	w1->bus_master.touch_bit = tegra_w1_touch_bit;
	w1->bus_master.reset_bus = tegra_w1_reset_bus;

	platform_set_drvdata(pdev, w1);
	w1->ready = true;

	err = w1_add_master_device(&w1->bus_master);
	if (err) {
		w1->ready = false;
		return dev_err_probe(dev, err, "failed to add w1 master\n");
	}

	return 0;
}

static void tegra_w1_remove(struct platform_device *pdev)
{
	struct tegra_w1 *w1 = platform_get_drvdata(pdev);

	w1_remove_master_device(&w1->bus_master);

	mutex_lock(&w1->lock);
	w1->ready = false;
	mutex_unlock(&w1->lock);
}

static int tegra_w1_suspend(struct device *dev)
{
	struct tegra_w1 *w1 = dev_get_drvdata(dev);

	mutex_lock(&w1->lock);
	w1->ready = false;
	mutex_unlock(&w1->lock);

	return 0;
}

static int tegra_w1_resume(struct device *dev)
{
	struct tegra_w1 *w1 = dev_get_drvdata(dev);
	int err;

	mutex_lock(&w1->lock);
	err = clk_enable(w1->clk);
	if (!err) {
		tegra_w1_setup(w1);
		clk_disable(w1->clk);
		w1->ready = true;
	}
	mutex_unlock(&w1->lock);

	return err;
}

static DEFINE_SIMPLE_DEV_PM_OPS(tegra_w1_pm_ops, tegra_w1_suspend,
				tegra_w1_resume);

static const struct of_device_id tegra_w1_of_match[] = {
	{ .compatible = "nvidia,tegra20-owr", .data = &tegra20_w1_timings },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra_w1_of_match);

static struct platform_driver tegra_w1_driver = {
	.probe = tegra_w1_probe,
	.remove = tegra_w1_remove,
	.driver = {
		.name = "tegra-w1",
		.of_match_table = tegra_w1_of_match,
		.pm = pm_sleep_ptr(&tegra_w1_pm_ops),
	},
};
module_platform_driver(tegra_w1_driver);

MODULE_DESCRIPTION("NVIDIA Tegra20 OWR 1-wire master driver");
MODULE_AUTHOR("Andrei Warkentin <andreiw@motorola.com>");
MODULE_LICENSE("GPL");
