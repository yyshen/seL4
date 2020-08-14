#
# Copyright 2020, DornerWorks
# Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: GPL-2.0-only
#

cmake_minimum_required(VERSION 3.7.2)

declare_platform(spike KernelPlatformSpike PLAT_SPIKE KernelArchRiscV)

if(KernelPlatformSpike)
    if("${KernelSel4Arch}" STREQUAL riscv32)
        declare_seL4_arch(riscv32)
    elseif("${KernelSel4Arch}" STREQUAL riscv64)
        declare_seL4_arch(riscv64)
    else()
        fallback_declare_seL4_arch_default(riscv64)
    endif()
    config_set(KernelRiscVPlatform RISCV_PLAT "spike")
    config_set(KernelPlatformFirstHartID FIRST_HART_ID 0)
    if(KernelSel4ArchRiscV32)
        list(APPEND KernelDTSList "tools/dts/spike32.dts")
    else()
        list(APPEND KernelDTSList "tools/dts/spike.dts")
        list(APPEND KernelDTSList "src/plat/spike/overlay-spike.dts")
    endif()
    declare_default_headers(
        TIMER_FREQUENCY 10000000llu PLIC_MAX_NUM_INT 128
        INTERRUPT_CONTROLLER drivers/irq/hifive.h
        #        TIMER_FREQUENCY 10000000llu PLIC_MAX_NUM_INT 0
        #INTERRUPT_CONTROLLER arch/machine/plic.h

    )
else()
    unset(KernelPlatformFirstHartID CACHE)
endif()
