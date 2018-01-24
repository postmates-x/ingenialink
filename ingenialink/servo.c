/*
 * MIT License
 *
 * Copyright (c) 2017-2018 Ingenia-CAT S.L.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "servo.h"

#include "ingenialink/err.h"

/*******************************************************************************
 * Private
 ******************************************************************************/

/**
 * Obtain register (pre-defined or from dictionary).
 *
 * @param [in] dict
 *	Dictionary.
 * @param [in] reg_pdef
 *	Pre-defined register.
 * @param [in] id
 *	Register ID.
 * @param [out] reg
 *	Where register will be stored.
 *
 * @return
 *	0 on success, error code otherwise.
 */
static int get_reg(il_dict_t *dict, const il_reg_t *reg_pdef,
		   const char *id, const il_reg_t **reg)
{
	int r;

	/* obtain register (predefined or from dictionary) */
	if (reg_pdef) {
		*reg = reg_pdef;
	} else {
		if (!dict) {
			ilerr__set("No dictionary loaded");
			return IL_EFAIL;
		}

		r = il_dict_reg_get(dict, id, reg);
		if (r < 0)
			return r;
	}

	return 0;
}

/**
 * Raw read.
 *
 * @param [in] servo
 *	Servo.
 * @param [in] reg_pdef
 *	Pre-defined register.
 * @param [in] id
 *	Register ID.
 * @param [in] dtype
 *	Expected data type.
 * @param [out] buf
 *	Where data will be stored.
 * @param [in] sz
 *	Buffer size.
 *
 * @return
 *	0 on success, error code otherwise.
 */
static int raw_read(il_servo_t *servo, const il_reg_t *reg_pdef,
		    const char *id, il_reg_dtype_t dtype, void *buf, size_t sz)
{
	int r;
	const il_reg_t *reg;

	/* obtain register (predefined or from dictionary) */
	r = get_reg(servo->dict, reg_pdef, id, &reg);
	if (r < 0)
		return r;

	/* verify register properties */
	if (reg->dtype != dtype) {
		ilerr__set("Unexpected register data type");
		return IL_EINVAL;
	}

	if (reg->access == IL_REG_ACCESS_WO) {
		ilerr__set("Register is write-only");
		return IL_EACCESS;
	}

	return il_net__read(servo->net, servo->id, reg->address, buf, sz);
}

/**
 * Raw write.
 *
 * @param [in] servo
 *	Servo.
 * @param [in] reg_pdef
 *	Pre-defined register.
 * @param [in] id
 *	Register ID.
 * @param [in] dtype
 *	Expected data type.
 * @param [in] data
 *	Data buffer.
 * @param [in] sz
 *	Data buffer size.
 * @param [in] confirmed
 *	Confirm write.
 *
 * @return
 *	0 on success, error code otherwise.
 */
static int raw_write(il_servo_t *servo, const il_reg_t *reg_pdef,
		     const char *id, il_reg_dtype_t dtype, const void *data,
		     size_t sz, int confirmed)
{
	int r, confirmed_;
	const il_reg_t *reg;

	/* obtain register (predefined or from dictionary) */
	r = get_reg(servo->dict, reg_pdef, id, &reg);
	if (r < 0)
		return r;

	/* verify register properties */
	if (reg->dtype != dtype) {
		ilerr__set("Unexpected register data type");
		return IL_EINVAL;
	}

	if (reg->access == IL_REG_ACCESS_RO) {
		ilerr__set("Register is read-only");
		return IL_EACCESS;
	}

	/* skip confirmation on write-only registers */
	confirmed_ = (reg->access == IL_REG_ACCESS_WO) ? 0 : confirmed;

	return il_net__write(servo->net, servo->id, reg->address, data, sz,
			     confirmed_);
}

/**
 * Wait until the statusword changes its value.
 *
 * @param [in] servo
 *	IngeniaLink servo.
 * @param [in, out] sw
 *	Current statusword value, where next value will be stored.
 * @param [in, out] timeout
 *	Timeout (ms), if positive will be updated with remaining ms.
 *
 * @return
 *	0 on success, error code otherwise.
 */
static int sw_wait_change(il_servo_t *servo, uint16_t *sw, int *timeout)
{
	int r = 0;
	osal_timespec_t start = { 0, 0 }, end, diff;

	/* obtain start time */
	if (*timeout > 0) {
		if (osal_clock_gettime(&start) < 0) {
			ilerr__set("Could not obtain system time");
			return IL_EFAIL;
		}
	}

	/* wait for change */
	osal_mutex_lock(servo->sw.lock);

	if (servo->sw.value == *sw) {
		r = osal_cond_wait(servo->sw.changed, servo->sw.lock, *timeout);
		if (r == OSAL_ETIMEDOUT) {
			ilerr__set("Operation timed out");
			r = IL_ETIMEDOUT;
			goto out;
		} else if (r < 0) {
			ilerr__set("Statusword wait change failed");
			r = IL_EFAIL;
			goto out;
		}
	}

	*sw = servo->sw.value;

out:
	/* update timeout */
	if ((*timeout > 0) && (r == 0)) {
		/* obtain end time */
		if (osal_clock_gettime(&end) < 0) {
			ilerr__set("Could not obtain system time");
			r = IL_EFAIL;
			goto unlock;
		}

		/* compute difference */
		if ((end.ns - start.ns) < 0) {
			diff.s = end.s - start.s - 1;
			diff.ns = end.ns - start.ns + OSAL_CLOCK_NANOSPERSEC;
		} else {
			diff.s = end.s - start.s;
			diff.ns = end.ns - start.ns;
		}

		/* update timeout */
		*timeout -= diff.s * 1000 + diff.ns / OSAL_CLOCK_NANOSPERMSEC;
		if (*timeout <= 0) {
			ilerr__set("Operation timed out");
			r = IL_ETIMEDOUT;
		}
	}

unlock:
	osal_mutex_unlock(servo->sw.lock);

	return r;
}

