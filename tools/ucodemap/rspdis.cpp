/*
 * arMLEt - tools/ucodemap/rspdis.cpp: decodes RSP instructions for the offline routine mapper (ucodemap.py), with the fork's disassembler
 *
 *   rspdis < sites > decoded        sites: lines "<idx> <pc> <instr hex> [...]"   (<out>/sites of a level-3 run)
 *   decoded line:  idx pc instr kind arg | text
 *     kind: seq (falls through), br (conditional branch, arg = target pc), j (arg = target), jal (arg = target), jr (arg = register number),
 *           jalr (arg = register), break, mtc0 (arg = CP0 register), mfc0 (arg = CP0 register), vu / ls / cop2 (arg 0)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stdio.h>

#include "rsp_disasm.hpp"

int main()
{
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned idx, pc, instr;
        if (sscanf(line, "%u %u %x", &idx, &pc, &instr) != 3)
            continue;
        const char *kind = "seq";
        unsigned arg = 0;
        unsigned t = instr >> 26;
        int16_t imm = int16_t(instr & 0xffff);
        if ((instr >> 25) == 0x25) {
            kind = "vu";
        } else {
            switch (t) {
            case 0:
                switch (instr & 63) {
                case 8: kind = "jr"; arg = (instr >> 21) & 31; break;
                case 9: kind = "jalr"; arg = (instr >> 21) & 31; break;
                case 13: kind = "break"; break;
                }
                break;
            case 1: kind = "br"; arg = (pc + 4 + imm * 4) & 0xfff; if (((instr >> 16) & 31) >= 16) kind = "brl"; break;
            case 2: kind = "j"; arg = (instr & 0x3ff) << 2; break;
            case 3: kind = "jal"; arg = (instr & 0x3ff) << 2; break;
            case 4: case 5: case 6: case 7: kind = "br"; arg = (pc + 4 + imm * 4) & 0xfff; break;
            case 16: kind = ((instr >> 21) & 31) == 4 ? "mtc0" : "mfc0"; arg = (instr >> 11) & 31; break;
            case 18: kind = "cop2"; break;
            case 50: case 58: kind = "ls"; break;
            }
        }
        std::string text = RSP::disassemble(pc, instr);
        printf("%u %u %08x %s %u | %s\n", idx, pc, instr, kind, arg, text.c_str());
    }
    return 0;
}
