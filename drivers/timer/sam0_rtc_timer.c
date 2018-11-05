/*
 * Copyright (c) 2018 omSquare s.r.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Atmel SAM0 series RTC-based system timer
 *
 * This system timer implementation supports both tickless and ticking modes.
 * In tickless mode, RTC counts continually in 32-bit mode and timeouts are
 * scheduled using the RTC comparator. In ticking mode, RTC is configured to
 * generate an interrupt every tick.
 */

#include <soc.h>
#include <clock_control.h>
#include <system_timer.h>
#include <sys_clock.h>

/* RTC registers. */
#define RTC0 ((RtcMode0 *) CONFIG_RTC_SAM0_BASE_ADDRESS)

/* Number of sys timer cycles per on tick. */
#define CYCLES_PER_TICK (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC \
			 / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

/* Maximum number of ticks. */
#define MAX_TICKS (UINT32_MAX / CYCLES_PER_TICK - 2)

#ifdef CONFIG_TICKLESS_KERNEL

/*
 * Due to the nature of clock synchronization, reading from or writing to some
 * RTC registers takes approximately six RTC_GCLK cycles. This constant defines
 * a safe threshold for the comparator.
 */
#define TICK_THRESHOLD 7

#else /* !CONFIG_TICKLESS_KERNEL */

/*
 * For some reason, RTC does not generate interrupts when COMP == 0,
 * MATCHCLR == 1 and PRESCALER == 0. So we need to check that CYCLES_PER_TICK
 * is more than one.
 */
#if CYCLES_PER_TICK < 2
#error unsupported configuration for ticking mode; CYCLES_PER_TICK must be \
	greater than 2
#endif

#endif /* CONFIG_TICKLESS_KERNEL */

/* Helper macro to get the correct GCLK GEN based on configuration. */
#define GCLK_GEN(n) GCLK_EVAL(n)
#define GCLK_EVAL(n) GCLK_CLKCTRL_GEN_GCLK##n

/* Tick count of the last announce call. */
static volatile u32_t rtc_last;

#ifndef CONFIG_TICKLESS_KERNEL

/* Current tick count. */
static volatile u32_t rtc_counter;

/* Tick value of the next timeout. */
static volatile u32_t rtc_timeout;

#endif /* CONFIG_TICKLESS_KERNEL */

static void rtc_reset(void);
static void rtc_isr(void *arg);

/*
 * Waits for RTC bus synchronization.
 */
static inline void rtc_sync(void)
{
	while (RTC0->STATUS.reg & RTC_STATUS_SYNCBUSY) {
		/* Wait for bus synchronization... */
	}
}

#ifdef CONFIG_TICKLESS_KERNEL

/*
 * Reads RTC COUNT register. First a read request must be written to READREQ,
 * then - when bus synchronization completes - the COUNT register is read and
 * returned.
 */
static u32_t rtc_count(void)
{
	/* Request synchronization and read the COUNT register. */
	RTC0->READREQ.reg = RTC_READREQ_RREQ;
	rtc_sync();
	return RTC0->COUNT.reg;
}

#endif /* CONFIG_TICKLESS_KERNEL */

/*
 * Initializes the RTC peripheral.
 */
int z_clock_driver_init(struct device *device)
{
	ARG_UNUSED(device);

	/* Set up bus clock and GCLK generator. */
	PM->APBAMASK.reg |= PM_APBAMASK_RTC;
	GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(RTC_GCLK_ID) | GCLK_CLKCTRL_CLKEN
			    | GCLK_GEN(CONFIG_RTC_SAM0_CLOCK_GENERATOR);

	while (GCLK->STATUS.bit.SYNCBUSY) {
		/* Synchronize GCLK. */
	}

	/* Reset module to hardware defaults. */
	rtc_reset();

	rtc_last = 0;

	/* Configure RTC with 32-bit mode, configured prescaler and MATCHCLR. */
	u16_t ctrl = RTC_MODE0_CTRL_MODE(0) | RTC_MODE0_CTRL_PRESCALER(0);
#ifndef CONFIG_TICKLESS_KERNEL
	ctrl |= RTC_MODE0_CTRL_MATCHCLR;
#endif
	rtc_sync();
	RTC0->CTRL.reg = ctrl;

	/* Enable RTC interrupt. */
	NVIC_ClearPendingIRQ(CONFIG_RTC_SAM0_IRQ);
	IRQ_CONNECT(CONFIG_RTC_SAM0_IRQ, CONFIG_RTC_SAM0_IRQ_PRIORITY, rtc_isr,
		    0, 0);
	irq_enable(CONFIG_RTC_SAM0_IRQ);

#ifdef CONFIG_TICKLESS_KERNEL
	/* Tickless kernel lets RTC count continually and ignores overflows. */
	RTC0->INTENSET.reg = RTC_MODE0_INTENSET_CMP0;
#else
	/* Non-tickless mode uses comparator together with MATCHCLR. */
	rtc_sync();
	RTC0->COMP[0].reg = CYCLES_PER_TICK;
	RTC0->INTENSET.reg = RTC_MODE0_INTENSET_OVF;
	rtc_counter = 0;
	rtc_timeout = 0;
#endif

	/* Enable RTC module. */
	rtc_sync();
	RTC0->CTRL.reg |= RTC_MODE0_CTRL_ENABLE;

	return 0;
}