/**
 * Statusword update callback
 *
 * @param [in] ctx
 *	Context (servo_t *).
 * @param [in] sw
 *	Statusword value.
 */
static void sw_update(void *ctx, uint16_t sw)
{
	il_servo_t *servo = ctx;

	osal_mutex_lock(servo->sw.lock);

	if (servo->sw.value != sw) {
		servo->sw.value = sw;
		osal_cond_broadcast(servo->sw.changed);
	}

	osal_mutex_unlock(servo->sw.lock);
}

/**
 * State change monitor, used to push state changes to external subscriptors.
 *
 * @param [in] args
 *	Arguments (il_servo_t *).
 */
static int state_subs_monitor(void *args)
{
	il_servo_t *servo = args;
	uint16_t sw;

	osal_mutex_lock(servo->sw.lock);
	sw = servo->sw.value;
	osal_mutex_unlock(servo->sw.lock);

	while (!servo->state_subs.stop) {
		int timeout;
		size_t i;
		il_servo_state_t state;
		int flags;

		/* wait for change */
		timeout = STATE_SUBS_TIMEOUT;
		if (sw_wait_change(servo, &sw, &timeout) < 0)
			continue;

		/* obtain state/flags */
		servo->ops->_state_decode(sw, &state, &flags);

		/* notify all subscribers */
		osal_mutex_lock(servo->state_subs.lock);

		for (i = 0; i < servo->state_subs.sz; i++) {
			void *ctx;

			if (!servo->state_subs.subs[i].cb)
				continue;

			ctx = servo->state_subs.subs[i].ctx;
			servo->state_subs.subs[i].cb(ctx, state, flags);
		}

		osal_mutex_unlock(servo->state_subs.lock);
	}

	return 0;
}

/**
 * Emergencies callback.
 *
 */
static void on_emcy(void *ctx, uint32_t code)
{
	il_servo_t *servo = ctx;
	il_servo_emcy_t *emcy = &servo->emcy;

	osal_mutex_lock(emcy->lock);

	/* if full, drop oldest item */
	if (!CIRC_SPACE(emcy->head, emcy->tail, EMCY_QUEUE_SZ))
		emcy->tail = (emcy->tail + 1) & (EMCY_QUEUE_SZ - 1);

	/* push emergency and notify */
	emcy->queue[emcy->head] = code;
	emcy->head = (emcy->head + 1) & (EMCY_QUEUE_SZ - 1);

	osal_cond_signal(emcy->not_empty);

	osal_mutex_unlock(emcy->lock);
}

/**
 * Emergencies monitor, used to notify about emergencies to external
 * subscriptors.
 *
 * @param [in] args
 *	Arguments (il_servo_t *).
 */
static int emcy_subs_monitor(void *args)
{
	il_servo_t *servo = args;
	il_servo_emcy_t *emcy = &servo->emcy;
	il_servo_emcy_subscriber_lst_t *emcy_subs = &servo->emcy_subs;

	while (!emcy_subs->stop) {
		int r;

		/* wait until emcy queue not empty */
		osal_mutex_lock(emcy->lock);

		r = osal_cond_wait(emcy->not_empty, emcy->lock,
				   EMCY_SUBS_TIMEOUT);

		if (r == 0) {
			/* process all available emergencies */
			while (CIRC_CNT(emcy->head, emcy->tail, emcy->sz)) {
				size_t i;
				uint32_t code;

				code = emcy->queue[emcy->tail];
				emcy->tail = (emcy->tail + 1) & (emcy->sz - 1);

				osal_mutex_unlock(emcy->lock);

				/* notify all subscribers */
				osal_mutex_lock(emcy_subs->lock);

				for (i = 0; i < emcy_subs->sz; i++) {
					void *ctx;

					if (!emcy_subs->subs[i].cb)
						continue;

					ctx = emcy_subs->subs[i].ctx;
					emcy_subs->subs[i].cb(ctx, code);
				}

				osal_mutex_unlock(emcy_subs->lock);
				osal_mutex_lock(emcy->lock);
			}
		}

		osal_mutex_unlock(emcy->lock);
	}

	return 0;
}

/*******************************************************************************
 * Base implementation
 ******************************************************************************/

