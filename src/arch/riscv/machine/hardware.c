/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2015, 2016 Hesham Almatary <heshamelmatary@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <config.h>
#include <types.h>
#include <machine/registerset.h>
#include <machine/timer.h>
#include <arch/machine.h>
#include <arch/smp/ipi.h>


#define SIPI_IP   1
#define SIPI_IE   1
#define STIMER_IP 5
#define STIMER_IE 5
#define STIMER_CAUSE 5
#define SEXTERNAL_IP 9
#define SEXTERNAL_IE 9
#define SEXTERNAL_CAUSE 9

#ifndef CONFIG_KERNEL_MCS
#define RESET_CYCLES ((TIMER_CLOCK_HZ / MS_IN_S) * CONFIG_TIMER_TICK_MS)
#endif /* !CONFIG_KERNEL_MCS */

#define IS_IRQ_VALID(X) (((X)) <= maxIRQ && (X)!= irqInvalid)

word_t PURE getRestartPC(tcb_t *thread)
{
    return getRegister(thread, FaultIP);
}

void setNextPC(tcb_t *thread, word_t v)
{
    setRegister(thread, NextIP, v);
}

BOOT_CODE int get_num_avail_p_regs(void)
{
    return sizeof(avail_p_regs) / sizeof(p_region_t);
}

BOOT_CODE p_region_t *get_avail_p_regs(void)
{
    return (p_region_t *) avail_p_regs;
}

BOOT_CODE void map_kernel_devices(void)
{

    map_kernel_frame(0x00000000, PLIC_PPTR, VMKernelOnly);

    if (kernel_devices == NULL) {
        return;
    }

    for (int i = 0; i < (sizeof(kernel_devices) / sizeof(kernel_frame_t)); i++) {
        map_kernel_frame(kernel_devices[i].paddr, KDEV_BASE,
                         VMKernelOnly);
        if (!kernel_devices[i].userAvailable) {
            p_region_t reg = {
                .start = kernel_devices[i].paddr,
                .end = kernel_devices[i].paddr + (1 << PAGE_BITS),
            };
            reserve_region(reg);
        }
    }
}

#if CONFIG_RISCV_NUM_VTIMERS > 0
#define VTIMER_MAX_CYCLES   0xffffffffffffffff
#define NUM_VTIMERS     (CONFIG_RISCV_NUM_VTIMERS + 1)
static uint64_t vtimer_cycles[NUM_VTIMERS];
static uint64_t vtimer_next_cycles = VTIMER_MAX_CYCLES;
static word_t   vtimer_next = NUM_VTIMERS;

#define KERNEL_PREEMPT_VTIMER 0


static void initVTimer(void)
{
    for (int i = 0; i < NUM_VTIMERS; i++) {
        vtimer_cycles[i] = VTIMER_MAX_CYCLES;
    }
}

void setVTimer(word_t vtimer, uint64_t cycles)
{
    vtimer_cycles[vtimer] = cycles;
    if (cycles < vtimer_next_cycles) {
        vtimer_next = vtimer;
        vtimer_next_cycles = cycles;
    }

    sbi_set_timer(vtimer_next_cycles);
    return;
}

static irq_t handleVTimer(void)
{
    assert(vtimer_next != NUM_VTIMERS);
    irq_t irq = INTERRUPT_CORE_TIMER + vtimer_next;
    vtimer_cycles[vtimer_next] = vtimer_next_cycles = VTIMER_MAX_CYCLES;

    /* Now need to scan for the next to set */
    for (int i = 0; i < NUM_VTIMERS; i++) {
        if (vtimer_cycles[i] < vtimer_next_cycles) {
            vtimer_next_cycles = vtimer_cycles[i];
            vtimer_next = i;
        }
    }
    if (vtimer_next_cycles == VTIMER_MAX_CYCLES) {
        /* no next timer */
        vtimer_next = NUM_VTIMERS;
    }
    sbi_set_timer(vtimer_next_cycles);
    return irq;
}

#endif

/*
 * The following assumes familiarity with RISC-V interrupt delivery and the PLIC.
 * See the RISC-V privileged specifivation v1.10 and the comment in
 * include/plat/spike/plat/machine.h for more information.
 * RISC-V IRQ handling on seL4 works as follows:
 *
 * On other architectures the kernel masks interrupts between delivering them to
 * userlevel and receiving the acknowledgement invocation. This strategy doesn't
 * work on RISC-V as an IRQ is implicitly masked when it is claimed, until the
 * claim is acknowledged. If we mask and unmask the interrupt at the PLIC while
 * a claim is in progress we sometimes experience IRQ sources not being masked
 * and unmasked as expected. Because of this, we don't mask and unmask IRQs that
 * are for user level, and also call plic_complete_claim for seL4_IRQHandler_Ack.
 */

