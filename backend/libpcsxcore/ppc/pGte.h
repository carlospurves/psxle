/*  Pcsx - Pc Psx Emulator
 *  Copyright (C) 1999-2003  Pcsx Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef PGET_H
#define PGET_H

#ifdef __cplusplus
extern "C" {
#endif

int psxCP2time[64] = {
        2, 16 , 1 , 1, 1, 1 , 8, 1, // 00
        1 , 1 , 1 , 1, 6 , 1 , 1 , 1, // 08
        8 , 8, 8, 19, 13 , 1 , 44 , 1, // 10
        1 , 1 , 1 , 17, 11 , 1 , 14  , 1, // 18
        30 , 1 , 1 , 1, 1, 1 , 1 , 1, // 20
        5 , 8 , 17 , 1, 1, 5, 6, 1, // 28
        23 , 1 , 1 , 1, 1, 1 , 1 , 1, // 30
        1 , 1 , 1 , 1, 1, 6 , 5  , 39  // 38
};

#define CP2_FUNC(f) \
void gte##f(); \
static void rec##f() { \
	if (pc < cop2readypc) idlecyclecount += (cop2readypc - pc)>>2; \
	iFlushRegs(0); \
	LIW(0, (u32)psxRegs.code); \
	STW(0, OFFSET(&psxRegs, &psxRegs.code), GetHWRegSpecial(PSXREGS)); \
	FlushAllHWReg(); \
	CALLFunc ((u32)gte##f); \
	cop2readypc = pc + psxCP2time[_fFunct_(psxRegs.code)]<<2; \
}

CP2_FUNC(MFC2);
CP2_FUNC(MTC2);
CP2_FUNC(CFC2);
CP2_FUNC(CTC2);
CP2_FUNC(LWC2);
CP2_FUNC(SWC2);
CP2_FUNC(RTPS);
CP2_FUNC(OP);
CP2_FUNC(NCLIP);
CP2_FUNC(DPCS);
CP2_FUNC(INTPL);
CP2_FUNC(MVMVA);
CP2_FUNC(NCDS);
CP2_FUNC(NCDT);
CP2_FUNC(CDP);
CP2_FUNC(NCCS);
CP2_FUNC(CC);
CP2_FUNC(NCS);
CP2_FUNC(NCT);
CP2_FUNC(SQR);
CP2_FUNC(DCPL);
CP2_FUNC(DPCT);
CP2_FUNC(AVSZ3);
CP2_FUNC(AVSZ4);
CP2_FUNC(RTPT);
CP2_FUNC(GPF);
CP2_FUNC(GPL);
CP2_FUNC(NCCT);

#ifdef __cplusplus
}
#endif
#endif
