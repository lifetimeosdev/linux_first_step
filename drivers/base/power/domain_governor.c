// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/power/domain_governor.c - Governors for device PM domains.
 *
 * Copyright (C) 2011 Rafael J. Wysocki <rjw@sisk.pl>, Renesas Electronics Corp.
 */
#include <linux/kernel.h>
#include <linux/pm_domain.h>
#include <linux/pm_qos.h>
#include <linux/hrtimer.h>
#include <linux/cpuidle.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>

static int dev_update_qos_constraint(struct device *dev, void *data)
{
	s64 *constraint_ns_p = data;
	s64 constraint_ns;

	if (dev->power.subsys_data && dev->power.subsys_data->domain_data) {
		/*
		 * Only take suspend-time QoS constraints of devices into
		 * account, because constraints updated after the device has
		 * been suspended are not guaranteed to be taken into account
		 * anyway.  In order for them to take effect, the device has to
		 * be resumed and suspended again.
		 */
		constraint_ns = dev_gpd_data(dev)->td.effective_constraint_ns;
	} else {
		/*
		 * The child is not in a domain and there's no info on its
		 * suspend/resume latencies, so assume them to be negligible and
		 * take its current PM QoS constraint (that's the only thing
		 * known at this point anyway).
		 */
		constraint_ns = dev_pm_qos_read_value(dev, DEV_PM_QOS_RESUME_LATENCY);
		constraint_ns *= NSEC_PER_USEC;
	}

	if (constraint_ns < *constraint_ns_p)
		*constraint_ns_p = constraint_ns;

	return 0;
}

/**
 * default_suspend_ok - Default PM domain governor routine to suspend devices.
 * @dev: Device to check.
 */
static bool default_suspend_ok(struct device *dev)
{
	struct gpd_timing_data *td = &dev_gpd_data(dev)->td;
	unsigned long flags;
	s64 constraint_ns;

	dev_dbg(dev, "%s()\n", __func__);

	spin_lock_irqsave(&dev->power.lock, flags);

	if (!td->constraint_changed) {
		bool ret = td->cached_suspend_ok;

		spin_unlock_irqrestore(&dev->power.lock, flags);
		return ret;
	}
	td->constraint_changed = false;
	td->cached_suspend_ok = false;
	td->effective_constraint_ns = 0;
	constraint_ns = __dev_pm_qos_resume_latency(dev);

	spin_unlock_irqrestore(&dev->power.lock, flags);

	if (constraint_ns == 0)
		return false;

	constraint_ns *= NSEC_PER_USEC;
	/*
	 * We can walk the children without any additional locking, because
	 * they all have been suspended at this point and their
	 * effective_constraint_ns fields won't be modified in parallel with us.
	 */
	if (!dev->power.ignore_children)
		device_for_each_child(dev, &constraint_ns,
				      dev_update_qos_constraint);

	if (constraint_ns == PM_QOS_RESUME_LATENCY_NO_CONSTRAINT_NS) {
		/* "No restriction", so the device is allowed to suspend. */
		td->effective_constraint_ns = PM_QOS_RESUME_LATENCY_NO_CONSTRAINT_NS;
		td->cached_suspend_ok = true;
	} else if (constraint_ns == 0) {
		/*
		 * This triggers if one of the children that don't belong to a
		 * domain has a zero PM QoS constraint and it's better not to
		 * suspend then.  effective_constraint_ns is zero already and
		 * cached_suspend_ok is false, so bail out.
		 */
		return false;
	} else {
		constraint_ns -= td->suspend_latency_ns +
				td->resume_latency_ns;
		/*
		 * effective_constraint_ns is zero already and cached_suspend_ok
		 * is false, so if the computed value is not positive, return
		 * right away.
		 */
		if (constraint_ns <= 0)
			return false;

		td->effective_constraint_ns = constraint_ns;
		td->cached_suspend_ok = true;
	}

	/*
	 * The children have been suspended already, so we don't need to take
	 * their suspend latencies into account here.
	 */
	return td->cached_suspend_ok;
}

