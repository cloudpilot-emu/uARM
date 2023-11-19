//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include <string.h>
#include <stdlib.h>
#include "util.h"
#include "MMU.h"
#include "mem.h"


//less sets is faster
#define MMU_TLB_BUCKET_SIZE		2
#define MMU_TLB_BUCKET_NUM		128


struct ArmPrvTlb {
	
	uint32_t pa, va;
	uint32_t sz;
	uint32_t ap:2;
	uint32_t domain:4;
	uint32_t c:1;
	uint32_t b:1;
};

struct ArmMmu {

	struct ArmMem *mem;
	uint32_t transTablPA;
	uint8_t S:1;
	uint8_t R:1;
	uint8_t xscale:1;
	uint16_t readPos[MMU_TLB_BUCKET_NUM];
	uint16_t replPos[MMU_TLB_BUCKET_NUM];
	struct ArmPrvTlb tlb[MMU_TLB_BUCKET_NUM][MMU_TLB_BUCKET_SIZE];
	uint32_t domainCfg;
};

void mmuTlbFlush(struct ArmMmu *mmu)
{
	memset(mmu->replPos, 0, sizeof(mmu->replPos));
	memset(mmu->replPos, 0, sizeof(mmu->replPos));
	memset(mmu->tlb, 0, sizeof(mmu->tlb));
}

void mmuReset(struct ArmMmu *mmu)
{
	mmu->transTablPA = MMU_DISABLED_TTP;
	mmuTlbFlush(mmu);
}

struct ArmMmu* mmuInit(struct ArmMem *mem, bool xscaleMode)
{
	struct ArmMmu *mmu = (struct ArmMmu*)malloc(sizeof(*mmu));
	
	if (!mmu)
		ERR("cannot alloc MMU");
	
	memset(mmu, 0, sizeof (*mmu));
	mmu->mem = mem;
	mmu->xscale = xscaleMode;
	mmuReset(mmu);
	
	return mmu;
}

bool mmuIsOn(struct ArmMmu *mmu)
{
	return mmu->transTablPA != MMU_DISABLED_TTP;
}

static uint_fast16_t mmuPrvHashAddr(uint32_t addr)	//addresses are granular on 1K 
{
	addr >>= 10;
	
	addr = addr ^ (addr >> 5) ^ (addr >> 10);
	
	return addr % MMU_TLB_BUCKET_NUM;
}