int il_servo_base__init(il_servo_t *servo, il_net_t *net, uint16_t id,
			const char *dict)
{
	int r;

	/* initialize */
	servo->net = net;
	servo->id = id;

	il_net__retain(servo->net);

	/* load dictionary (optional) */
	if (dict) {
		servo->dict = il_dict_create(dict);
		if (!servo->dict) {
			r = IL_EFAIL;
			goto cleanup_net;
		}
	} else {
		servo->dict = NULL;
	}

	/* configure units */
	servo->units.lock = osal_mutex_create();
	if (!servo->units.lock) {
		ilerr__set("Units lock allocation failed");
		r = IL_EFAIL;
		goto cleanup_dict;
	}

	servo->units.torque = IL_UNITS_TORQUE_NATIVE;
	servo->units.pos = IL_UNITS_POS_NATIVE;
	servo->units.vel = IL_UNITS_VEL_NATIVE;
	servo->units.acc = IL_UNITS_ACC_NATIVE;

	/* configure statusword subscription */
	servo->sw.lock = osal_mutex_create();
	if (!servo->sw.lock) {
		ilerr__set("Statusword subscriber lock allocation failed");
		r = IL_EFAIL;
		goto cleanup_units_lock;
	}

	servo->sw.changed = osal_cond_create();
	if (!servo->sw.changed) {
		ilerr__set("Statusword subscriber condition allocation failed");
		r = IL_EFAIL;
		goto cleanup_sw_lock;
	}

	servo->sw.value = 0;

	r = il_net__sw_subscribe(servo->net, servo->id, sw_update, servo);
	if (r < 0)
		goto cleanup_sw_changed;

	servo->sw.slot = r;

	/* configute external state subscriptors */
	servo->state_subs.subs = calloc(STATE_SUBS_SZ_DEF,
					sizeof(*servo->state_subs.subs));
	if (!servo->state_subs.subs) {
		ilerr__set("State subscribers allocation failed");
		r = IL_EFAIL;
		goto cleanup_sw_subscribe;
	}

	servo->state_subs.sz = STATE_SUBS_SZ_DEF;

	servo->state_subs.lock = osal_mutex_create();
	if (!servo->state_subs.lock) {
		ilerr__set("State subscription lock allocation failed");
		r = IL_EFAIL;
		goto cleanup_state_subs_subs;
	}

	servo->state_subs.stop = 0;

	servo->state_subs.monitor = osal_thread_create(state_subs_monitor,
						       servo);
	if (!servo->state_subs.monitor) {
		ilerr__set("State change monitor could not be created");
		r = IL_EFAIL;
		goto cleanup_state_subs_lock;
	}

	/* configure emergency subscription */
	servo->emcy.lock = osal_mutex_create();
	if (!servo->emcy.lock) {
		ilerr__set("Emergency subscriber lock allocation failed");
		r = IL_EFAIL;
		goto cleanup_state_subs_monitor;
	}

	servo->emcy.not_empty = osal_cond_create();
	if (!servo->emcy.not_empty) {
		ilerr__set("Emergency subscriber condition allocation failed");
		r = IL_EFAIL;
		goto cleanup_emcy_lock;
	}

	servo->emcy.head = 0;
	servo->emcy.tail = 0;
	servo->emcy.sz = EMCY_QUEUE_SZ;

	r = il_net__emcy_subscribe(servo->net, servo->id, on_emcy, servo);
	if (r < 0)
		goto cleanup_emcy_not_empty;

	servo->emcy.slot = r;

	/* configure external emergency subscriptors */
	servo->emcy_subs.subs = calloc(EMCY_SUBS_SZ_DEF,
				       sizeof(*servo->emcy_subs.subs));
	if (!servo->emcy_subs.subs) {
		ilerr__set("Emergency subscribers allocation failed");
		r = IL_EFAIL;
		goto cleanup_emcy_subscribe;
	}

	servo->emcy_subs.sz = EMCY_SUBS_SZ_DEF;

	servo->emcy_subs.lock = osal_mutex_create();
	if (!servo->emcy_subs.lock) {
		ilerr__set("Emergency subscription lock allocation failed");
		r = IL_EFAIL;
		goto cleanup_emcy_subs_subs;
	}

	servo->emcy_subs.stop = 0;
	servo->emcy_subs.monitor = osal_thread_create(emcy_subs_monitor, servo);
	if (!servo->emcy_subs.monitor) {
		ilerr__set("Emergency monitor could not be created");
		r = IL_EFAIL;
		goto cleanup_emcy_subs_lock;
	}

	return 0;

cleanup_emcy_subs_lock:
	osal_mutex_destroy(servo->emcy_subs.lock);

cleanup_emcy_subs_subs:
	free(servo->emcy_subs.subs);

cleanup_emcy_subscribe:
	il_net__emcy_unsubscribe(servo->net, servo->emcy.slot);

cleanup_emcy_not_empty:
	osal_cond_destroy(servo->emcy.not_empty);

cleanup_emcy_lock:
	osal_mutex_destroy(servo->emcy.lock);

cleanup_state_subs_monitor:
	servo->state_subs.stop = 1;
	(void)osal_thread_join(servo->state_subs.monitor, NULL);

cleanup_state_subs_lock:
	osal_mutex_destroy(servo->state_subs.lock);

cleanup_state_subs_subs:
	free(servo->state_subs.subs);

cleanup_sw_subscribe:
	il_net__sw_unsubscribe(servo->net, servo->sw.slot);

cleanup_sw_changed:
	osal_cond_destroy(servo->sw.changed);

cleanup_sw_lock:
	osal_mutex_destroy(servo->sw.lock);

cleanup_units_lock:
	osal_mutex_destroy(servo->units.lock);

cleanup_dict:
	if (servo->dict)
		il_dict_destroy(servo->dict);

cleanup_net:
	il_net__release(servo->net);

	return r;
}

void il_servo_base__deinit(il_servo_t *servo)
{
	servo->emcy_subs.stop = 1;
	(void)osal_thread_join(servo->emcy_subs.monitor, NULL);
	osal_mutex_destroy(servo->emcy_subs.lock);
	free(servo->emcy_subs.subs);

	il_net__emcy_unsubscribe(servo->net, servo->emcy.slot);
	osal_cond_destroy(servo->emcy.not_empty);
	osal_mutex_destroy(servo->emcy.lock);

	servo->state_subs.stop = 1;
	(void)osal_thread_join(servo->state_subs.monitor, NULL);
	osal_mutex_destroy(servo->state_subs.lock);
	free(servo->state_subs.subs);

	il_net__sw_unsubscribe(servo->net, servo->sw.slot);
	osal_cond_destroy(servo->sw.changed);
	osal_mutex_destroy(servo->sw.lock);

	osal_mutex_destroy(servo->units.lock);

	if (servo->dict)
		il_dict_destroy(servo->dict);
}

