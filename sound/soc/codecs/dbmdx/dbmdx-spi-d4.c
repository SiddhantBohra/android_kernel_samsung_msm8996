/*
 * DSPG DBMD4 SPI interface driver
 *
 * Copyright (C) 2014 DSP Group
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
/* #define DEBUG */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/firmware.h>

#include "dbmdx-interface.h"
#include "dbmdx-va-regmap.h"
#include "dbmdx-vqe-regmap.h"
#include "dbmdx-spi.h"

#define DBMD4_MAX_SPI_BOOT_SPEED 9000000

static const u8 clr_crc_cmd[] = {0x5A, 0x0F};
static const u8 chng_pll_cmd[] = {0x5A, 0x10, 0, 0xEC, 0x0B, 00};


int spi_set_speed(struct dbmdx_private *p, int index)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	struct spi_device *spi = spi_p->client;
	int ret = 0;
	if (spi->max_speed_hz != p->pdata->va_speed_cfg[index].spi_rate) {

		spi->max_speed_hz = p->pdata->va_speed_cfg[index].spi_rate;

		dev_dbg(spi_p->dev,
			"%s Update SPI Max Speed to %d hz\n",
			__func__, spi->max_speed_hz);

		ret = spi_setup(spi);
		if (ret < 0)
			dev_err(spi_p->dev, "%s:failed %x\n", __func__, ret);
	}

	return ret;
}


static int dbmd4_spi_boot(const struct firmware *fw, struct dbmdx_private *p,
		    const void *checksum, size_t chksum_len, int load_fw)
{
	int retry = RETRY_COUNT;
	int ret = 0;
	ssize_t send_bytes;
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;

	struct spi_device *spi = spi_p->client;
	u8 rx_checksum[7];

	dev_dbg(spi_p->dev, "%s\n", __func__);

	while (retry--) {

		if (p->active_fw == DBMDX_FW_PRE_BOOT) {

			/* reset DBMD4 chip */
			p->reset_sequence(p);

			/* delay before sending commands */
			if (p->clk_get_rate(p, DBMDX_CLK_MASTER) <= 32768)
				msleep(DBMDX_MSLEEP_SPI_D4_AFTER_RESET_32K);
			else
				usleep_range(DBMDX_USLEEP_SPI_D4_AFTER_RESET,
					DBMDX_USLEEP_SPI_D4_AFTER_RESET + 5000);

			ret = spi_set_speed(p, DBMDX_VA_SPEED_MAX);
			if (ret < 0) {
				dev_err(spi_p->dev, "%s:failed %x\n",
					__func__, ret);
				continue;
			}

			if (spi->max_speed_hz > DBMD4_MAX_SPI_BOOT_SPEED) {

				ret = spi_set_speed(p, DBMDX_VA_SPEED_NORMAL);
				if (ret < 0) {
					dev_err(spi_p->dev, "%s:failed %x\n",
						__func__, ret);
					continue;
				}

				/* send CRC clear command */
				ret = send_spi_data(p, chng_pll_cmd,
					sizeof(chng_pll_cmd));
				if (ret != sizeof(chng_pll_cmd)) {
					dev_err(p->dev,
						"%s: failed to clear CRC\n",
						__func__);
					continue;
				}
				msleep(DBMDX_MSLEEP_SPI_D4_AFTER_PLL_CHANGE);

				ret = spi_set_speed(p, DBMDX_VA_SPEED_MAX);
				if (ret < 0) {
					dev_err(spi_p->dev, "%s:failed %x\n",
						__func__, ret);
					continue;
				}
			}
			/* send CRC clear command */
			ret = send_spi_data(p, clr_crc_cmd,
				sizeof(clr_crc_cmd));
			if (ret != sizeof(clr_crc_cmd)) {
				dev_err(p->dev, "%s: failed to clear CRC\n",
					 __func__);
				continue;
			}
		} else {
			ret = send_spi_cmd_va(p,
					DBMDX_VA_SWITCH_TO_BOOT,
					NULL);
			if (ret < 0) {
				dev_err(p->dev,
					"%s: failed to send 'Switch to BOOT' cmd\n",
					 __func__);
				continue;
			}
		}

		if (!load_fw)
			break;

		/* send firmware */
		send_bytes = send_spi_data(p, fw->data, fw->size - 4);
		if (send_bytes != fw->size - 4) {
			dev_err(p->dev,
				"%s: -----------> load firmware error\n",
				__func__);
			continue;
		}

		/* verify checksum */
		if (checksum) {
			ret = send_spi_cmd_boot(spi_p, DBMDX_READ_CHECKSUM);
			if (ret < 0) {
				dev_err(spi_p->dev,
					"%s: could not read checksum\n",
					__func__);
				continue;
			}

			ret = spi_read(spi_p->client, rx_checksum, 7);
			if (ret < 0) {
				dev_err(spi_p->dev,
					"%s: could not read checksum data\n",
					__func__);
				continue;
			}

			ret = p->verify_checksum(
				p, checksum, (void *)(&rx_checksum[3]), 4);
			if (ret) {
				dev_err(p->dev, "%s: checksum mismatch\n",
					__func__);
				continue;
			}
		}


		dev_info(p->dev, "%s: ---------> firmware loaded\n",
			__func__);
		break;
	}

	/* no retries left, failed to boot */
	if (retry < 0) {
		dev_err(p->dev, "%s: failed to load firmware\n", __func__);
		return -1;
	}

	/* send boot command */
	ret = send_spi_cmd_boot(spi_p, DBMDX_FIRMWARE_BOOT);
	if (ret < 0) {
		dev_err(p->dev, "%s: booting the firmware failed\n", __func__);
		return -1;
	}


	ret = spi_set_speed(p, DBMDX_VA_SPEED_NORMAL);
	if (ret < 0)
		dev_err(spi_p->dev, "%s:failed %x\n", __func__, ret);

	return ret;
}

