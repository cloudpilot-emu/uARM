//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_PWR_CLK_H_
#define _PXA_PWR_CLK_H_

#include "mem.h"
#include "CPU.h"

struct PxaPwrClk;


struct PxaPwrClk* pxaPwrClkInit(struct ArmCpu *cpu, struct ArmMem *physMem, bool isPXA270);





#endif
