/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <soc/qcom/spm.h>
#include "spm_driver.h"

struct msm_spm_power_modes {
	uint32_t mode;
	bool notify_rpm;
	uint32_t start_addr;
};

struct msm_spm_device {
	struct list_head list;
	bool initialized;
	const char *name;
	struct msm_spm_driver_data reg_data;
	struct msm_spm_power_modes *modes;
	uint32_t num_modes;
	uint32_t cpu_vdd;
	void __iomem *q2s_reg;
};

struct msm_spm_vdd_info {
	uint32_t cpu;
	uint32_t vlevel;
	int err;
};

static LIST_HEAD(spm_list);
static struct msm_spm_device *msm_spm_l2_device;
static DEFINE_PER_CPU_SHARED_ALIGNED(struct msm_spm_device, msm_cpu_spm_device);
static bool msm_spm_L2_apcs_master;

static void msm_spm_smp_set_vdd(void *data)
{
	struct msm_spm_device *dev;
	struct msm_spm_vdd_info *info = (struct msm_spm_vdd_info *)data;

	if (msm_spm_L2_apcs_master)
		dev = msm_spm_l2_device;
	else
		dev = &per_cpu(msm_cpu_spm_device, info->cpu);

	if (!dev->initialized)
		return;

	if (msm_spm_L2_apcs_master)
		get_cpu();

	dev->cpu_vdd = info->vlevel;
	info->err = msm_spm_drv_set_vdd(&dev->reg_data, info->vlevel);

	if (msm_spm_L2_apcs_master)
		put_cpu();
}

/**
 * msm_spm_probe_done(): Verify and return the status of the cpu(s) and l2
 * probe.
 * Return: 0 if all spm devices have been probed, else return -EPROBE_DEFER.
 */
int msm_spm_probe_done(void)
{
	struct msm_spm_device *dev;
	int cpu;

	if (msm_spm_L2_apcs_master && !msm_spm_l2_device) {
		return -EPROBE_DEFER;
	} else {
		for_each_possible_cpu(cpu) {
			dev = &per_cpu(msm_cpu_spm_device, cpu);
			if (!dev->initialized)
				return -EPROBE_DEFER;
		}
	}

	return 0;
}
EXPORT_SYMBOL(msm_spm_probe_done);

/**
 * msm_spm_set_vdd(): Set core voltage
 * @cpu: core id
 * @vlevel: Encoded PMIC data.
 */
int msm_spm_set_vdd(unsigned int cpu, unsigned int vlevel)
{
	struct msm_spm_vdd_info info;
	int ret;
	int current_cpu;

	info.cpu = cpu;
	info.vlevel = vlevel;
	info.err = -ENODEV;

	current_cpu = get_cpu();
	if (!msm_spm_L2_apcs_master && (current_cpu != cpu) &&
			cpu_online(cpu)) {
		/**
		 * We do not want to set the voltage of another core from
		 * this core, as its possible that we may race the vdd change
		 * with the SPM state machine of that core, which could also
		 * be changing the voltage of that core during power collapse.
		 * Hence, set the function to be executed on that core and block
		 * until the vdd change is complete.
		 */
		ret = smp_call_function_single(cpu, msm_spm_smp_set_vdd,
				&info, true);
		if (!ret)
			ret = info.err;
	} else {
		/**
		 * Since the core is not online, it is safe to set the vdd
		 * directly.
		 */
		msm_spm_smp_set_vdd(&info);
		ret = info.err;
	}
	put_cpu();

	return ret;
}
EXPORT_SYMBOL(msm_spm_set_vdd);

/**
 * msm_spm_get_vdd(): Get core voltage
 * @cpu: core id
 * @return: Returns encoded PMIC data.
 */
unsigned int msm_spm_get_vdd(unsigned int cpu)
{
	struct msm_spm_device *dev;

	if (msm_spm_L2_apcs_master)
		dev = msm_spm_l2_device;
	else
		dev = &per_cpu(msm_cpu_spm_device, cpu);
	return dev->cpu_vdd;
}
EXPORT_SYMBOL(msm_spm_get_vdd);