void il_servo_base__state_get(il_servo_t *servo, il_servo_state_t *state,
			      int *flags)
{
	uint16_t sw;

	osal_mutex_lock(servo->sw.lock);
	sw = servo->sw.value;
	osal_mutex_unlock(servo->sw.lock);

	servo->ops->_state_decode(sw, state, flags);
}

int il_servo_base__state_subscribe(il_servo_t *servo,
				   il_servo_state_subscriber_cb_t cb, void *ctx)
{
	int r = 0;
	int slot;

	osal_mutex_lock(servo->state_subs.lock);

	/* look for the first empty slot */
	for (slot = 0; slot < (int)servo->state_subs.sz; slot++) {
		if (!servo->state_subs.subs[slot].cb)
			break;
	}

	/* increase array if no space left */
	if (slot == (int)servo->state_subs.sz) {
		size_t sz;
		il_servo_state_subscriber_t *subs;

		/* double in size on each realloc */
		sz = 2 * servo->state_subs.sz * sizeof(*subs);
		subs = realloc(servo->state_subs.subs, sz);
		if (!subs) {
			ilerr__set("Subscribers re-allocation failed");
			r = IL_ENOMEM;
			goto unlock;
		}

		servo->state_subs.subs = subs;
		servo->state_subs.sz = sz;
	}

	servo->state_subs.subs[slot].cb = cb;
	servo->state_subs.subs[slot].ctx = ctx;

	r = slot;

unlock:
	osal_mutex_unlock(servo->state_subs.lock);

	return r;
}

void il_servo_base__state_unsubscribe(il_servo_t *servo, int slot)
{
	osal_mutex_lock(servo->state_subs.lock);

	/* skip out of range slot */
	if (slot >= (int)servo->state_subs.sz)
		return;

	servo->state_subs.subs[slot].cb = NULL;
	servo->state_subs.subs[slot].ctx = NULL;

	osal_mutex_unlock(servo->state_subs.lock);
}

int il_servo_base__emcy_subscribe(il_servo_t *servo,
				  il_servo_emcy_subscriber_cb_t cb, void *ctx)
{
	int r = 0;
	int slot;

	osal_mutex_lock(servo->emcy_subs.lock);

	/* look for the first empty slot */
	for (slot = 0; slot < (int)servo->emcy_subs.sz; slot++) {
		if (!servo->emcy_subs.subs[slot].cb)
			break;
	}

	/* increase array if no space left */
	if (slot == (int)servo->emcy_subs.sz) {
		size_t sz;
		il_servo_emcy_subscriber_t *subs;

		/* double in size on each realloc */
		sz = 2 * servo->emcy_subs.sz * sizeof(*subs);
		subs = realloc(servo->emcy_subs.subs, sz);
		if (!subs) {
			ilerr__set("Subscribers re-allocation failed");
			r = IL_ENOMEM;
			goto unlock;
		}

		servo->emcy_subs.subs = subs;
		servo->emcy_subs.sz = sz;
	}

	servo->emcy_subs.subs[slot].cb = cb;
	servo->emcy_subs.subs[slot].ctx = ctx;

	r = slot;

unlock:
	osal_mutex_unlock(servo->emcy_subs.lock);

	return r;
}

void il_servo_base__emcy_unsubscribe(il_servo_t *servo, int slot)
{
	osal_mutex_lock(servo->emcy_subs.lock);

	/* skip out of range slot */
	if (slot >= (int)servo->emcy_subs.sz)
		return;

	servo->emcy_subs.subs[slot].cb = NULL;
	servo->emcy_subs.subs[slot].ctx = NULL;

	osal_mutex_unlock(servo->emcy_subs.lock);
}

il_dict_t *il_servo_base__dict_get(il_servo_t *servo)
{
	return servo->dict;
}

int il_servo_base__dict_load(il_servo_t *servo, const char *dict)
{
	if (servo->dict) {
		ilerr__set("Dictionary already loaded");
		return IL_EALREADY;
	}

	servo->dict = il_dict_create(dict);
	if (!servo->dict)
		return IL_EFAIL;

	return 0;
}

il_units_torque_t il_servo_base__units_torque_get(il_servo_t *servo)
{
	il_units_torque_t units;

	osal_mutex_lock(servo->units.lock);
	units = servo->units.torque;
	osal_mutex_unlock(servo->units.lock);

	return units;
}

void il_servo_base__units_torque_set(il_servo_t *servo, il_units_torque_t units)
{
	osal_mutex_lock(servo->units.lock);
	servo->units.torque = units;
	osal_mutex_unlock(servo->units.lock);
}

il_units_pos_t il_servo_base__units_pos_get(il_servo_t *servo)
{
	il_units_pos_t units;

	osal_mutex_lock(servo->units.lock);
	units = servo->units.pos;
	osal_mutex_unlock(servo->units.lock);

	return units;
}

void il_servo_base__units_pos_set(il_servo_t *servo, il_units_pos_t units)
{
	osal_mutex_lock(servo->units.lock);
	servo->units.pos = units;
	osal_mutex_unlock(servo->units.lock);
}

il_units_vel_t il_servo_base__units_vel_get(il_servo_t *servo)
{
	il_units_vel_t units;

	osal_mutex_lock(servo->units.lock);
	units = servo->units.vel;
	osal_mutex_unlock(servo->units.lock);

	return units;
}