static bool __default_power_down_ok(struct dev_pm_domain *pd,
				     unsigned int state)
{
	struct generic_pm_domain *genpd = pd_to_genpd(pd);
	struct gpd_link *link;
	struct pm_domain_data *pdd;
	s64 min_off_time_ns;
	s64 off_on_time_ns;

	off_on_time_ns = genpd->states[state].power_off_latency_ns +
		genpd->states[state].power_on_latency_ns;

	min_off_time_ns = -1;
	/*
	 * Check if subdomains can be off for enough time.
	 *
	 * All subdomains have been powered off already at this point.
	 */
	list_for_each_entry(link, &genpd->parent_links, parent_node) {
		struct generic_pm_domain *sd = link->child;
		s64 sd_max_off_ns = sd->max_off_time_ns;

		if (sd_max_off_ns < 0)
			continue;

		/*
		 * Check if the subdomain is allowed to be off long enough for
		 * the current domain to turn off and on (that's how much time
		 * it will have to wait worst case).
		 */
		if (sd_max_off_ns <= off_on_time_ns)
			return false;

		if (min_off_time_ns > sd_max_off_ns || min_off_time_ns < 0)
			min_off_time_ns = sd_max_off_ns;
	}

	/*
	 * Check if the devices in the domain can be off enough time.
	 */
	list_for_each_entry(pdd, &genpd->dev_list, list_node) {
		struct gpd_timing_data *td;
		s64 constraint_ns;

		/*
		 * Check if the device is allowed to be off long enough for the
		 * domain to turn off and on (that's how much time it will
		 * have to wait worst case).
		 */
		td = &to_gpd_data(pdd)->td;
		constraint_ns = td->effective_constraint_ns;
		/*
		 * Zero means "no suspend at all" and this runs only when all
		 * devices in the domain are suspended, so it must be positive.
		 */
		if (constraint_ns == PM_QOS_RESUME_LATENCY_NO_CONSTRAINT_NS)
			continue;

		if (constraint_ns <= off_on_time_ns)
			return false;

		if (min_off_time_ns > constraint_ns || min_off_time_ns < 0)
			min_off_time_ns = constraint_ns;
	}

	/*
	 * If the computed minimum device off time is negative, there are no
	 * latency constraints, so the domain can spend arbitrary time in the
	 * "off" state.
	 */
	if (min_off_time_ns < 0)
		return true;

	/*
	 * The difference between the computed minimum subdomain or device off
	 * time and the time needed to turn the domain on is the maximum
	 * theoretical time this domain can spend in the "off" state.
	 */
	genpd->max_off_time_ns = min_off_time_ns -
		genpd->states[state].power_on_latency_ns;
	return true;
}

/**
 * default_power_down_ok - Default generic PM domain power off governor routine.
 * @pd: PM domain to check.
 *
 * This routine must be executed under the PM domain's lock.
 */
static bool default_power_down_ok(struct dev_pm_domain *pd)
{
	struct generic_pm_domain *genpd = pd_to_genpd(pd);
	struct gpd_link *link;

	if (!genpd->max_off_time_changed) {
		genpd->state_idx = genpd->cached_power_down_state_idx;
		return genpd->cached_power_down_ok;
	}

	/*
	 * We have to invalidate the cached results for the parents, so
	 * use the observation that default_power_down_ok() is not
	 * going to be called for any parent until this instance
	 * returns.
	 */
	list_for_each_entry(link, &genpd->child_links, child_node)
		link->parent->max_off_time_changed = true;

	genpd->max_off_time_ns = -1;
	genpd->max_off_time_changed = false;
	genpd->cached_power_down_ok = true;
	genpd->state_idx = genpd->state_count - 1;

	/* Find a state to power down to, starting from the deepest. */
	while (!__default_power_down_ok(pd, genpd->state_idx)) {
		if (genpd->state_idx == 0) {
			genpd->cached_power_down_ok = false;
			break;
		}
		genpd->state_idx--;
	}

	genpd->cached_power_down_state_idx = genpd->state_idx;
	return genpd->cached_power_down_ok;
}

static bool always_on_power_down_ok(struct dev_pm_domain *domain)
{
	return false;
}

struct dev_power_governor simple_qos_governor = {
	.suspend_ok = default_suspend_ok,
	.power_down_ok = default_power_down_ok,
};

/**
 * pm_genpd_gov_always_on - A governor implementing an always-on policy
 */
struct dev_power_governor pm_domain_always_on_gov = {
	.power_down_ok = always_on_power_down_ok,
	.suspend_ok = default_suspend_ok,
};