static void msm_spm_config_q2s(struct msm_spm_device *dev, unsigned int mode)
{
	uint32_t spm_legacy_mode = 0;
	uint32_t qchannel_ignore = 0;
	uint32_t val = 0;

	if (!dev->q2s_reg)
		return;

	switch (mode) {
	case MSM_SPM_MODE_DISABLED:
	case MSM_SPM_MODE_CLOCK_GATING:
		qchannel_ignore = 1;
		spm_legacy_mode = 0;
		break;
	case MSM_SPM_MODE_RETENTION:
		qchannel_ignore = 0;
		spm_legacy_mode = 0;
		break;
	case MSM_SPM_MODE_GDHS:
	case MSM_SPM_MODE_POWER_COLLAPSE:
		qchannel_ignore = 1;
		spm_legacy_mode = 1;
		break;
	default:
		break;
	}

	val = spm_legacy_mode << 2 | qchannel_ignore << 1;
	__raw_writel(val, dev->q2s_reg);
	mb();
}

static int msm_spm_dev_set_low_power_mode(struct msm_spm_device *dev,
		unsigned int mode, bool notify_rpm)
{
	uint32_t i;
	uint32_t start_addr = 0;
	int ret = -EINVAL;
	bool pc_mode = false;

	if (!dev->initialized)
		return -ENXIO;

	if ((mode == MSM_SPM_MODE_POWER_COLLAPSE)
			|| (mode == MSM_SPM_MODE_GDHS))
		pc_mode = true;

	if (mode == MSM_SPM_MODE_DISABLED) {
		ret = msm_spm_drv_set_spm_enable(&dev->reg_data, false);
	} else if (!msm_spm_drv_set_spm_enable(&dev->reg_data, true)) {
		for (i = 0; i < dev->num_modes; i++) {
			if ((dev->modes[i].mode == mode) &&
				(dev->modes[i].notify_rpm == notify_rpm)) {
				start_addr = dev->modes[i].start_addr;
				break;
			}
		}
		ret = msm_spm_drv_set_low_power_mode(&dev->reg_data,
					start_addr, pc_mode);
	}

	msm_spm_config_q2s(dev, mode);

	return ret;
}

static int msm_spm_dev_init(struct msm_spm_device *dev,
		struct msm_spm_platform_data *data)
{
	int i, ret = -ENOMEM;
	uint32_t offset = 0;

	dev->num_modes = data->num_modes;
	dev->modes = kmalloc(
			sizeof(struct msm_spm_power_modes) * dev->num_modes,
			GFP_KERNEL);

	if (!dev->modes)
		goto spm_failed_malloc;

	dev->reg_data.ver_reg = data->ver_reg;
	ret = msm_spm_drv_init(&dev->reg_data, data);

	if (ret)
		goto spm_failed_init;

	for (i = 0; i < dev->num_modes; i++) {

		/* Default offset is 0 and gets updated as we write more
		 * sequences into SPM
		 */
		dev->modes[i].start_addr = offset;
		ret = msm_spm_drv_write_seq_data(&dev->reg_data,
						data->modes[i].cmd, &offset);
		if (ret < 0)
			goto spm_failed_init;

		dev->modes[i].mode = data->modes[i].mode;
		dev->modes[i].notify_rpm = data->modes[i].notify_rpm;
	}
	msm_spm_drv_flush_seq_entry(&dev->reg_data);
	dev->initialized = true;
	return 0;

spm_failed_init:
	kfree(dev->modes);
spm_failed_malloc:
	return ret;
}

/**
 * msm_spm_turn_on_cpu_rail(): Power on cpu rail before turning on core
 * @base: core 0's base SAW address
 * @cpu: core id
 */
int msm_spm_turn_on_cpu_rail(unsigned long base, unsigned int cpu)
{
	uint32_t val = 0;
	uint32_t timeout = 512; /* delay for voltage to settle on the core */
	void *reg = NULL;

	if (cpu == 0 || cpu >= num_possible_cpus())
		return -EINVAL;

	reg = ioremap_nocache(base + (cpu * 0x10000), SZ_4K);
	if (!reg)
		return -ENOMEM;

	reg += 0x1C;

	/*
	 * Set FTS2 type CPU supply regulator to 1.15 V. This assumes that the
	 * regulator is already configured in LV range.
	 */
	val = 0x40000E6;
	writel_relaxed(val, reg);
	mb();
	udelay(timeout);

	/* Enable CPU supply regulator */
	val = 0x2030080;
	writel_relaxed(val, reg);
	mb();
	udelay(timeout);

	iounmap(reg);

	return 0;
}
EXPORT_SYMBOL(msm_spm_turn_on_cpu_rail);