void il_servo_base__units_vel_set(il_servo_t *servo, il_units_vel_t units)
{
	osal_mutex_lock(servo->units.lock);
	servo->units.vel = units;
	osal_mutex_unlock(servo->units.lock);
}

il_units_acc_t il_servo_base__units_acc_get(il_servo_t *servo)
{
	il_units_acc_t units;

	osal_mutex_lock(servo->units.lock);
	units = servo->units.acc;
	osal_mutex_unlock(servo->units.lock);

	return units;
}

void il_servo_base__units_acc_set(il_servo_t *servo, il_units_acc_t units)
{
	osal_mutex_lock(servo->units.lock);
	servo->units.acc = units;
	osal_mutex_unlock(servo->units.lock);
}

int il_servo_base__raw_read_u8(il_servo_t *servo, const il_reg_t *reg,
			       const char *id, uint8_t *buf)
{
	return raw_read(servo, reg, id, IL_REG_DTYPE_U8, buf, sizeof(*buf));
}

int il_servo_base__raw_read_s8(il_servo_t *servo, const il_reg_t *reg,
			       const char *id, int8_t *buf)
{
	return raw_read(servo, reg, id, IL_REG_DTYPE_S8, buf, sizeof(*buf));
}

int il_servo_base__raw_read_u16(il_servo_t *servo, const il_reg_t *reg,
				const char *id, uint16_t *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_U16, buf, sizeof(*buf));
	if (r == 0)
		*buf = __swap_16(*buf);

	return r;
}

int il_servo_base__raw_read_s16(il_servo_t *servo, const il_reg_t *reg,
				const char *id, int16_t *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_S16, buf, sizeof(*buf));
	if (r == 0)
		*buf = (int16_t)__swap_16(*buf);

	return r;
}

int il_servo_base__raw_read_u32(il_servo_t *servo, const il_reg_t *reg,
				const char *id, uint32_t *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_U32, buf, sizeof(*buf));
	if (r == 0)
		*buf = __swap_32(*buf);

	return r;
}

int il_servo_base__raw_read_s32(il_servo_t *servo, const il_reg_t *reg,
				const char *id, int32_t *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_S32, buf, sizeof(*buf));
	if (r == 0)
		*buf = (int32_t)__swap_32(*buf);

	return r;
}

int il_servo_base__raw_read_u64(il_servo_t *servo, const il_reg_t *reg,
				const char *id, uint64_t *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_U64, buf, sizeof(*buf));
	if (r == 0)
		*buf = __swap_64(*buf);

	return r;
}

int il_servo_base__raw_read_s64(il_servo_t *servo, const il_reg_t *reg,
				const char *id, int64_t *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_S64, buf, sizeof(*buf));
	if (r == 0)
		*buf = (int64_t)__swap_64(*buf);

	return r;
}

int il_servo_base__raw_read_float(il_servo_t *servo, const il_reg_t *reg,
				  const char *id, float *buf)
{
	int r;

	r = raw_read(servo, reg, id, IL_REG_DTYPE_FLOAT, buf, sizeof(*buf));
	if (r == 0)
		*buf = __swap_float(*buf);

	return r;
}

int il_servo_base__read(il_servo_t *servo, const il_reg_t *reg, const char *id,
			double *buf)
{
	int r;

	const il_reg_t *reg_;

	uint8_t u8_v;
	uint16_t u16_v;
	uint32_t u32_v;
	uint64_t u64_v;
	int8_t s8_v;
	int16_t s16_v;
	int32_t s32_v;
	int64_t s64_v;
	float float_v;

	double buf_;

	/* obtain register (predefined or from dictionary) */
	r = get_reg(servo->dict, reg, id, &reg_);
	if (r < 0)
		return r;

	/* read */
	switch (reg_->dtype) {
	case IL_REG_DTYPE_U8:
		r = il_servo_raw_read_u8(servo, reg_, NULL, &u8_v);
		buf_ = (float)u8_v;
		break;
	case IL_REG_DTYPE_S8:
		r = il_servo_raw_read_s8(servo, reg_, NULL, &s8_v);
		buf_ = (float)s8_v;
		break;
	case IL_REG_DTYPE_U16:
		r = il_servo_raw_read_u16(servo, reg_, NULL, &u16_v);
		buf_ = (float)u16_v;
		break;
	case IL_REG_DTYPE_S16:
		r = il_servo_raw_read_s16(servo, reg_, NULL, &s16_v);
		buf_ = (float)s16_v;
		break;
	case IL_REG_DTYPE_U32:
		r = il_servo_raw_read_u32(servo, reg_, NULL, &u32_v);
		buf_ = (float)u32_v;
		break;
	case IL_REG_DTYPE_S32:
		r = il_servo_raw_read_s32(servo, reg_, NULL, &s32_v);
		buf_ = (float)s32_v;
		break;
	case IL_REG_DTYPE_U64:
		r = il_servo_raw_read_u64(servo, reg_, NULL, &u64_v);
		buf_ = (float)u64_v;
		break;
	case IL_REG_DTYPE_S64:
		r = il_servo_raw_read_s64(servo, reg_, NULL, &s64_v);
		buf_ = (float)s64_v;
		break;
	case IL_REG_DTYPE_FLOAT:
		r = il_servo_raw_read_float(servo, reg_, NULL, &float_v);
		buf_ = (double)float_v;
		break;
	default:
		ilerr__set("Unsupported register data type");
		return IL_EINVAL;
	}

	if (r < 0)
		return r;

	/* store converted value to buffer */
	*buf = buf_ * il_servo_units_factor(servo, reg_);

	return 0;
}

