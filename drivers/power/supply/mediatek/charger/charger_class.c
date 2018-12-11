/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/module.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/slab.h>

#include <mt-plat/charger_class.h>

static struct class *charger_class;

static ssize_t charger_show_name(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct charger_device *chg_dev = to_charger_device(dev);

	return snprintf(buf, 20, "%s\n",
		       chg_dev->props.alias_name ?
		       chg_dev->props.alias_name : "anonymous");
}

static int charger_suspend(struct device *dev, pm_message_t state)
{
	struct charger_device *chg_dev = to_charger_device(dev);

	if (chg_dev->ops->suspend)
		return chg_dev->ops->suspend(chg_dev, state);

	return 0;
}

static int charger_resume(struct device *dev)
{
	struct charger_device *chg_dev = to_charger_device(dev);

	if (chg_dev->ops->resume)
		return chg_dev->ops->resume(chg_dev);

	return 0;
}

static void charger_device_release(struct device *dev)
{
	struct charger_device *chg_dev = to_charger_device(dev);

	kfree(chg_dev);
}

int charger_dev_enable(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->enable)
		return chg_dev->ops->enable(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable);

int charger_dev_is_enabled(struct charger_device *chg_dev, bool *en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->is_enabled)
		return chg_dev->ops->is_enabled(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_is_enabled);

int charger_dev_plug_in(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->plug_in)
		return chg_dev->ops->plug_in(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_plug_in);

int charger_dev_plug_out(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->plug_out)
		return chg_dev->ops->plug_out(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_plug_out);

int charger_dev_do_event(struct charger_device *chg_dev, u32 event, u32 args)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->event)
		return chg_dev->ops->event(chg_dev, event, args);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_do_event);

int charger_dev_set_charging_current(struct charger_device *chg_dev, u32 uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_charging_current)
		return chg_dev->ops->set_charging_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_charging_current);

int charger_dev_get_charging_current(struct charger_device *chg_dev, u32 *uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_charging_current)
		return chg_dev->ops->get_charging_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_charging_current);

int charger_dev_get_min_charging_current(struct charger_device *chg_dev,
					 u32 *uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_min_charging_current)
		return chg_dev->ops->get_min_charging_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_min_charging_current);

int charger_dev_enable_chip(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_chip)
		return chg_dev->ops->enable_chip(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_chip);

int charger_dev_is_chip_enabled(struct charger_device *chg_dev, bool *en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->is_chip_enabled)
		return chg_dev->ops->is_chip_enabled(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_is_chip_enabled);

int charger_dev_enable_direct_charging(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_direct_charging)
		return chg_dev->ops->enable_direct_charging(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_direct_charging);

int charger_dev_kick_direct_charging_wdt(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->kick_direct_charging_wdt)
		return chg_dev->ops->kick_direct_charging_wdt(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_kick_direct_charging_wdt);

int charger_dev_get_vbus(struct charger_device *chg_dev, u32 *vbus)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_vbus_adc)
		return chg_dev->ops->get_vbus_adc(chg_dev, vbus);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_vbus);

int charger_dev_get_ibus(struct charger_device *chg_dev, u32 *ibus)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_ibus_adc)
		return chg_dev->ops->get_ibus_adc(chg_dev, ibus);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_ibus);

int charger_dev_get_temperature(struct charger_device *chg_dev, int *tchg_min,
		int *tchg_max)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_tchg_adc)
		return chg_dev->ops->get_tchg_adc(chg_dev, tchg_min,
						  tchg_max);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_temperature);

int charger_dev_set_input_current(struct charger_device *chg_dev, u32 uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_input_current)
		return chg_dev->ops->set_input_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_input_current);

int charger_dev_get_input_current(struct charger_device *chg_dev, u32 *uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_input_current)
		return chg_dev->ops->get_input_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_input_current);

int charger_dev_get_min_input_current(struct charger_device *chg_dev, u32 *uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_min_input_current)
		return chg_dev->ops->get_min_input_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_min_input_current);

int charger_dev_set_eoc_current(struct charger_device *chg_dev, u32 uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_eoc_current)
		chg_dev->ops->set_eoc_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_eoc_current);

int charger_dev_get_eoc_current(struct charger_device *chg_dev, u32 *uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_eoc_current)
		return chg_dev->ops->get_eoc_current(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_eoc_current);

int charger_dev_kick_wdt(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->kick_wdt)
		return chg_dev->ops->kick_wdt(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_kick_wdt);

int charger_dev_set_constant_voltage(struct charger_device *chg_dev, u32 uV)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_constant_voltage)
		return chg_dev->ops->set_constant_voltage(chg_dev, uV);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_constant_voltage);

int charger_dev_get_constant_voltage(struct charger_device *chg_dev, u32 *uV)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_constant_voltage)
		return chg_dev->ops->get_constant_voltage(chg_dev, uV);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_constant_voltage);

int charger_dev_dump_registers(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->dump_registers)
		return chg_dev->ops->dump_registers(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_dump_registers);

int charger_dev_is_charging_done(struct charger_device *chg_dev, bool *done)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->is_charging_done)
		return chg_dev->ops->is_charging_done(chg_dev, done);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_is_charging_done);