void msm_spm_reinit(void)
{
	unsigned int cpu;
	for_each_possible_cpu(cpu)
		msm_spm_drv_reinit(&per_cpu(msm_cpu_spm_device.reg_data, cpu));
}
EXPORT_SYMBOL(msm_spm_reinit);

/**
 * msm_spm_set_low_power_mode() - Configure SPM start address for low power mode
 * @mode: SPM LPM mode to enter
 * @notify_rpm: Notify RPM in this mode
 */
int msm_spm_set_low_power_mode(unsigned int mode, bool notify_rpm)
{
	struct msm_spm_device *dev = &__get_cpu_var(msm_cpu_spm_device);
	return msm_spm_dev_set_low_power_mode(dev, mode, notify_rpm);
}
EXPORT_SYMBOL(msm_spm_set_low_power_mode);

/**
 * msm_spm_init(): Board initalization function
 * @data: platform specific SPM register configuration data
 * @nr_devs: Number of SPM devices being initialized
 */
int __init msm_spm_init(struct msm_spm_platform_data *data, int nr_devs)
{
	unsigned int cpu;
	int ret = 0;

	BUG_ON((nr_devs < num_possible_cpus()) || !data);

	for_each_possible_cpu(cpu) {
		struct msm_spm_device *dev = &per_cpu(msm_cpu_spm_device, cpu);
		ret = msm_spm_dev_init(dev, &data[cpu]);
		if (ret < 0) {
			pr_warn("%s():failed CPU:%u ret:%d\n", __func__,
					cpu, ret);
			break;
		}
	}

	return ret;
}

struct msm_spm_device *msm_spm_get_device_by_name(const char *name)
{
	struct list_head *list;

	list_for_each(list, &spm_list) {
		struct msm_spm_device *dev
			= list_entry(list, typeof(*dev), list);
		if (dev->name && !strcmp(dev->name, name))
			return dev;
	}
	return ERR_PTR(-ENODEV);
}

int msm_spm_config_low_power_mode(struct msm_spm_device *dev,
		unsigned int mode, bool notify_rpm)
{
	return msm_spm_dev_set_low_power_mode(dev, mode, notify_rpm);
}
#ifdef CONFIG_MSM_L2_SPM

/**
 * msm_spm_apcs_set_phase(): Set number of SMPS phases.
 * phase_cnt: Number of phases to be set active
 */
int msm_spm_apcs_set_phase(unsigned int phase_cnt)
{
	if (!msm_spm_l2_device || !msm_spm_l2_device->initialized)
		return -ENXIO;
	return msm_spm_drv_set_pmic_data(&msm_spm_l2_device->reg_data,
			MSM_SPM_PMIC_PHASE_PORT, phase_cnt);
}
EXPORT_SYMBOL(msm_spm_apcs_set_phase);

/** msm_spm_enable_fts_lpm() : Enable FTS to switch to low power
 *                             when the cores are in low power modes
 * @mode: The mode configuration for FTS
 */
int msm_spm_enable_fts_lpm(uint32_t mode)
{
	if (!msm_spm_l2_device || !msm_spm_l2_device->initialized)
		return -ENXIO;
	return msm_spm_drv_set_pmic_data(&msm_spm_l2_device->reg_data,
			MSM_SPM_PMIC_PFM_PORT, mode);
}
EXPORT_SYMBOL(msm_spm_enable_fts_lpm);

#endif

static int get_cpu_id(struct device_node *node)
{
	struct device_node *cpu_node;
	u32 cpu;
	int ret = -EINVAL;
	char *key = "qcom,cpu";

	cpu_node = of_parse_phandle(node, key, 0);
	if (cpu_node) {
		for_each_possible_cpu(cpu) {
			if (of_get_cpu_node(cpu, NULL) == cpu_node)
				return cpu;
		}
	} else {
		char *key = "qcom,core-id";

		ret = of_property_read_u32(node, key, &cpu);
		if (!ret)
			return cpu;
	}
	return ret;
}