int il_servo_base__raw_write_u8(il_servo_t *servo, const il_reg_t *reg,
				const char *id, uint8_t val, int confirm)
{
	return raw_write(servo, reg, id, IL_REG_DTYPE_U8, &val, sizeof(val),
			 confirm);
}

int il_servo_base__raw_write_s8(il_servo_t *servo, const il_reg_t *reg,
				const char *id, int8_t val, int confirm)
{
	return raw_write(servo, reg, id, IL_REG_DTYPE_S8, &val, sizeof(val),
			 confirm);
}

int il_servo_base__raw_write_u16(il_servo_t *servo, const il_reg_t *reg,
				 const char *id, uint16_t val, int confirm)
{
	uint16_t val_;

	val_ = __swap_16(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_U16, &val_, sizeof(val_),
			 confirm);
}

int il_servo_base__raw_write_s16(il_servo_t *servo, const il_reg_t *reg,
				 const char *id, int16_t val, int confirm)
{
	int16_t val_;

	val_ = (int16_t)__swap_16(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_S16, &val_, sizeof(val_),
			 confirm);
}

int il_servo_base__raw_write_u32(il_servo_t *servo, const il_reg_t *reg,
				 const char *id, uint32_t val, int confirm)
{
	uint32_t val_;

	val_ = __swap_32(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_U32, &val_, sizeof(val_),
			 confirm);
}

int il_servo_base__raw_write_s32(il_servo_t *servo, const il_reg_t *reg,
				 const char *id, int32_t val, int confirm)
{
	int32_t val_;

	val_ = (int32_t)__swap_32(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_S32, &val_, sizeof(val_),
			 confirm);
}

int il_servo_base__raw_write_u64(il_servo_t *servo, const il_reg_t *reg,
				 const char *id, uint64_t val, int confirm)
{
	uint64_t val_;

	val_ = __swap_64(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_U64, &val_, sizeof(val_),
			 confirm);
}

int il_servo_base__raw_write_s64(il_servo_t *servo, const il_reg_t *reg,
				 const char *id, int64_t val, int confirm)
{
	int64_t val_;

	val_ = (int64_t)__swap_64(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_S64, &val_, sizeof(val_),
			 confirm);
}

int il_servo_base__raw_write_float(il_servo_t *servo, const il_reg_t *reg,
				   const char *id, float val, int confirm)
{
	float val_;

	val_ = __swap_float(val);

	return raw_write(servo, reg, id, IL_REG_DTYPE_FLOAT, &val_,
			 sizeof(val_), confirm);
}

int il_servo_base__write(il_servo_t *servo, const il_reg_t *reg, const char *id,
			 double val, int confirm)
{
	int r;

	const il_reg_t *reg_;
	double val_;

	/* obtain register (predefined or from dictionary) */
	r = get_reg(servo->dict, reg, id, &reg_);
	if (r < 0)
		return r;

	/* convert to native units */
	val_ = val / il_servo_units_factor(servo, reg_);

	/* write using the appropriate native type */
	switch (reg_->dtype) {
	case IL_REG_DTYPE_U8:
		return il_servo_raw_write_u8(servo, reg_, NULL, (uint8_t)val_,
					     confirm);
	case IL_REG_DTYPE_S8:
		return il_servo_raw_write_s8(servo, reg_, NULL, (int8_t)val_,
					     confirm);
	case IL_REG_DTYPE_U16:
		return il_servo_raw_write_u16(servo, reg_, NULL, (uint16_t)val_,
					      confirm);
	case IL_REG_DTYPE_S16:
		return il_servo_raw_write_s16(servo, reg_, NULL, (int16_t)val_,
					      confirm);
	case IL_REG_DTYPE_U32:
		return il_servo_raw_write_u32(servo, reg_, NULL, (uint32_t)val_,
					      confirm);
	case IL_REG_DTYPE_S32:
		return il_servo_raw_write_s32(servo, reg_, NULL, (int32_t)val_,
					      confirm);
	case IL_REG_DTYPE_U64:
		return il_servo_raw_write_u64(servo, reg_, NULL, (uint64_t)val_,
					      confirm);
	case IL_REG_DTYPE_S64:
		return il_servo_raw_write_s64(servo, reg_, NULL, (int64_t)val_,
					      confirm);
	case IL_REG_DTYPE_FLOAT:
		return il_servo_raw_write_float(servo, reg_, NULL, (float)val_,
						confirm);
	default:
		ilerr__set("Unsupported register data type");
		return IL_EINVAL;
	}
}

/*******************************************************************************
 * Internal
 ******************************************************************************/

void il_servo__retain(il_servo_t *servo)
{
	servo->ops->_retain(servo);
}

void il_servo__release(il_servo_t *servo)
{
	servo->ops->_release(servo);
}

/*******************************************************************************
 * Public
 ******************************************************************************/

il_servo_t *il_servo_create(il_net_t *net, uint16_t id, const char *dict)
{
	switch (il_net_prot_get(net)) {
#ifdef IL_HAS_PROT_EUSB
	case IL_NET_PROT_EUSB:
		return il_eusb_servo_ops.create(net, id, dict);
#endif
#ifdef IL_HAS_PROT_MCB
	case IL_NET_PROT_MCB:
		return il_mcb_servo_ops.create(net, id, dict);
#endif
	default:
		ilerr__set("Unsupported network protocol");
		return NULL;
	}
}

void il_servo_destroy(il_servo_t *servo)
{
	servo->ops->destroy(servo);
}

void il_servo_state_get(il_servo_t *servo, il_servo_state_t *state, int *flags)
{
	servo->ops->state_get(servo, state, flags);
}