int charger_dev_enable_vbus_ovp(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_vbus_ovp)
		return chg_dev->ops->enable_vbus_ovp(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_vbus_ovp);

int charger_dev_set_mivr(struct charger_device *chg_dev, u32 uV)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->set_mivr)
		return chg_dev->ops->set_mivr(chg_dev, uV);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_mivr);

int charger_dev_enable_powerpath(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_powerpath)
		return chg_dev->ops->enable_powerpath(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_powerpath);

int charger_dev_is_powerpath_enabled(struct charger_device *chg_dev, bool *en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->is_powerpath_enabled)
		return chg_dev->ops->is_powerpath_enabled(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_is_powerpath_enabled);

int charger_dev_enable_safety_timer(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_safety_timer)
		return chg_dev->ops->enable_safety_timer(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_safety_timer);

int charger_dev_is_safety_timer_enabled(struct charger_device *chg_dev,
					bool *en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->is_safety_timer_enabled)
		return chg_dev->ops->is_safety_timer_enabled(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_is_safety_timer_enabled);

int charger_dev_enable_termination(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_termination)
		return chg_dev->ops->enable_termination(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_termination);

int charger_dev_get_mivr_state(struct charger_device *chg_dev, bool *in_loop)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->get_mivr_state)
		return chg_dev->ops->get_mivr_state(chg_dev, in_loop);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_mivr_state);

int charger_dev_send_ta_current_pattern(struct charger_device *chg_dev,
					bool is_increase)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->send_ta_current_pattern)
		return chg_dev->ops->send_ta_current_pattern(chg_dev,
							     is_increase);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_send_ta_current_pattern);

int charger_dev_send_ta20_current_pattern(struct charger_device *chg_dev,
					  u32 uV)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->send_ta20_current_pattern)
		return chg_dev->ops->send_ta20_current_pattern(chg_dev, uV);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_send_ta20_current_pattern);

int charger_dev_reset_ta(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->reset_ta)
		return chg_dev->ops->reset_ta(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_reset_ta);

int charger_dev_set_pe20_efficiency_table(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_pe20_efficiency_table)
		return chg_dev->ops->set_pe20_efficiency_table(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_pe20_efficiency_table);

int charger_dev_enable_cable_drop_comp(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_cable_drop_comp)
		return chg_dev->ops->enable_cable_drop_comp(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_cable_drop_comp);

int charger_dev_set_direct_charging_ibusoc(struct charger_device *chg_dev,
					   u32 uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_direct_charging_ibusoc)
		return chg_dev->ops->set_direct_charging_ibusoc(chg_dev,
								uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_direct_charging_ibusoc);

int charger_dev_set_direct_charging_vbusov(struct charger_device *chg_dev,
					   u32 uV)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_direct_charging_vbusov)
		return chg_dev->ops->set_direct_charging_vbusov(chg_dev,
								uV);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_direct_charging_vbusov);

int charger_dev_enable_chg_type_det(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_chg_type_det)
		return chg_dev->ops->enable_chg_type_det(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_chg_type_det);

int charger_dev_enable_otg(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->enable_otg)
		return chg_dev->ops->enable_otg(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_otg);

int charger_dev_enable_discharge(struct charger_device *chg_dev, bool en)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->enable_discharge)
		return chg_dev->ops->enable_discharge(chg_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_discharge);

int charger_dev_set_boost_current_limit(struct charger_device *chg_dev, u32 uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->set_boost_current_limit)
		return chg_dev->ops->set_boost_current_limit(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_set_boost_current_limit);

int charger_dev_get_zcv(struct charger_device *chg_dev, u32 *uV)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->get_zcv)
		return chg_dev->ops->get_zcv(chg_dev, uV);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_zcv);

int charger_dev_run_aicl(struct charger_device *chg_dev, u32 *uA)
{
	if (chg_dev != NULL && chg_dev->ops != NULL && chg_dev->ops->run_aicl)
		return chg_dev->ops->run_aicl(chg_dev, uA);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_run_aicl);

int charger_dev_reset_eoc_state(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->reset_eoc_state)
		return chg_dev->ops->reset_eoc_state(chg_dev);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_reset_eoc_state);

int charger_dev_safety_check(struct charger_device *chg_dev)
{
	if (chg_dev != NULL && chg_dev->ops != NULL &&
	    chg_dev->ops->safety_check)
		return chg_dev->ops->safety_check(chg_dev);

	return -ENOTSUPP;
}

int charger_dev_notify(struct charger_device *chg_dev, int event)
{
	return srcu_notifier_call_chain(
		&chg_dev->evt_nh, event, &chg_dev->noti);
}

int charger_dev_get_fod_status(struct charger_device *charger_dev, u8 *status)
{
	if (charger_dev != NULL && charger_dev->ops != NULL &&
					       charger_dev->ops->get_fod_status)
		return charger_dev->ops->get_fod_status(charger_dev, status);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_get_fod_status);

int charger_dev_enable_fod_oneshot(struct charger_device *charger_dev, bool en)
{
	if (charger_dev != NULL && charger_dev->ops != NULL &&
					   charger_dev->ops->enable_fod_oneshot)
		return charger_dev->ops->enable_fod_oneshot(charger_dev, en);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_enable_fod_oneshot);