static struct msm_spm_device *msm_spm_get_device(struct platform_device *pdev)
{
	struct msm_spm_device *dev = NULL;
	const char *val = NULL;
	char *key = "qcom,name";
	int cpu = get_cpu_id(pdev->dev.of_node);

	if ((cpu >= 0) && cpu < num_possible_cpus()) {
		dev = &per_cpu(msm_cpu_spm_device, cpu);
	} else if (cpu == 0xffff) {
		dev = devm_kzalloc(&pdev->dev, sizeof(struct msm_spm_device),
				GFP_KERNEL);
		msm_spm_l2_device = dev;
	}
	if (!dev)
		return NULL;

	if (of_property_read_string(pdev->dev.of_node, key, &val)) {
		pr_err("%s(): Cannot find a required node key:%s\n",
				__func__, key);
		return NULL;
	}
	dev->name = val;
	list_add(&dev->list, &spm_list);

	return dev;
}

static int msm_spm_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	int cpu = 0;
	int i = 0;
	struct device_node *node = pdev->dev.of_node;
	struct msm_spm_platform_data spm_data;
	char *key = NULL;
	uint32_t val = 0;
	struct msm_spm_seq_entry modes[MSM_SPM_MODE_NR];
	int len = 0;
	struct msm_spm_device *dev = NULL;
	struct resource *res = NULL;
	uint32_t mode_count = 0;

	struct spm_of {
		char *key;
		uint32_t id;
	};

	struct spm_of spm_of_data[] = {
		{"qcom,saw2-cfg", MSM_SPM_REG_SAW2_CFG},
		{"qcom,saw2-avs-ctl", MSM_SPM_REG_SAW2_AVS_CTL},
		{"qcom,saw2-avs-hysteresis", MSM_SPM_REG_SAW2_AVS_HYSTERESIS},
		{"qcom,saw2-avs-limit", MSM_SPM_REG_SAW2_AVS_LIMIT},
		{"qcom,saw2-avs-dly", MSM_SPM_REG_SAW2_AVS_DLY},
		{"qcom,saw2-spm-dly", MSM_SPM_REG_SAW2_SPM_DLY},
		{"qcom,saw2-spm-ctl", MSM_SPM_REG_SAW2_SPM_CTL},
		{"qcom,saw2-pmic-data0", MSM_SPM_REG_SAW2_PMIC_DATA_0},
		{"qcom,saw2-pmic-data1", MSM_SPM_REG_SAW2_PMIC_DATA_1},
		{"qcom,saw2-pmic-data2", MSM_SPM_REG_SAW2_PMIC_DATA_2},
		{"qcom,saw2-pmic-data3", MSM_SPM_REG_SAW2_PMIC_DATA_3},
		{"qcom,saw2-pmic-data4", MSM_SPM_REG_SAW2_PMIC_DATA_4},
		{"qcom,saw2-pmic-data5", MSM_SPM_REG_SAW2_PMIC_DATA_5},
		{"qcom,saw2-pmic-data6", MSM_SPM_REG_SAW2_PMIC_DATA_6},
		{"qcom,saw2-pmic-data7", MSM_SPM_REG_SAW2_PMIC_DATA_7},
	};

	struct mode_of {
		char *key;
		uint32_t id;
		uint32_t notify_rpm;
	};

	struct mode_of mode_of_data[] = {
		{"qcom,saw2-spm-cmd-wfi", MSM_SPM_MODE_CLOCK_GATING, 0},
		{"qcom,saw2-spm-cmd-ret", MSM_SPM_MODE_RETENTION, 0},
		{"qcom,saw2-spm-cmd-gdhs", MSM_SPM_MODE_GDHS, 1},
		{"qcom,saw2-spm-cmd-spc", MSM_SPM_MODE_POWER_COLLAPSE, 0},
		{"qcom,saw2-spm-cmd-pc", MSM_SPM_MODE_POWER_COLLAPSE, 1},
	};

	memset(&spm_data, 0, sizeof(struct msm_spm_platform_data));
	memset(&modes, 0,
		(MSM_SPM_MODE_NR - 2) * sizeof(struct msm_spm_seq_entry));

	key = "qcom,saw2-ver-reg";
	ret = of_property_read_u32(node, key, &val);
	if (ret)
		goto fail;
	spm_data.ver_reg = val;

	key = "qcom,vctl-timeout-us";
	ret = of_property_read_u32(node, key, &val);
	if (!ret)
		spm_data.vctl_timeout_us = val;

	/* SAW start address */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		goto fail;

	spm_data.reg_base_addr = devm_ioremap(&pdev->dev, res->start,
					resource_size(res));
	if (!spm_data.reg_base_addr)
		return -ENOMEM;

	spm_data.vctl_port = -1;
	spm_data.phase_port = -1;
	spm_data.pfm_port = -1;

	key = "qcom,vctl-port";
	of_property_read_u32(node, key, &spm_data.vctl_port);

	key = "qcom,phase-port";
	of_property_read_u32(node, key, &spm_data.phase_port);

	key = "qcom,pfm-port";
	of_property_read_u32(node, key, &spm_data.pfm_port);

	dev = msm_spm_get_device(pdev);
	if (!dev)
		return -EINVAL;

	/* Q2S (QChannel-2-SPM) register */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		dev->q2s_reg = devm_ioremap(&pdev->dev, res->start,
						resource_size(res));
		if (!dev->q2s_reg) {
			pr_err("%s(): Unable to iomap Q2S register\n",
					__func__);
			return -EADDRNOTAVAIL;
		}
	}

	/* optional */
	if (dev == msm_spm_l2_device) {
		key = "qcom,L2-spm-is-apcs-master";
		msm_spm_L2_apcs_master =
			of_property_read_bool(pdev->dev.of_node, key);
	}

	for (i = 0; i < ARRAY_SIZE(spm_of_data); i++) {
		ret = of_property_read_u32(node, spm_of_data[i].key, &val);
		if (ret)
			continue;
		spm_data.reg_init_values[spm_of_data[i].id] = val;
	}

	for (i = 0; i < ARRAY_SIZE(mode_of_data); i++) {
		key = mode_of_data[i].key;
		modes[mode_count].cmd =
			(uint8_t *)of_get_property(node, key, &len);
		if (!modes[mode_count].cmd)
			continue;
		modes[mode_count].mode = mode_of_data[i].id;
		modes[mode_count].notify_rpm = mode_of_data[i].notify_rpm;
		pr_debug("%s(): dev: %s cmd:%s, mode:%d rpm:%d\n", __func__,
				dev->name, key, modes[mode_count].mode,
				modes[mode_count].notify_rpm);
		mode_count++;
	}

	spm_data.modes = modes;
	spm_data.num_modes = mode_count;

	ret = msm_spm_dev_init(dev, &spm_data);
	platform_set_drvdata(pdev, dev);

	if (ret < 0)
		pr_warn("%s():failed core-id:%u ret:%d\n", __func__, cpu, ret);

	return ret;

fail:
	pr_err("%s: Failed reading node=%s, key=%s\n",
			__func__, node->full_name, key);
	return -EFAULT;
}

static int msm_spm_dev_remove(struct platform_device *pdev)
{
	struct msm_spm_device *dev = platform_get_drvdata(pdev);
	list_del(&dev->list);
	return 0;
}

static struct of_device_id msm_spm_match_table[] = {
	{.compatible = "qcom,spm-v2"},
	{},
};

static struct platform_driver msm_spm_device_driver = {
	.probe = msm_spm_dev_probe,
	.remove = msm_spm_dev_remove,
	.driver = {
		.name = "spm-v2",
		.owner = THIS_MODULE,
		.of_match_table = msm_spm_match_table,
	},
};

/**
 * msm_spm_device_init(): Device tree initialization function
 */
int __init msm_spm_device_init(void)
{
	static bool registered;
	if (registered)
		return 0;
	registered = true;
	return platform_driver_register(&msm_spm_device_driver);
}
arch_initcall(msm_spm_device_init);