static int dbmd4_spi_reset_post_pll_divider(struct dbmdx_private *p)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = send_spi_cmd_va(p,
				DBMDX_VA_GENERAL_CONFIGURATION_2,
				&spi_p->post_pll_div);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to get post pll divider\n",
			__func__);
		return ret;
	}


	ret = send_spi_cmd_va(p,
			DBMDX_VA_GENERAL_CONFIGURATION_2 |
			(spi_p->post_pll_div & ~DBMDX_POST_PLL_DIV_MASK),
			NULL);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to get post pll divider\n",
			__func__);
		return ret;
	}

	usleep_range(DBMDX_USLEEP_SPI_D4_POST_PLL,
				DBMDX_USLEEP_SPI_D4_POST_PLL + 1000);

	return 0;
}

static int dbmd4_spi_restore_post_pll_divider(struct dbmdx_private *p)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = send_spi_cmd_va(p,
				DBMDX_VA_GENERAL_CONFIGURATION_2 |
				spi_p->post_pll_div,
				NULL);
	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to restore post pll divider\n",
			__func__);
		return ret;
	}

	usleep_range(DBMDX_USLEEP_SPI_D4_POST_PLL,
				DBMDX_USLEEP_SPI_D4_POST_PLL + 1000);

	return 0;
}

static int dbmd4_reconfigure_dsp_clock(struct dbmdx_private *p, int index)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;
	u32 dsp_clock_cfg;
	u16 mic1_val;
	u16 mic2_val;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	dsp_clock_cfg = p->pdata->va_speed_cfg[index].cfg;

	if (!dsp_clock_cfg) {
		dev_dbg(spi_p->dev,
			"%s: dsp clock cfg is not set for rate #%d\n",
			__func__, index);
		return 0;
	}

	ret = send_spi_cmd_va(p,
				DBMDX_VA_MICROPHONE1_CONFIGURATION,
				&mic1_val);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to get mic1 value\n",
			__func__);
		return ret;
	}

	ret = send_spi_cmd_va(p,
				DBMDX_VA_MICROPHONE2_CONFIGURATION,
				&mic2_val);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to get mic2 value\n",
			__func__);
		return ret;
	}

	ret = send_spi_cmd_va(p,
				DBMDX_VA_MICROPHONE1_CONFIGURATION,
				NULL);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to reset mic1 value\n",
			__func__);
		return ret;
	}

	ret = send_spi_cmd_va(p,
				DBMDX_VA_MICROPHONE2_CONFIGURATION,
				NULL);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to reset mic2 value\n",
			__func__);
		return ret;
	}

	ret = send_spi_cmd_va(p,
		DBMDX_VA_CLK_CFG | (dsp_clock_cfg & 0xffff),
		NULL);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to set DBMDX_VA_CLK_CFG\n",
			__func__);
		return ret;
	}

	/* Give to PLL enough time for stabilization */
	msleep(DBMDX_MSLEEP_CONFIG_VA_MODE_REG);

	ret = send_spi_cmd_va(p,
				DBMDX_VA_MICROPHONE1_CONFIGURATION | mic1_val,
				NULL);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to restore mic1 value\n",
			__func__);
		return ret;
	}

	ret = send_spi_cmd_va(p,
				DBMDX_VA_MICROPHONE2_CONFIGURATION | mic2_val,
				NULL);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed to restore mic2 value\n",
			__func__);
		return ret;
	}

	return 0;
}