bool mmuTranslate(struct ArmMmu *mmu, uint32_t adr, bool priviledged, bool write, uint32_t* paP, uint_fast8_t* fsrP, uint8_t *mappingInfoP)
{
	bool c = false, b = false, ur = false, uw = false, sr = false, sw = false;
	bool section = false, coarse = true, pxa_tex_page = false;
	uint32_t va, pa = 0, sz, t;
	int_fast16_t i, j, bucket;
	uint_fast8_t dom, ap = 0;
	
	//handle the 'MMU off' case
		
	if (mmu->transTablPA == MMU_DISABLED_TTP) {
		va = pa = 0;
		goto calc;
	}

	//check the TLB
	if (MMU_TLB_BUCKET_SIZE && MMU_TLB_BUCKET_NUM) {
		
		bucket = mmuPrvHashAddr(adr);
				
		for (j = 0, i = mmu->readPos[bucket]; j < MMU_TLB_BUCKET_SIZE; j++, i = (i + MMU_TLB_BUCKET_SIZE - 1) % MMU_TLB_BUCKET_SIZE) {
			
			va = mmu->tlb[bucket][i].va;
			sz = mmu->tlb[bucket][i].sz;
			
			if (adr >= va && adr - va < sz) {
				
				pa = mmu->tlb[bucket][i].pa;
				ap = mmu->tlb[bucket][i].ap;
				dom = mmu->tlb[bucket][i].domain;
				c = !!mmu->tlb[bucket][i].c;
				b = !!mmu->tlb[bucket][i].b;
				mmu->readPos[bucket] = i;
								
				goto check;
			}
		}
	}
	
	//read first level table
	if (mmu->transTablPA & 3) {
		if (fsrP)
			*fsrP = 0x01;	//alignment fault
		return false;
	}
	
	if (!memAccess(mmu->mem, mmu->transTablPA + ((adr & 0xFFF00000ul) >> 18), 4, MEM_ACCESS_TYPE_READ, &t)) {
		
		if (fsrP)
			*fsrP = 0x0C;	//translation external abort first level
		return false;
	}
	
	dom = (t >> 5) & 0x0F;
	switch (t & 3) {
		
		case 0:	//fault
		
			if (fsrP)
				*fsrP = 0x5;	//section translation fault
			return false;
		
		case 1:	//coarse pagetable
			
			t &= 0xFFFFFC00UL;
			t += (adr & 0x000FF000UL) >> 10;
			break;
		
		case 2:	//1MB section
		
			pa = t & 0xFFF00000UL;
			va = adr & 0xFFF00000UL;
			sz = 1UL << 20;
			ap = (t >> 10) & 3;
			c = !!(t & 0x08);
			b = !!(t & 0x04);
			section = true;
			goto translated;
			
		case 3:	//fine page table
			
			coarse = false;
			t &= 0xFFFFF000UL;
			t += (adr & 0x000FFC00UL) >> 8;
			break;
	}
	
	
	//read second level table
	if (!memAccess(mmu->mem, t, 4, MEM_ACCESS_TYPE_READ, &t)) {
		if (fsrP)
			*fsrP = 0x0E | (dom << 4);	//translation external abort second level
		return false;
	}
	
	c = !!(t & 0x08);
	b = !!(t & 0x04);
	
	switch (t & 3) {
		
		case 0:	//fault
		
			if (fsrP)
				*fsrP = 0x07 | (dom << 4);	//page translation fault
			return false;
		
		case 1:	//64K mapping
			
			pa = t & 0xFFFF0000UL;
			va = adr & 0xFFFF0000UL;
			sz = 65536UL;
			ap = (adr >> 14) & 3;		//in "ap" store which AP we need [of the 4]
			
			break;
		
		case 2:	//4K mapping (1K effective thenks to having 4 AP fields)
		
			ap = (adr >> 10) & 3;		//in "ap" store which AP we need [of the 4]
			
page_size_4k:
			pa = t & 0xFFFFF000UL;
			va = adr & 0xFFFFF000UL;
			sz = 4096;
			break;
			
		case 3:	//1K mapping
			
			if (coarse) {
				
				if (mmu->xscale) {
					pxa_tex_page = true;
					ap = 0;
					goto page_size_4k;	
				}
				else {
					
					fprintf(stderr, "invalid coarse page table entry\r\n");
					if (fsrP)
						*fsrP = 7;
				}
			}
			
			pa = t & 0xFFFFFC00UL;
			va = adr & 0xFFFFFC00UL;
			ap = (t >> 4) & 3;		//in "ap" store the actual AP [and skip quarter-page resolution later using the goto]
			sz = 1024;
			goto translated;
	}
	
	
	//handle 4 AP sections

	i = (t >> 4) & 0xFF;
	if (pxa_tex_page || ((i & 0x0F) == (i >> 4) && (i & 0x03) == ((i >> 2) & 0x03)))	//if all domains are the same, add the whole thing
		ap = (t >> 4) & 3;
		
	else{	//take the quarter that is the one we need
	
		sz /= 4;
		pa += ((uint32_t)ap) * sz;
		va += ((uint32_t)ap) * sz;
		ap = (t >> (4 + 2 * ap)) & 3;
	}
	
	
translated:

	//insert tlb entry
	if (MMU_TLB_BUCKET_NUM && MMU_TLB_BUCKET_SIZE) {
		
		mmu->tlb[bucket][mmu->replPos[bucket]].pa = pa;
		mmu->tlb[bucket][mmu->replPos[bucket]].sz = sz;
		mmu->tlb[bucket][mmu->replPos[bucket]].va = va;
		mmu->tlb[bucket][mmu->replPos[bucket]].ap = ap;
		mmu->tlb[bucket][mmu->replPos[bucket]].domain = dom;
		mmu->tlb[bucket][mmu->replPos[bucket]].c = c ? 1 : 0;
		mmu->tlb[bucket][mmu->replPos[bucket]].b = b ? 1 : 0;
		mmu->readPos[bucket] = mmu->replPos[bucket];
		if (++mmu->replPos[bucket] == MMU_TLB_BUCKET_SIZE)
			mmu->replPos[bucket] = 0;
	}

check:
				
	//check domain permissions
	
	switch ((mmu->domainCfg >> (dom * 2)) & 3) {
		
		case 0:	//NO ACCESS:
		case 2:	//RESERVED: unpredictable	(treat as no access)
			
			if (fsrP)
				*fsrP = (section ? 0x08 : 0xB) | (dom << 4);	//section or page domain fault
			return false;
			
			
		case 1:	//CLIENT: check permissions
		
			break;
		
			
		case 3:	//MANAGER: allow all access
			
			ur = true;
			uw = true;
			sr = true;
			sw = true;
			
			goto calc;
		
	}

	//check permissions 
	
	switch (ap) {
		
		case 0:
			
			ur = mmu->R;
			sr = mmu->S || mmu->R;
			
			if (write || (!mmu->R && (!priviledged || !mmu->S)))
				break;
			goto calc;
		
		case 1:
		
			sr = true;
			sw = true;
			
			if (!priviledged)
				break;
			goto calc;

		case 2:
		
			ur = true;
			sr = true;
			sw = true;
			
			if (!priviledged && write)
				break;
			goto calc;
		
		case 3:
		
			ur = true;
			uw = true;
			sr = true;
			sw = true;
		
			//all is good, allow access!
			goto calc;
	}
	
//perm_err:

	if (fsrP)
		*fsrP = (section ? 0x0D : 0x0F) | (dom << 4);		//section or subpage permission fault
	return false;
	
calc:
	if (mappingInfoP) {
		*mappingInfoP =
					(c ? MMU_MAPPING_CACHEABLE : 0) |
					(b ? MMU_MAPPING_BUFFERABLE : 0) |
					(ur ? MMU_MAPPING_UR : 0) |
					(uw ? MMU_MAPPING_UW : 0) |
					(sr ? MMU_MAPPING_SR : 0) |
					(sw ? MMU_MAPPING_SW : 0);
	}
	*paP = adr - va + pa;
	return true;
}