int il_servo_state_subscribe(il_servo_t *servo,
			     il_servo_state_subscriber_cb_t cb, void *ctx)
{
	return servo->ops->state_subscribe(servo, cb, ctx);
}

void il_servo_state_unsubscribe(il_servo_t *servo, int slot)
{
	servo->ops->state_unsubscribe(servo, slot);
}

int il_servo_emcy_subscribe(il_servo_t *servo, il_servo_emcy_subscriber_cb_t cb,
			    void *ctx)
{
	return servo->ops->emcy_subscribe(servo, cb, ctx);
}

void il_servo_emcy_unsubscribe(il_servo_t *servo, int slot)
{
	servo->ops->emcy_unsubscribe(servo, slot);
}

il_dict_t *il_servo_dict_get(il_servo_t *servo)
{
	return servo->dict;
}

int il_servo_dict_load(il_servo_t *servo, const char *dict)
{
	if (servo->dict) {
		ilerr__set("Dictionary already loaded");
		return IL_EALREADY;
	}

	servo->dict = il_dict_create(dict);
	if (!servo->dict)
		return IL_EFAIL;

	return 0;
}

int il_servo_name_get(il_servo_t *servo, char *name, size_t sz)
{
	return servo->ops->name_get(servo, name, sz);
}

int il_servo_name_set(il_servo_t *servo, const char *name)
{
	return servo->ops->name_set(servo, name);
}

int il_servo_info_get(il_servo_t *servo, il_servo_info_t *info)
{
	return servo->ops->info_get(servo, info);
}

int il_servo_store_all(il_servo_t *servo)
{
	return servo->ops->store_all(servo);
}

int il_servo_store_comm(il_servo_t *servo)
{
	return servo->ops->store_comm(servo);
}

int il_servo_store_app(il_servo_t *servo)
{
	return servo->ops->store_app(servo);
}

int il_servo_units_update(il_servo_t *servo)
{
	return servo->ops->units_update(servo);
}

double il_servo_units_factor(il_servo_t *servo, const il_reg_t *reg)
{
	return servo->ops->units_factor(servo, reg);
}

il_units_torque_t il_servo_units_torque_get(il_servo_t *servo)
{
	return servo->ops->units_torque_get(servo);
}

void il_servo_units_torque_set(il_servo_t *servo, il_units_torque_t units)
{
	servo->ops->units_torque_set(servo, units);
}

il_units_pos_t il_servo_units_pos_get(il_servo_t *servo)
{
	return servo->ops->units_pos_get(servo);
}

void il_servo_units_pos_set(il_servo_t *servo, il_units_pos_t units)
{
	servo->ops->units_pos_set(servo, units);
}

il_units_vel_t il_servo_units_vel_get(il_servo_t *servo)
{
	return servo->ops->units_vel_get(servo);
}

void il_servo_units_vel_set(il_servo_t *servo, il_units_vel_t units)
{
	servo->ops->units_vel_set(servo, units);
}

il_units_acc_t il_servo_units_acc_get(il_servo_t *servo)
{
	return servo->ops->units_acc_get(servo);
}

void il_servo_units_acc_set(il_servo_t *servo, il_units_acc_t units)
{
	servo->ops->units_acc_set(servo, units);
}

int il_servo_raw_read_u8(il_servo_t *servo, const il_reg_t *reg, const char *id,
			 uint8_t *buf)
{
	return servo->ops->raw_read_u8(servo, reg, id, buf);
}

int il_servo_raw_read_s8(il_servo_t *servo, const il_reg_t *reg, const char *id,
			 int8_t *buf)
{
	return servo->ops->raw_read_s8(servo, reg, id, buf);
}

int il_servo_raw_read_u16(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, uint16_t *buf)
{
	return servo->ops->raw_read_u16(servo, reg, id, buf);
}

int il_servo_raw_read_s16(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, int16_t *buf)
{
	return servo->ops->raw_read_s16(servo, reg, id, buf);
}

int il_servo_raw_read_u32(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, uint32_t *buf)
{
	return servo->ops->raw_read_u32(servo, reg, id, buf);
}

int il_servo_raw_read_s32(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, int32_t *buf)
{
	return servo->ops->raw_read_s32(servo, reg, id, buf);
}

int il_servo_raw_read_u64(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, uint64_t *buf)
{
	return servo->ops->raw_read_u64(servo, reg, id, buf);
}

int il_servo_raw_read_s64(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, int64_t *buf)
{
	return servo->ops->raw_read_s64(servo, reg, id, buf);
}

int il_servo_raw_read_float(il_servo_t *servo, const il_reg_t *reg,
			    const char *id, float *buf)
{
	return servo->ops->raw_read_float(servo, reg, id, buf);
}

int il_servo_read(il_servo_t *servo, const il_reg_t *reg, const char *id,
		  double *buf)
{
	return servo->ops->read(servo, reg, id, buf);
}

int il_servo_raw_write_u8(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, uint8_t val, int confirm)
{
	return servo->ops->raw_write_u8(servo, reg, id, val, confirm);
}

int il_servo_raw_write_s8(il_servo_t *servo, const il_reg_t *reg,
			  const char *id, int8_t val, int confirm)
{
	return servo->ops->raw_write_s8(servo, reg, id, val, confirm);
}

int il_servo_raw_write_u16(il_servo_t *servo, const il_reg_t *reg,
			   const char *id, uint16_t val, int confirm)
{
	return servo->ops->raw_write_u16(servo, reg, id, val, confirm);
}

