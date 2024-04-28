//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include <string.h>
#include <stdlib.h>
#include "icache.h"
#include "util.h"
#include "endian.h"
#include "CPU.h"


#define ICACHE_L		5	//line size is 2^L bytes
#define ICACHE_S		11	//number of sets is 2^S
#define ICACHE_A		1	//set associativity (less for speed)


#define ICACHE_LINE_SZ		(1 << ICACHE_L)
#define ICACHE_BUCKET_NUM	(1 << ICACHE_S)
#define ICACHE_BUCKET_SZ	(ICACHE_A)


#define ICACHE_ADDR_MASK	((uint32_t)-ICACHE_LINE_SZ)
#define ICACHE_USED_MASK	1UL
#define ICACHE_PRIV_MASK	2UL

struct icacheLine {

	uint32_t info;	//addr, masks
	uint8_t data[ICACHE_LINE_SZ];
};

struct icache {

	struct ArmMem* mem;
	struct ArmMmu* mmu;
	
	struct icacheLine lines[ICACHE_BUCKET_NUM][ICACHE_BUCKET_SZ];
	uint8_t ptr[ICACHE_BUCKET_NUM];
};


void icacheInval(struct icache *ic)
{
	uint_fast16_t i, j;
	
	for (i = 0; i < ICACHE_BUCKET_NUM; i++) {
		for(j = 0; j < ICACHE_BUCKET_SZ; j++)
			ic->lines[i][j].info = 0;
		ic->ptr[i] = 0;
	}
}

struct icache* icacheInit(struct ArmMem* mem, struct ArmMmu *mmu)
{
	struct icache *ic = (struct icache*)malloc(sizeof(*ic));
	
	if (!ic)
		ERR("cannot alloc icache");
	
	memset(ic, 0, sizeof (*ic));
	
	ic->mem = mem;
	ic->mmu = mmu;
	
	icacheInval(ic);
	
	return ic;	
}

static uint_fast16_t icachePrvHash(uint32_t addr)
{
	addr >>= ICACHE_L;
	addr &= (1UL << ICACHE_S) - 1UL;

	return addr;
}

void icacheInvalAddr(struct icache* ic, uint32_t va)
{
	uint32_t off = va % ICACHE_LINE_SZ;
	int_fast16_t i, j, bucket;
	struct icacheLine *lines;
	
	va -= off;

	bucket = icachePrvHash(va);
	lines = ic->lines[bucket];
	
	for (i = 0, j = ic->ptr[bucket]; i < ICACHE_BUCKET_SZ; i++) {
		
		if (--j == -1)
			j = ICACHE_BUCKET_SZ - 1;
		
		if ((lines[j].info & (ICACHE_ADDR_MASK | ICACHE_USED_MASK)) == (va | ICACHE_USED_MASK))	//found it!
			lines[j].info = 0;
	}
}

bool icacheFetch(struct icache* ic, uint32_t va, uint_fast8_t sz, bool priviledged, uint_fast8_t* fsrP, void* buf)
{
	struct icacheLine *lines, *line = NULL;
	uint32_t off = va % ICACHE_LINE_SZ;
	int_fast16_t i, j, bucket;
	bool needRead = false;
	
	va -= off;
	
	if (va & (sz - 1)) {	//alignment issue
		
		if (fsrP)
			*fsrP = 3;
		return false;
	}

	bucket = icachePrvHash(va);
	lines = ic->lines[bucket];
	
	for (i = 0, j = ic->ptr[bucket]; i < ICACHE_BUCKET_SZ; i++) {
		
		if (--j == -1)
			j = ICACHE_BUCKET_SZ - 1;
		
		if ((lines[j].info & (ICACHE_ADDR_MASK | ICACHE_USED_MASK)) == (va | ICACHE_USED_MASK)) {	//found it!
		
			if (!priviledged && (lines[j].info & ICACHE_PRIV_MASK)) {	//we found a line but it was cached as priviledged and we are not sure if unpriv can access it
				
				//attempt a re-read. if it passes, remove priv flag
				needRead = true;
			}
			
			line = &lines[j];
			break;
		}
	}
	
	if (!line) {
		
		needRead = true;
		
		j = ic->ptr[bucket]++;
		if (ic->ptr[bucket] == ICACHE_BUCKET_SZ)
			ic->ptr[bucket] = 0;
		line = lines + j;
	}
	
	if (needRead) {
	
		uint8_t data[ICACHE_LINE_SZ], mappingInfo;
		uint32_t pa;
	
		//if we're here, we found nothing - maybe time to populate the cache
		
		if (!mmuTranslate(ic->mmu, va, priviledged, false, &pa, fsrP, &mappingInfo))
			return false;
		
		if (!mmuIsOn(ic->mmu) || !(mappingInfo & MMU_MAPPING_CACHEABLE)) {	//uncacheable mapping or mmu is off - just do the read we were asked to and do not fill the line
			
			if (!memAccess(ic->mem, pa + off, sz, MEM_ACCESS_TYPE_READ, buf)) {
				
				if (fsrP)
					*fsrP = 0x0d;	//perm error
				
				return false;
			}
			
			return true;
		}
		
		if (!memAccess(ic->mem, pa, ICACHE_LINE_SZ, MEM_ACCESS_TYPE_READ, data)) {
			
			if (fsrP)
				*fsrP = 0x0d;	//perm error
			
			return false;
		}
	
		memcpy(line->data, data, ICACHE_LINE_SZ);
		line->info = va | (priviledged ? ICACHE_PRIV_MASK : 0) | ICACHE_USED_MASK;
	}
		
	if (sz == 4)
		*(uint32_t*)buf = *(uint32_t*)(line->data + off);
	else if (sz == 2) {
		//icache reads in words, but code requests may come in halfwords
		//on BE hosts this means we need to swap the order of halfwords
		// (to unswap what he had already swapped)
		#if __BYTE_ORDER == __BIG_ENDIAN
			*(uint16_t*)buf = *(uint16_t*)(line->data + (off ^ 2));
		#elif __BYTE_ORDER == __LITTLE_ENDIAN
			*(uint16_t*)buf = *(uint16_t*)(line->data + off);
		#else
			#error "WTF"
		#endif
	}
	else
		memcpy(buf, line->data + off, sz);
	
	return priviledged || !(line->info & ICACHE_PRIV_MASK);
}