/**
 * Gets the new active irq from the PLIC or STIP.
 *
 * getNewActiveIRQ is only called by getActiveIRQ and checks for a pending IRQ.
 * We read sip and if the SEIP bit is set we claim an
 * IRQ from the PLIC. If STIP is set then it is a kernel timer interrupt.
 * Otherwise we return IRQ invalid. It is possible to reveive irqInvalid from
 * the PLIC if another HART context has claimed the IRQ before us. This function
 * is not idempotent as plic_get_claim is called which accepts an IRQ message
 * from the PLIC and will claim different IRQs if called subsequent times.
 *
 * @return     The new active irq.
 */
static irq_t getNewActiveIRQ(void)
{

    uint64_t sip = read_sip();
    /* Interrupt priority (high to low ): external -> software -> timer */
    if (sip & BIT(SEXTERNAL_IP)) {
        irq_t irq = plic_get_claim();
        if (irq != irqInvalid) {
            plic_complete_claim(irq);
        }
        return irq;
#ifdef ENABLE_SMP_SUPPORT
    } else if (sip & BIT(SIPI_IP)) {
        sbi_clear_ipi();
        return ipi_get_irq();
#endif
    } else if (sip & BIT(STIMER_IP)) {
#if CONFIG_RISCV_NUM_VTIMERS > 0
        return handleVTimer();
#else
        // Supervisor timer interrupt
        return INTERRUPT_CORE_TIMER;
#endif
    }

    return irqInvalid;
}

static uint32_t active_irq[CONFIG_MAX_NUM_NODES] = { irqInvalid };


/**
 * Gets the active irq. Returns the same irq if called again before ackInterrupt.
 *
 * getActiveIRQ is used to return a currently pending IRQ. This function can be
 * called multiple times and needs to return the same IRQ until ackInterrupt is
 * called. getActiveIRQ returns irqInvalid if no interrupt is pending. It is
 * assumed that if isIRQPending is true, then getActiveIRQ will not return
 * irqInvalid. getActiveIRQ will call getNewActiveIRQ and cache its result until
 * ackInterrupt is called.
 *
 * @return     The active irq.
 */
static inline irq_t getActiveIRQ(void)
{

    uint32_t irq;
    if (!IS_IRQ_VALID(active_irq[CURRENT_CPU_INDEX()])) {
        active_irq[CURRENT_CPU_INDEX()] = getNewActiveIRQ();
    }

    if (IS_IRQ_VALID(active_irq[CURRENT_CPU_INDEX()])) {
        irq = active_irq[CURRENT_CPU_INDEX()];
    } else {
        irq = irqInvalid;
    }

    return irq;
}

#ifdef HAVE_SET_TRIGGER
/**
 * Sets the irq trigger.
 *
 * setIRQTrigger can change the trigger between edge and level at the PLIC for
 * external interrupts. It is implementation specific as whether the PLIC has
 * support for this operation.
 *
 * @param[in]  irq             The irq
 * @param[in]  edge_triggered  edge triggered otherwise level triggered
 */
void setIRQTrigger(irq_t irq, bool_t edge_triggered)
{
    plic_irq_set_trigger(irq, edge_triggered);
}
#endif

/* isIRQPending is used to determine whether to preempt long running
 * operations at various preemption points throughout the kernel. If this
 * returns true, it means that if the Kernel were to return to user mode, it
 * would then immediately take an interrupt. We check the SIP register for if
 * either a timer interrupt (STIP) or an external interrupt (SEIP) is pending.
 * We don't check software generated interrupts. These are used to perform cross
 * core signalling which isn't currently supported.
 * TODO: Add SSIP check when SMP support is added.
 */
static inline bool_t isIRQPending(void)
{
    word_t sip = read_sip();
    return (sip & (BIT(STIMER_IP) | BIT(SEXTERNAL_IP)));
}