uint32_t mmuGetTTP(struct ArmMmu *mmu)
{
	return mmu->transTablPA;
}

void mmuSetTTP(struct ArmMmu *mmu, uint32_t ttp)
{
	mmuTlbFlush(mmu);
	mmu->transTablPA = ttp;
}

void mmuSetS(struct ArmMmu *mmu, bool on)
{
	mmu->S = on;	
}

void mmuSetR(struct ArmMmu *mmu, bool on)
{
	mmu->R = on;	
}

bool mmuGetS(struct ArmMmu *mmu)
{
	return mmu->S;
}

bool mmuGetR(struct ArmMmu *mmu)
{
	return mmu->R;
}

uint32_t mmuGetDomainCfg(struct ArmMmu *mmu)
{
	return mmu->domainCfg;
}

void mmuSetDomainCfg(struct ArmMmu *mmu, uint32_t val)
{
	mmu->domainCfg = val;
}

///////////////////////////  debugging helpers  ///////////////////////////



static uint32_t mmuPrvDebugRead(struct ArmMmu *mmu, uint32_t addr)
{
	uint32_t t;
	
	if (!memAccess(mmu->mem, addr, 4, MEM_ACCESS_TYPE_READ, &t))
		t = 0xFFFFFFF0UL;
	
	return t;
}

static void mmuPrvDumpUpdate(uint32_t va, uint32_t pa, uint32_t len, uint8_t dom, uint8_t ap, bool c, bool b, bool valid)
{	
	static bool wasValid = false;
	static uint32_t expectPa = 0;
	static uint32_t startVa = 0;
	static uint32_t startPa = 0;
	static uint8_t wasDom = 0;
	static uint8_t wasAp = 0;
	static bool wasB = 0;
	static bool wasC = 0;
	uint32_t va_end;
	
	
	va_end = (va || len) ? va - 1 : 0xFFFFFFFFUL;
	
	if (!wasValid && !valid)
		return;	//no need to bother...
	
	if (valid != wasValid || dom != wasDom || ap != wasAp || c != wasC || b != wasB || expectPa != pa) {	//not a continuation of what we've been at...
		
		if (wasValid)
			fprintf(stderr, "0x%08lx - 0x%08lx -> 0x%08lx - 0x%08lx don%u ap%u %c %c\n",
				(unsigned long)startVa, (unsigned long)va_end, (unsigned long)startPa, (unsigned long)(startPa + (va_end - startVa)),
				wasDom, wasAp, (char)(wasC ? 'c' : ' '), (char)(wasB ? 'b' : ' '));
		
		wasValid = valid;
		if (valid) {	//start of a new range
			
			wasDom = dom;
			wasAp = ap;
			wasC = c;
			wasB = b;
			startVa = va;
			startPa = pa;
			expectPa = pa + len;
		}
	}
	else	//continuation of what we've been up to...
		expectPa += len;
}