void z_clock_set_timeout(s32_t ticks, bool idle)
{
	ARG_UNUSED(idle);

#ifdef CONFIG_TICKLESS_KERNEL

	if (ticks < 0 || ticks > MAX_TICKS) {
		/* Handle K_FOREVER and too big ticks values. */
		ticks = MAX_TICKS;
	}

	/* Compute number of RTC cycles until the next timeout. */
	u32_t count = rtc_count();
	u32_t timeout = ticks * CYCLES_PER_TICK
			+ (count - rtc_last) % CYCLES_PER_TICK;

	/* Round to the nearest tick boundary. */
	timeout = (timeout + CYCLES_PER_TICK - 1) / CYCLES_PER_TICK
		  * CYCLES_PER_TICK;

	if (timeout < TICK_THRESHOLD) {
		/* Trigger the interrupt right away if ticks is too low. */
		NVIC_SetPendingIRQ(CONFIG_RTC_SAM0_IRQ);
		return;
	}

	rtc_sync();
	RTC0->COMP[0].reg = count + timeout;

#else /* !CONFIG_TICKLESS_KERNEL */

	if (ticks < 0) {
		/* Disable comparator for K_FOREVER and other negative
		 * values.
		 */
		rtc_timeout = rtc_counter;
		return;
	}

	if (ticks == 0) {
		/* Trigger the interrupt right away if ticks is zero. */
		NVIC_SetPendingIRQ(CONFIG_RTC_SAM0_IRQ);
		return;
	}

	/* Avoid race condition between reading counter and ISR incrementing
	 * it.
	 */
	int key = irq_lock();

	rtc_timeout = rtc_counter + ticks;
	irq_unlock(key);

#endif /* CONFIG_TICKLESS_KERNEL */
}

/*
 * Gets number of ticks elapsed since the last z_clock_announce call.
 */
u32_t z_clock_elapsed(void)
{
#ifdef CONFIG_TICKLESS_KERNEL
	return (rtc_count() - rtc_last) / CYCLES_PER_TICK;
#else
	return rtc_counter - rtc_last;
#endif
}

void z_clock_idle_exit(void)
{
	/* Nothing to do here, RTC bookkeeping is done in ISR. */
}

u32_t _timer_cycle_get_32(void)
{
	return (u32_t) z_tick_get() * CYCLES_PER_TICK;
}

/*
 * Resets RTC to its initial state.
 */
void rtc_reset(void)
{
	rtc_sync();

	/* Disable interrupt. */
	RTC0->INTENCLR.reg = RTC_MODE0_INTENCLR_MASK;
	/* Clear interrupt flag. */
	RTC0->INTFLAG.reg = RTC_MODE0_INTFLAG_MASK;

	/* Disable RTC module. */
	RTC0->CTRL.reg &= ~RTC_MODE0_CTRL_ENABLE;

	rtc_sync();

	/* Initiate software reset. */
	RTC0->CTRL.reg |= RTC_MODE0_CTRL_SWRST;
}

/*
 * Handles RTC interrupt.
 */
void rtc_isr(void *arg)
{
	ARG_UNUSED(arg);

	/* Read and clear the interrupt flag register. */
	u16_t status = RTC0->INTFLAG.reg;

	RTC0->INTFLAG.reg = status;

#ifdef CONFIG_TICKLESS_KERNEL

	/* Just read the current counter and announce the elapsed time. */
	u32_t count = rtc_count();

	if (count != rtc_last) {
		z_clock_announce((count - rtc_last) / CYCLES_PER_TICK);
		rtc_last = count;
	}

#else /* !CONFIG_TICKLESS_KERNEL */

	if (status) {
		/* RTC just ticked one more tick... */
		if (++rtc_counter == rtc_timeout) {
			z_clock_announce(rtc_counter - rtc_last);
			rtc_last = rtc_counter;
		}
	} else {
		/* ISR was invoked directly from z_clock_set_timeout. */
		z_clock_announce(0);
	}

#endif /* CONFIG_TICKLESS_KERNEL */
}