int il_servo_raw_write_s16(il_servo_t *servo, const il_reg_t *reg,
			   const char *id, int16_t val, int confirm)
{
	return servo->ops->raw_write_s16(servo, reg, id, val, confirm);
}

int il_servo_raw_write_u32(il_servo_t *servo, const il_reg_t *reg,
			   const char *id, uint32_t val, int confirm)
{
	return servo->ops->raw_write_u32(servo, reg, id, val, confirm);
}

int il_servo_raw_write_s32(il_servo_t *servo, const il_reg_t *reg,
			   const char *id, int32_t val, int confirm)
{
	return servo->ops->raw_write_s32(servo, reg, id, val, confirm);
}

int il_servo_raw_write_u64(il_servo_t *servo, const il_reg_t *reg,
			   const char *id, uint64_t val, int confirm)
{
	return servo->ops->raw_write_u64(servo, reg, id, val, confirm);
}

int il_servo_raw_write_s64(il_servo_t *servo, const il_reg_t *reg,
			   const char *id, int64_t val, int confirm)
{
	return servo->ops->raw_write_s64(servo, reg, id, val, confirm);
}

int il_servo_raw_write_float(il_servo_t *servo, const il_reg_t *reg,
			     const char *id, float val, int confirm)
{
	return servo->ops->raw_write_float(servo, reg, id, val, confirm);
}

int il_servo_write(il_servo_t *servo, const il_reg_t *reg, const char *id,
		   double val, int confirm)
{
	return servo->ops->write(servo, reg, id, val, confirm);
}

int il_servo_disable(il_servo_t *servo)
{
	return servo->ops->disable(servo);
}

int il_servo_switch_on(il_servo_t *servo, int timeout)
{
	return servo->ops->switch_on(servo, timeout);
}

int il_servo_enable(il_servo_t *servo, int timeout)
{
	return servo->ops->enable(servo, timeout);
}

int il_servo_fault_reset(il_servo_t *servo)
{
	return servo->ops->fault_reset(servo);
}

int il_servo_mode_get(il_servo_t *servo, il_servo_mode_t *mode)
{
	return servo->ops->mode_get(servo, mode);
}

int il_servo_mode_set(il_servo_t *servo, il_servo_mode_t mode)
{
	return servo->ops->mode_set(servo, mode);
}

int il_servo_ol_voltage_get(il_servo_t *servo, double *voltage)
{
	return servo->ops->ol_voltage_get(servo, voltage);
}

int il_servo_ol_voltage_set(il_servo_t *servo, double voltage)
{
	return servo->ops->ol_voltage_set(servo, voltage);
}

int il_servo_ol_frequency_get(il_servo_t *servo, double *freq)
{
	return servo->ops->ol_frequency_get(servo, freq);
}

int il_servo_ol_frequency_set(il_servo_t *servo, double freq)
{
	return servo->ops->ol_frequency_set(servo, freq);
}

int il_servo_homing_start(il_servo_t *servo)
{
	return servo->ops->homing_start(servo);
}

int il_servo_homing_wait(il_servo_t *servo, int timeout)
{
	return servo->ops->homing_wait(servo, timeout);
}

int il_servo_torque_get(il_servo_t *servo, double *torque)
{
	return servo->ops->torque_get(servo, torque);
}

int il_servo_torque_set(il_servo_t *servo, double torque)
{
	return servo->ops->torque_set(servo, torque);
}

int il_servo_position_get(il_servo_t *servo, double *pos)
{
	return servo->ops->position_get(servo, pos);
}

int il_servo_position_set(il_servo_t *servo, double pos, int immediate,
			  int relative, int sp_timeout)
{
	return servo->ops->position_set(servo, pos, immediate, relative,
					sp_timeout);
}

int il_servo_position_res_get(il_servo_t *servo, uint32_t *res)
{
	return servo->ops->position_res_get(servo, res);
}

int il_servo_velocity_get(il_servo_t *servo, double *vel)
{
	return servo->ops->velocity_get(servo, vel);
}

int il_servo_velocity_set(il_servo_t *servo, double vel)
{
	return servo->ops->velocity_set(servo, vel);
}

int il_servo_velocity_res_get(il_servo_t *servo, uint32_t *res)
{
	return servo->ops->velocity_res_get(servo, res);
}

int il_servo_wait_reached(il_servo_t *servo, int timeout)
{
	return servo->ops->wait_reached(servo, timeout);
}

int il_servo_lucky(il_net_prot_t prot, il_net_t **net, il_servo_t **servo,
		   const char *dict)
{
	il_net_dev_list_t *devs, *dev;
	il_net_servos_list_t *servo_ids, *servo_id;

	/* scan all available network devices */
	devs = il_net_dev_list_get(prot);
	il_net_dev_list_foreach(dev, devs) {
		il_net_opts_t opts;

		opts.port = dev->port;
		opts.timeout_rd = IL_NET_TIMEOUT_RD_DEF;
		opts.timeout_wr = IL_NET_TIMEOUT_WR_DEF;

		*net = il_net_create(prot, &opts);
		if (!*net)
			continue;

		/* try to connect to any available servo */
		servo_ids = il_net_servos_list_get(*net, NULL, NULL);
		il_net_servos_list_foreach(servo_id, servo_ids) {
			*servo = il_servo_create(*net, servo_id->id, dict);
			/* found */
			if (*servo) {
				il_net_servos_list_destroy(servo_ids);
				il_net_dev_list_destroy(devs);

				return 0;
			}
		}

		il_net_servos_list_destroy(servo_ids);
		il_net_destroy(*net);
	}

	il_net_dev_list_destroy(devs);

	ilerr__set("No connected servos found");
	return IL_EFAIL;
}