static int dbmd4_spi_switch_to_buffering_speed(struct dbmdx_private *p,
						bool reconfigure_dsp_clock)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = dbmd4_spi_reset_post_pll_divider(p);

	if (ret < 0) {
		dev_err(spi_p->dev,
			"%s: failed, cannot reset post pll divider\n",
			__func__);
		return ret;
	}

	if (reconfigure_dsp_clock) {

		ret = dbmd4_reconfigure_dsp_clock(p, DBMDX_VA_SPEED_BUFFERING);

		if (ret < 0) {
			dev_err(spi_p->dev,
				"%s: failed to reconfigure dsp clock\n",
				__func__);
			return ret;
		}
	}

	ret = spi_set_speed(p, DBMDX_VA_SPEED_BUFFERING);

	if (ret < 0) {
		dev_err(spi_p->dev, "%s:failed setting speed %x\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int dbmd4_spi_switch_to_normal_speed(struct dbmdx_private *p,
	bool reconfigure_dsp_clock)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = spi_set_speed(p, DBMDX_VA_SPEED_NORMAL);

	if (ret < 0) {
		dev_err(spi_p->dev, "%s:failed setting speed %x\n",
			__func__, ret);
		return ret;
	}

	ret = dbmd4_spi_restore_post_pll_divider(p);

	if (ret < 0) {
		dev_err(p->dev,
			"%s: failed, cannot restore post pll divider\n",
			__func__);
		return ret;
	}

	if (reconfigure_dsp_clock) {

		ret = dbmd4_reconfigure_dsp_clock(p, DBMDX_VA_SPEED_NORMAL);

		if (ret < 0) {
			dev_err(spi_p->dev,
				"%s: failed to reconfigure dsp clock\n",
				__func__);
			return ret;
		}
	}

	return 0;
}

static int dbmd4_spi_prepare_buffering(struct dbmdx_private *p)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = dbmd4_spi_switch_to_buffering_speed(p, false);

	if (ret < 0) {
		dev_err(p->dev,
			"%s: failed to change speed to buffering\n",
			__func__);
		goto out;
	}
out:
	return ret;
}

static int dbmd4_spi_finish_buffering(struct dbmdx_private *p)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = dbmd4_spi_switch_to_normal_speed(p, false);

	if (ret < 0) {
		dev_err(p->dev,
			"%s: failed to change speed to buffering\n",
			__func__);
		goto out;
	}
out:
	return ret;
}


static int dbmd4_spi_prepare_amodel_loading(struct dbmdx_private *p)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = dbmd4_spi_switch_to_buffering_speed(p, true);

	if (ret < 0) {
		dev_err(p->dev,
			"%s: failed to change speed to buffering\n",
			__func__);
		goto out;
	}

out:
	return ret;
}

static int dbmd4_spi_finish_amodel_loading(struct dbmdx_private *p)
{
	struct dbmdx_spi_private *spi_p =
				(struct dbmdx_spi_private *)p->chip->pdata;
	int ret;

	dev_dbg(spi_p->dev, "%s\n", __func__);

	ret = dbmd4_spi_switch_to_normal_speed(p, true);

	if (ret < 0) {
		dev_err(p->dev,
			"%s: failed to change speed to buffering\n",
			__func__);
		goto out;
	}
out:
	return ret;
}