int charger_dev_is_typec_ot(struct charger_device *charger_dev, bool *ot)
{
	if (charger_dev != NULL && charger_dev->ops != NULL &&
						  charger_dev->ops->is_typec_ot)
		return charger_dev->ops->is_typec_ot(charger_dev, ot);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(charger_dev_is_typec_ot);

static DEVICE_ATTR(name, 0444, charger_show_name, NULL);

static struct attribute *charger_class_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};

static const struct attribute_group charger_group = {
	.attrs = charger_class_attrs,
};

static const struct attribute_group *charger_groups[] = {
	&charger_group,
	NULL,
};

int register_charger_device_notifier(struct charger_device *chg_dev,
				struct notifier_block *nb)
{
	int ret;

	ret = srcu_notifier_chain_register(&chg_dev->evt_nh, nb);
	return ret;
}
EXPORT_SYMBOL(register_charger_device_notifier);

int unregister_charger_device_notifier(struct charger_device *chg_dev,
				struct notifier_block *nb)
{
	return srcu_notifier_chain_unregister(&chg_dev->evt_nh, nb);
}
EXPORT_SYMBOL(unregister_charger_device_notifier);

/**
 * charger_device_register - create and register a new object of
 *   charger_device class.
 * @name: the name of the new object
 * @parent: a pointer to the parent device
 * @devdata: an optional pointer to be stored for private driver use.
 * The methods may retrieve it by using charger_get_data(charger_dev).
 * @ops: the charger operations structure.
 *
 * Creates and registers new charger device. Returns either an
 * ERR_PTR() or a pointer to the newly allocated device.
 */
struct charger_device *charger_device_register(const char *name,
		struct device *parent, void *devdata,
		const struct charger_ops *ops,
		const struct charger_properties *props)
{
	struct charger_device *chg_dev;
	static struct lock_class_key key;
	struct srcu_notifier_head *head;
	int rc;

	pr_debug("charger_device_register: name=%s\n", name);
	chg_dev = kzalloc(sizeof(*chg_dev), GFP_KERNEL);
	if (!chg_dev)
		return ERR_PTR(-ENOMEM);

	mutex_init(&chg_dev->ops_lock);
	chg_dev->dev.class = charger_class;
	chg_dev->dev.parent = parent;
	chg_dev->dev.release = charger_device_release;
	dev_set_name(&chg_dev->dev, name);
	dev_set_drvdata(&chg_dev->dev, devdata);
	head = &chg_dev->evt_nh;
	srcu_init_notifier_head(head);
	/* Rename srcu's lock to avoid LockProve warning */
	lockdep_init_map(&(&head->srcu)->dep_map, name, &key, 0);

	/* Copy properties */
	if (props) {
		memcpy(&chg_dev->props, props,
		       sizeof(struct charger_properties));
	}
	rc = device_register(&chg_dev->dev);
	if (rc) {
		kfree(chg_dev);
		return ERR_PTR(rc);
	}
	chg_dev->ops = ops;
	return chg_dev;
}
EXPORT_SYMBOL(charger_device_register);

/**
 * charger_device_unregister - unregisters a switching charger device
 * object.
 * @charger_dev: the switching charger device object to be unregistered
 * and freed.
 *
 * Unregisters a previously registered via charger_device_register object.
 */
void charger_device_unregister(struct charger_device *chg_dev)
{
	if (!chg_dev)
		return;

	mutex_lock(&chg_dev->ops_lock);
	chg_dev->ops = NULL;
	mutex_unlock(&chg_dev->ops_lock);
	device_unregister(&chg_dev->dev);
}
EXPORT_SYMBOL(charger_device_unregister);


static int charger_match_device_by_name(struct device *dev,
	const void *data)
{
	const char *name = data;

	return strcmp(dev_name(dev), name) == 0;
}

struct charger_device *get_charger_by_name(const char *name)
{
	struct device *dev;

	if (!name)
		return (struct charger_device *)NULL;
	dev = class_find_device(charger_class, NULL, name,
				charger_match_device_by_name);

	return dev ? to_charger_device(dev) : NULL;

}
EXPORT_SYMBOL(get_charger_by_name);

static void __exit charger_class_exit(void)
{
	class_destroy(charger_class);
}

static int __init charger_class_init(void)
{
	charger_class = class_create(THIS_MODULE, "switching_charger");
	if (IS_ERR(charger_class)) {
		pr_notice("Unable to create charger class; errno = %ld\n",
			PTR_ERR(charger_class));
		return PTR_ERR(charger_class);
	}
	charger_class->dev_groups = charger_groups;
	charger_class->suspend = charger_suspend;
	charger_class->resume = charger_resume;
	return 0;
}

subsys_initcall(charger_class_init);
module_exit(charger_class_exit);

MODULE_DESCRIPTION("Switching Charger Class Device");
MODULE_AUTHOR("Patrick Chang <patrick_chang@richtek.com>");
MODULE_VERSION("1.0.0_G");
MODULE_LICENSE("GPL");