/**
 * Disable or enable IRQs.
 *
 * maskInterrupt disables and enables IRQs. When an IRQ is disabled, it should
 * not raise an interrupt on the Kernel's HART context. This either masks the
 * core timer on the sie register or masks an external IRQ at the plic.
 *
 * @param[in]  disable  The disable
 * @param[in]  irq      The irq
 */
static inline void maskInterrupt(bool_t disable, irq_t irq)
{
    assert(IS_IRQ_VALID(irq));
    if (irq == INTERRUPT_CORE_TIMER) {
        if (disable) {
            clear_sie_mask(BIT(STIMER_IE));
        } else {
            set_sie_mask(BIT(STIMER_IE));
        }
#ifdef ENABLE_SMP_SUPPORT
    } else if (irq == irq_reschedule_ipi || irq == irq_remote_call_ipi) {
        return;
#endif
    } else {
        plic_mask_irq(disable, irq);
    }
}

/**
 * Kernel has dealt with the pending interrupt getActiveIRQ can return next IRQ.
 *
 * ackInterrupt is used by the kernel to indicate it has processed the interrupt
 * delivery and getActiveIRQ is now able to return a different IRQ number. Note
 * that this is called after a notification has been signalled to user level,
 * but before user level has handled the cause.
 *
 * @param[in]  irq   The irq
 */
static inline void ackInterrupt(irq_t irq)
{
    assert(IS_IRQ_VALID(irq));
    active_irq[CURRENT_CPU_INDEX()] = irqInvalid;

    if (irq == INTERRUPT_CORE_TIMER) {
        /* Reprogramming the timer has cleared the interrupt. */
        return;
    }
#ifdef ENABLE_SMP_SUPPORT
    if (irq == irq_reschedule_ipi || irq == irq_remote_call_ipi) {
        ipi_clear_irq(irq);
    }
#endif
}

static inline uint64_t get_cycles(void)
#if __riscv_xlen == 32
{
    uint32_t nH, nL;
    asm volatile(
        "rdtimeh %0\n"
        "rdtime  %1\n"
        : "=r"(nH), "=r"(nL));
    return ((uint64_t)((uint64_t) nH << 32)) | (nL);
}
#else
{
    uint64_t n;
    asm volatile(
        "rdtime %0"
        : "=r"(n));
    return n;
}
#endif
}

static inline int read_current_timer(unsigned long *timer_val)
{
    *timer_val = riscv_read_time();
    return 0;
}

#ifndef CONFIG_KERNEL_MCS
void resetTimer(void)
{

    uint64_t target;
    // repeatedly try and set the timer in a loop as otherwise there is a race and we
    // may set a timeout in the past, resulting in it never getting triggered
    do {
        target = get_cycles() + RESET_CYCLES;
#if CONFIG_RISCV_NUM_VTIMERS > 0
        setVTimer(KERNEL_PREEMPT_VTIMER, target);
#else
        sbi_set_timer(target);
#endif
    } while (get_cycles() > target);
}

/**
   DONT_TRANSLATE
 */
BOOT_CODE void initTimer(void)
{
#if CONFIG_RISCV_NUM_VTIMERS > 0
    initVTimer();
    setVTimer(KERNEL_PREEMPT_VTIMER, get_cycles() + RESET_CYCLES);
#else
    sbi_set_timer(get_cycles() + RESET_CYCLES);
#endif
}
#endif /* !CONFIG_KERNEL_MCS */

void plat_cleanL2Range(paddr_t start, paddr_t end)
{
}
void plat_invalidateL2Range(paddr_t start, paddr_t end)
{
}

void plat_cleanInvalidateL2Range(paddr_t start, paddr_t end)
{
}

BOOT_CODE void initL2Cache(void)
{
}

BOOT_CODE void initLocalIRQController(void)
{
    printf("Init local IRQ\n");

#ifdef CONFIG_PLAT_HIFIVE
    /* Init per-hart PLIC */
    plic_init_hart();
#endif

    word_t sie = 0;
    sie |= BIT(SEXTERNAL_IE);
    sie |= BIT(STIMER_IE);

#ifdef ENABLE_SMP_SUPPORT
    /* enable the software-generated interrupts */
    sie |= BIT(SIPI_IE);
#endif

    set_sie_mask(sie);
}

BOOT_CODE void initIRQController(void)
{
    printf("Initialing PLIC...\n");

    plic_init_controller();
}

static inline void handleSpuriousIRQ(void)
{
    /* Do nothing */
    printf("Superior IRQ!! SIP %lx\n", read_sip());
}
