//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _ICACHE_H_
#define _ICACHE_H_


#include <stdbool.h>
#include <stdint.h>
#include "mem.h"
#include "MMU.h"
#include "CPU.h"

struct icache;


struct icache* icacheInit(struct ArmMem* mem, struct ArmMmu *mmu);
bool icacheFetch(struct icache* ic, uint32_t va, uint_fast8_t sz, bool priviledged, uint_fast8_t* fsr, void* buf);
void icacheInval(struct icache* ic);
void icacheInvalAddr(struct icache* ic, uint32_t addr);



#endif

