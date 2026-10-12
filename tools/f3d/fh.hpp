/*
 * arMLEt - tools/f3d/fh.hpp: test host for the F3D native routines. A mini RSP host around the fork's JIT: it runs the microcode from a
 * routine entry to its exit on the JIT (the oracle), runs the native routine from the same state, and compares everything the contract
 * lists (GPRs, vector registers, accumulator, flags, divide unit, DMEM, IMEM, the SP/DPC registers, RDRAM).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FH_HPP
#define FH_HPP
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "m64p_plugin.h"
#include "rsp_1.1.h"
#include "rsp_jit.hpp"

namespace RSP
{
extern RSP_INFO rsp;
extern JIT::CPU cpu;
} // namespace RSP
extern "C" unsigned int parallelRSPDoRspCycles(unsigned int);
extern "C" void parallelRSPInitiateRSP(RSP_INFO, unsigned int *);

#include "snap.h"


struct FH
{
    uint32_t *rdram;
    alignas(16) uint32_t dmem[1024];
    alignas(16) uint32_t imem[1024];
    uint32_t mi_intr = 0, spmem = 0, spdram = 0, rdlen = 0, wrlen = 0, status = 1, dmafull = 0, dmabusy = 0, sppc = 0, sem = 0;
    uint32_t dpc[8] = {0};
    int rdp_calls = 0;
    std::vector<uint8_t> rdp_log;     // concatenated lists as the video plugin would see them
    static FH *self;
    static void cb_nop() {}
    static void cb_rdp()
    {
        FH *h = self;
        h->rdp_calls++;
        uint32_t s = h->dpc[2] & 0xffffff, e = h->dpc[1] & 0xffffff;
        for (uint32_t a = s; a < e; a += 4) {
            uint32_t w = h->rdram[(a & 0x7fffff) >> 2];
            h->rdp_log.insert(h->rdp_log.end(), (uint8_t *)&w, (uint8_t *)&w + 4);
        }
        h->dpc[0] = h->dpc[2] = h->dpc[1];
    }
    FH()
    {
        self = this;
        rdram = (uint32_t *)calloc(1, 8 << 20);
        RSP_INFO i;
        memset(&i, 0, sizeof i);
        i.RDRAM = (unsigned char *)rdram;
        i.DMEM = (unsigned char *)dmem;
        i.IMEM = (unsigned char *)imem;
        i.MI_INTR_REG = &mi_intr;
        i.SP_MEM_ADDR_REG = &spmem;
        i.SP_DRAM_ADDR_REG = &spdram;
        i.SP_RD_LEN_REG = &rdlen;
        i.SP_WR_LEN_REG = &wrlen;
        i.SP_STATUS_REG = &status;
        i.SP_DMA_FULL_REG = &dmafull;
        i.SP_DMA_BUSY_REG = &dmabusy;
        i.SP_PC_REG = &sppc;
        i.SP_SEMAPHORE_REG = &sem;
        i.DPC_START_REG = &dpc[0];
        i.DPC_END_REG = &dpc[1];
        i.DPC_CURRENT_REG = &dpc[2];
        i.DPC_STATUS_REG = &dpc[3];
        i.DPC_CLOCK_REG = &dpc[4];
        i.DPC_BUFBUSY_REG = &dpc[5];
        i.DPC_PIPEBUSY_REG = &dpc[6];
        i.DPC_TMEM_REG = &dpc[7];
        i.CheckInterrupts = cb_nop;
        i.ProcessRdpList = cb_rdp;
        unsigned cc;
        parallelRSPInitiateRSP(i, &cc);
    }
    RSP::CPUState &st() { return RSP::cpu.get_state(); }

    // DMEM / RDRAM accessors, byte addresses as the RSP / CPU see them
    uint8_t dm8(unsigned a) const { return ((const uint8_t *)dmem)[(a ^ 3) & 0xfff]; }
    uint16_t dm16(unsigned a) const { return ((const uint16_t *)dmem)[((a ^ 2) & 0xfff) >> 1]; }
    uint32_t dm32(unsigned a) const { return dmem[(a & 0xfff) >> 2]; }
    void s8(unsigned a, uint8_t v) { ((uint8_t *)dmem)[(a ^ 3) & 0xfff] = v; }
    void s16(unsigned a, uint16_t v) { ((uint16_t *)dmem)[((a ^ 2) & 0xfff) >> 1] = v; }
    void s32(unsigned a, uint32_t v) { dmem[(a & 0xfff) >> 2] = v; }
    void r16(unsigned a, uint16_t v) { ((uint16_t *)rdram)[(a ^ 2) >> 1] = v; }
    void r32(unsigned a, uint32_t v) { rdram[a >> 2] = v; }
    uint16_t rd16(unsigned a) const { return ((const uint16_t *)rdram)[(a ^ 2) >> 1]; }
    uint32_t rd32(unsigned a) const { return rdram[a >> 2]; }

    static std::vector<uint8_t> readfile(const std::string &p)
    {
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) {
            perror(p.c_str());
            exit(1);
        }
        std::vector<uint8_t> v;
        uint8_t b[4096];
        size_t n;
        while ((n = fread(b, 1, 4096, f)) > 0)
            v.insert(v.end(), b, b + n);
        fclose(f);
        return v;
    }

    void save(Snap &s)
    {
        RSP::CPUState &c = st();
        memcpy(s.sr, c.sr, sizeof s.sr);
        for (int i = 0; i < 32; i++)
            memcpy(s.vr[i], c.cp2.regs[i].e, 16);
        memcpy(s.acc, c.cp2.acc.e, 48);
        for (int i = 0; i < 3; i++)
            memcpy(s.flags[i], c.cp2.flags[i].e, 32);
        s.div_out = c.cp2.div_out;
        s.div_in = c.cp2.div_in;
        s.dp_flag = c.cp2.dp_flag;
        memcpy(s.dmem, dmem, 4096);
        memcpy(s.imem, imem, 4096);
        for (int i = 0; i < 16; i++)
            s.cr[i] = *c.cp0.cr[i];
        s.irq = *c.cp0.irq;
        s.pc = c.pc;
    }
    void load(const Snap &s)
    {
        RSP::CPUState &c = st();
        memcpy(c.sr, s.sr, sizeof s.sr);
        for (int i = 0; i < 32; i++)
            memcpy(c.cp2.regs[i].e, s.vr[i], 16);
        memcpy(c.cp2.acc.e, s.acc, 48);
        for (int i = 0; i < 3; i++)
            memcpy(c.cp2.flags[i].e, s.flags[i], 32);
        c.cp2.div_out = s.div_out;
        c.cp2.div_in = s.div_in;
        c.cp2.dp_flag = s.dp_flag;
        memcpy(dmem, s.dmem, 4096);
        memcpy(imem, s.imem, 4096);
        for (int i = 0; i < 16; i++)
            *c.cp0.cr[i] = s.cr[i];
        *c.cp0.irq = s.irq;
        c.pc = s.pc;
        c.has_delay_slot = 0;
        RSP::cpu.invalidate_imem();
    }

    // Runs until any of the stop pcs is about to execute; returns the pc it stopped at (or -1 on runaway)
    int run_any(unsigned pc, const std::vector<unsigned> &stops)
    {
        std::vector<uint32_t> saved;
        for (unsigned s : stops) { saved.push_back(imem[s >> 2]); imem[s >> 2] = 0x0000000d; }
        RSP::cpu.invalidate_imem();
        st().pc = pc;
        *st().cp0.cr[4] = 0;
        *st().cp0.irq = 0;
        RSP::ReturnMode m;
        int guard = 0, rc = 0;
        do {
            m = RSP::cpu.run();
            if (++guard > 1000000) { rc = -1; break; }
        } while (m != RSP::MODE_BREAK);
        unsigned at = st().pc - 4;
        for (size_t i = 0; i < stops.size(); i++) imem[stops[i] >> 2] = saved[i];
        RSP::cpu.invalidate_imem();
        *st().cp0.cr[4] = 0;
        *st().cp0.irq = 0;
        st().pc = at;
        return rc ? -1 : (int)at;
    }

    // Records the order in which the probe pcs are first reached (each is hit once; then it is disarmed), up to the stop pc
    std::vector<unsigned> trace(unsigned pc, std::vector<unsigned> probes, unsigned stop)
    {
        std::vector<unsigned> order;
        std::vector<unsigned> armed = probes;
        armed.push_back(stop);
        std::vector<uint32_t> saved;
        for (unsigned s : armed) saved.push_back(imem[s >> 2]);
        unsigned cur = pc;
        for (int round = 0; round < 64; round++) {
            for (unsigned s : armed) imem[s >> 2] = 0x0000000d;
            RSP::cpu.invalidate_imem();
            st().pc = cur;
            *st().cp0.cr[4] = 0;
            int guard = 0;
            RSP::ReturnMode m;
            do { m = RSP::cpu.run(); if (++guard > 100000) break; } while (m != RSP::MODE_BREAK);
            for (size_t i = 0; i < armed.size(); i++) imem[armed[i] >> 2] = saved[i];
            *st().cp0.cr[4] = 0;
            if (m != RSP::MODE_BREAK) { order.push_back(0xffff); break; }
            unsigned at = st().pc - 4;
            order.push_back(at);
            if (at == stop) break;
            for (size_t i = 0; i < armed.size(); i++) if (armed[i] == at) { armed.erase(armed.begin() + i); saved.erase(saved.begin() + i); break; }
            cur = at;
        }
        RSP::cpu.invalidate_imem();
        return order;
    }

    // Runs the JIT from pc until it is about to execute stop_pc (a break is planted there). Returns 0 on success.
    int run(unsigned pc, unsigned stop_pc)
    {
        uint32_t saved = imem[stop_pc >> 2];
        imem[stop_pc >> 2] = 0x0000000d;
        RSP::cpu.invalidate_imem();
        st().pc = pc;
        *st().cp0.cr[4] = 0;
        *st().cp0.irq = 0;
        RSP::ReturnMode m;
        int guard = 0, rc = 0;
        do {
            m = RSP::cpu.run();
            if (++guard > 1000000) {
                fprintf(stderr, "runaway\n");
                rc = -1;
                break;
            }
        } while (m != RSP::MODE_BREAK);
        imem[stop_pc >> 2] = saved;
        RSP::cpu.invalidate_imem();
        *st().cp0.cr[4] = 0;
        *st().cp0.irq = 0;
        st().pc = stop_pc;
        return rc;
    }
};
#endif