#ifdef CONFIG_PM_SLEEP
static int dbmdx_spi_suspend(struct device *dev)
{
	struct chip_interface *ci = spi_get_drvdata(to_spi_device(dev));
	struct dbmdx_spi_private *spi_p = (struct dbmdx_spi_private *)ci->pdata;

	dev_dbg(dev, "%s\n", __func__);

	spi_interface_suspend(spi_p);

	return 0;
}

static int dbmdx_spi_resume(struct device *dev)
{
	struct chip_interface *ci = spi_get_drvdata(to_spi_device(dev));
	struct dbmdx_spi_private *spi_p = (struct dbmdx_spi_private *)ci->pdata;

	dev_dbg(dev, "%s\n", __func__);
	spi_interface_resume(spi_p);

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM_RUNTIME
static int dbmdx_spi_runtime_suspend(struct device *dev)
{
	struct chip_interface *ci = spi_get_drvdata(to_spi_device(dev));
	struct dbmdx_spi_private *spi_p = (struct dbmdx_spi_private *)ci->pdata;

	dev_dbg(dev, "%s\n", __func__);

	spi_interface_suspend(spi_p);

	return 0;
}

static int dbmdx_spi_runtime_resume(struct device *dev)
{
	struct chip_interface *ci = spi_get_drvdata(to_spi_device(dev));
	struct dbmdx_spi_private *spi_p = (struct dbmdx_spi_private *)ci->pdata;

	dev_dbg(dev, "%s\n", __func__);
	spi_interface_resume(spi_p);

	return 0;
}

#endif /* CONFIG_PM_RUNTIME */

static const struct dev_pm_ops dbmdx_spi_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(dbmdx_spi_suspend, dbmdx_spi_resume)
	SET_RUNTIME_PM_OPS(dbmdx_spi_runtime_suspend,
			   dbmdx_spi_runtime_resume, NULL)
};

static int dbmd4_spi_probe(struct spi_device *client)
{
	int rc;
	struct dbmdx_spi_private *p;
	struct chip_interface *ci;

	rc = spi_common_probe(client);

	if (rc < 0)
		return rc;

	ci = spi_get_drvdata(client);
	p = (struct dbmdx_spi_private *)ci->pdata;

	/* fill in chip interface functions */
	p->chip.prepare_amodel_loading = dbmd4_spi_prepare_amodel_loading;
	p->chip.finish_amodel_loading = dbmd4_spi_finish_amodel_loading;
	p->chip.prepare_buffering = dbmd4_spi_prepare_buffering;
	p->chip.finish_buffering = dbmd4_spi_finish_buffering;
	p->chip.boot = dbmd4_spi_boot;

	return rc;
}


static const struct of_device_id dbmd4_spi_of_match[] = {
	{ .compatible = "dspg,dbmd4-spi", },
	{},
};
MODULE_DEVICE_TABLE(of, dbmd4_spi_of_match);

static const struct spi_device_id dbmd4_spi_id[] = {
	{ "dbmdx-spi", 0 },
	{ "dbmd4-spi", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, dbmd4_spi_id);

static struct spi_driver dbmd4_spi_driver = {
	.driver = {
		.name = "dbmd4-spi",
		.bus	= &spi_bus_type,
		.owner = THIS_MODULE,
		.of_match_table = dbmd4_spi_of_match,
		.pm = &dbmdx_spi_pm,
	},
	.probe =    dbmd4_spi_probe,
	.remove =   spi_common_remove,
	.id_table = dbmd4_spi_id,
};

static int __init dbmd4_modinit(void)
{
	return spi_register_driver(&dbmd4_spi_driver);
}

module_init(dbmd4_modinit);

static void __exit dbmd4_exit(void)
{
	spi_unregister_driver(&dbmd4_spi_driver);
}
module_exit(dbmd4_exit);

MODULE_DESCRIPTION("DSPG DBMD4 spi interface driver");
MODULE_LICENSE("GPL");