void __attribute__((used)) mmuDump(struct ArmMmu *mmu)
{
	uint32_t i, j, t, sla, va, psz;
	bool coarse = false;
	uint_fast8_t dom;
	
	for (i = 0; i < 0x1000; i++) {
		
		t = mmuPrvDebugRead(mmu, mmu->transTablPA + (i << 2));
		va = i << 20;
		dom = (t >> 5) & 0x0F;
		switch (t & 3) {
			
			case 0:		//done
				mmuPrvDumpUpdate(va, 0, 1UL << 20, 0, 0, false, false, false);
				continue;
			
			case 1:		//coarse page table
				coarse = true;
				t &= 0xFFFFFC00UL;
				break;
			
			case 2:		//section
				mmuPrvDumpUpdate(va, t & 0xFFF00000UL, 1UL << 20, dom, (t >> 10) & 3, !!(t & 8), !!(t & 4), true);
				continue;
			
			case 3:		//fine page table
				t &= 0xFFFFF000UL;
				break;
		}
		
		sla = t;
		psz = coarse ? 4096 : 1024;
		for (j = 0; j < ((1UL << 20) / psz); j++) {
			t = mmuPrvDebugRead(mmu, sla + (j << 2));
			va = (i << 20) + (j * psz);
			switch (t & 3) {
				
				case 0:		//invalid
					mmuPrvDumpUpdate(va, 0, psz, 0, 0, false, false, false);
					break;
				
				case 1:		//large 64k page
					mmuPrvDumpUpdate(va + 0 * 16384UL, (t & 0xFFFF0000UL) + 0 * 16384UL, 16384, dom, (t >>  4) & 3, !!(t & 8), !!(t & 4), true);
					mmuPrvDumpUpdate(va + 1 * 16384UL, (t & 0xFFFF0000UL) + 1 * 16384UL, 16384, dom, (t >>  6) & 3, !!(t & 8), !!(t & 4), true);
					mmuPrvDumpUpdate(va + 2 * 16384UL, (t & 0xFFFF0000UL) + 2 * 16384UL, 16384, dom, (t >>  8) & 3, !!(t & 8), !!(t & 4), true);
					mmuPrvDumpUpdate(va + 3 * 16384UL, (t & 0xFFFF0000UL) + 3 * 16384UL, 16384, dom, (t >> 10) & 3, !!(t & 8), !!(t & 4), true);
					j += coarse ? 15 : 63;
					break;
				
				case 2:		//small 4k page
					mmuPrvDumpUpdate(va + 0 * 1024, (t & 0xFFFFF000UL) + 0 * 1024, 1024, dom, (t >>  4) & 3, !!(t & 8), !!(t & 4), true);
					mmuPrvDumpUpdate(va + 1 * 1024, (t & 0xFFFFF000UL) + 1 * 1024, 1024, dom, (t >>  6) & 3, !!(t & 8), !!(t & 4), true);
					mmuPrvDumpUpdate(va + 2 * 1024, (t & 0xFFFFF000UL) + 2 * 1024, 1024, dom, (t >>  8) & 3, !!(t & 8), !!(t & 4), true);
					mmuPrvDumpUpdate(va + 3 * 1024, (t & 0xFFFFF000UL) + 3 * 1024, 1024, dom, (t >> 10) & 3, !!(t & 8), !!(t & 4), true);
					if(!coarse)
						j += 3;
					break;
				
				case 3:		//tiny 1k page or TEX page on pxa
					if (coarse)
						mmuPrvDumpUpdate(va, t & 0xFFFFF000UL, 4096, dom, (t >> 4) & 3, !!(t & 8), !!(t & 4), true);
					else
						mmuPrvDumpUpdate(va, t & 0xFFFFFC00UL, 1024, dom, (t >> 4) & 3, !!(t & 8), !!(t & 4), true);
					break;
			}
		}
	}
	mmuPrvDumpUpdate(0, 0, 0, 0, 0, false, false, false);	//finish things off
}
