/* main.c - MemTest-86  Version 3.5
 *
 * Released under version 2 of the Gnu Public License.
 * By Chris Brady
 * Minor modifications by Mark Gottscho <mgottscho@ucla.edu> August 2011
 */
#include "stdint.h"
#include "stddef.h"
#include "test.h"
#include "defs.h"
#include "smp.h"
#undef TEST_TIMES
#define DEFTESTS 9

/* The main stack is allocated during boot time. The stack size should
 * preferably be a multiple of page size(4Kbytes)
*/
#define STACKSIZE (4*1024)
#define MAX_CPUS 16

extern void bzero();
extern long do_reloc;

const struct tseq tseq[] = {
	/* for MWG: cache_enabled, pat_num, num_iter, ??, display_name */
	{0,  5,  6, 0,  "[Address test, walking ones, no cache]"},
	{1,  16,  6, 0, "[MWG MOD WRITE ONLY, ADDR, cached]    "}, //1 MWG MOD WRITE ONLY ADDRESS TEST FOR POWER MEASUREMENTS, CACHE ENABLED
	{1,  13,  6, 0, "[MWG MOD READ ONLY, cached]           "}, //2 MWG MOD READ ONLY TEST FOR POWER MEASUREMENTS, CACHE ENABLED
	{1,  8,   1, 0, "[MWG MOD IDLE, cached]                "}, //3 MWG MOD IDLE TEST FOR POWER MEASUREMENTS. CACHE ENABLED
	{0,  16,  6, 0, "[MWG MOD WRITE ONLY 1, no cache]       "},//4 MWG MOD WRITE ONLY 1 NO CACHE 
	{0,  13,  6, 0, "[MWG MOD READ ONLY, no cache]         "}, 
	{0,  8,   1, 0, "[MWG MOD IDLE,  no cache]             "}, 
	{1,  14,  24, 0, "[MWG MOD WRITE ONLY 1, cached]        "}, //7 Use for MWG MOD ALT WRITE ONLY 1
    {1, 15,   3, 0, "[MWG MOD WRITE ONLY 0, cached]        "}, //8 Use for MWG MOD ALT WRITE ONLY 0
	{1,  12,   1, 0, "[MWG MOD WRITE ALT 1/0, cached]       "}, //9 Use for MWG MOD WRITE 1/0 ALT
	{0,  0,   0, 0, NULL}
	

	//Original test routines
	/* {0,  5,   6, 0, "[Address test, walking ones, no cache]"},
	{1,  6,   6, 0, "[Address test, own address]           "},
	{1,  0,   6, 0, "[Moving inversions, ones & zeros]     "},
	{1,  1,   3, 0, "[Moving inversions, 8 bit pattern]    "},
	{1, 10,  45, 0, "[Moving inversions, random pattern]   "},
	{1,  7,  90, 0, "[Block move]                          "},
	{1,  2,   3, 0, "[Moving inversions, 32 bit pattern]   "},
	{1,  9,  24, 0, "[Random number sequence]              "},
        {1, 11,   3, 0, "[Modulo 20, Random pattern]           "},
	{1,  8,   1, 0, "[Bit fade test, 90 min, 2 patterns]   "},
	{0,  0,   0, 0, NULL} */
};

int             num_cpus        = 1;
bool            smp_mode        = TRUE;
bool	        restart_pending = FALSE;
ulong* volatile jmp_address[MAX_CPUS];
ulong**         old_jmp_address[MAX_CPUS];
uint8_t         volatile stacks[MAX_CPUS][STACKSIZE];

char cmdline_parsed = 0;
struct vars variables = {};
struct vars * const v = &variables;

volatile ulong *p = 0;
ulong p1 = 0, p2 = 0, p0 = 0;
int bail = 0;
int test_ticks;
int segs, nticks;
ulong high_test_adr;
volatile int sched_barrier=0;
int rel;

volatile short bsp_first = 0, ap_first = 0;
volatile short cpu_sel = 0, cpu_mode = CPM_RROBIN;
static int c_iter;
static int window = 0;
static struct pmap windows[] = 
{
	{ 0/* Written at startup */, 0x080000 },   /* first 2gb */
	{ 0x000000, 0 /* Written at startup */},   /* for relocation */
	{ 0x080000, 0x100000 },			   /* 2nd 2gb */
	{ 0x100000, 0x180000 },
	{ 0x180000, 0x200000 },
	{ 0x200000, 0x280000 },
	{ 0x280000, 0x300000 },
	{ 0x300000, 0x380000 },
	{ 0x380000, 0x400000 },
	{ 0x400000, 0x480000 },
	{ 0x480000, 0x500000 },
	{ 0x500000, 0x580000 },
	{ 0x580000, 0x600000 },
	{ 0x600000, 0x680000 },
	{ 0x680000, 0x700000 },
	{ 0x700000, 0x780000 },
	{ 0x780000, 0x800000 },
	{ 0x800000, 0x880000 },
	{ 0x880000, 0x900000 },
	{ 0x900000, 0x980000 },
	{ 0x980000, 0xA00000 },
	{ 0xA00000, 0xA80000 },
	{ 0xA80000, 0xB00000 },
	{ 0xB00000, 0xB80000 },
	{ 0xB80000, 0xC00000 },
	{ 0xC00000, 0xC80000 },
	{ 0xC80000, 0xD00000 },
	{ 0xD00000, 0xD80000 },
	{ 0xD80000, 0xE00000 },
	{ 0xE00000, 0xE80000 },
	{ 0xE80000, 0xF00000 },
	{ 0xF00000, 0xF80000 },
	{ 0xF80000, 0x1000000 },
};

#if (LOW_TEST_ADR > (400*1024))
#error LOW_TEST_ADR must be below 400K
#endif

static int find_ticks_for_test(int ch, int test);
void find_ticks_for_pass(void);
static int find_chunks(void);
static void test_setup(void);
static int compute_segments(int win);
int do_test(void);

static void __run_at(unsigned long addr)
{
	do_reloc = 1;
	/* Copy memtest86 code */
	memmove((void *)addr, &_start, _end - _start);
	/* Jump to the start address */
	p = (ulong *)(addr + startup_32 - _start);
	goto *p;
}

static unsigned long run_at_addr = 0xffffffff;
static void run_at(unsigned long addr)
{
	unsigned long start;
	unsigned long len;

	run_at_addr = addr;

	start = (unsigned long) &_start;
	len = _end - _start;
	if (	((start < addr) && ((start + len) >= addr)) || 
		((addr < start) &&  ((addr + len) >= start))) {
		/* Handle overlap by doing an extra relocation */
		if (addr + len < high_test_adr) {
			__run_at(high_test_adr);
		}
		else if (start + len < addr) {
			__run_at(LOW_TEST_ADR);
		}
	}
	__run_at(run_at_addr);
}

/* Switch from the boot stack to the main stack. First the main stack
 * is allocated, then the contents of the boot stack are copied, then
 * ESP is adjusted to point to the new stack.  
 */
static void
switch_to_main_stack(unsigned my_cpu_num)
{
	extern uintptr_t boot_stack;
	extern uintptr_t boot_stack_top; 
	uintptr_t *src, *dst;
	int offs;
   
	uint8_t * stackAddr, *stackTop;
	stackAddr = (uint8_t *) &stacks[my_cpu_num][0];

	stackTop  = stackAddr + STACKSIZE;
   
	src = (uintptr_t*)&boot_stack_top;
	dst = (uintptr_t*)stackTop;
	do {
		src--; dst--;
		*dst = *src;
	} while ((uintptr_t *)src > (uintptr_t *)&boot_stack);

	offs = (uint8_t *)&boot_stack_top - stackTop;
	__asm__ __volatile__ (
	"subl %%eax, %%esp" 
		: /*no output*/
		: "a" (offs) : "memory" 
	);
}

void ap_halt(void)
{
	__asm__ __volatile__(
	"cli; 2: hlt; jmp 2b"
	:
	:
	);
}

void restart_internal(void)
{
	/* clear variables */
	smp_mode        = TRUE;
        restart_pending = FALSE;

	run_at(LOW_TEST_ADR);
}

void restart(void)
{
	bail++;
        restart_pending = TRUE;
}

void initialise_cpus(void)
{
        int cpu_num, j;

        smp_init_bsp();
        num_cpus = smp_num_cpus();
        /* let the BSP initialise the APs. */
        for(cpu_num = 1; cpu_num < num_cpus; cpu_num++) {
             smp_boot_ap(cpu_num);
             if(!smp_mode) { 
 	        /* some error in booting the AP, 
                 *  halt the already started APs */
                 for (j = 1; j <=cpu_num; j++) {
                      jmp_address[j] = (void *)ap_halt;
                 }
                 break;
             } 
        }
}

/* command line passing using the 'old' boot protocol */
#define MK_PTR(seg,off) ((void*)(((unsigned long)(seg) << 4) + (off)))
#define OLD_CL_MAGIC_ADDR ((unsigned short*) MK_PTR(INITSEG,0x20))
#define OLD_CL_MAGIC 0xA33F 
#define OLD_CL_OFFSET_ADDR ((unsigned short*) MK_PTR(INITSEG,0x22))

static void parse_command_line(void)
{
	char *cmdline;

	if (cmdline_parsed)
		return;

	if (*OLD_CL_MAGIC_ADDR != OLD_CL_MAGIC)
		return;

	unsigned short offset = *OLD_CL_OFFSET_ADDR;
	cmdline = MK_PTR(INITSEG, offset);

	/* skip leading spaces */
	while (*cmdline == ' ')
		cmdline++;

	while (*cmdline) {
		if (!strncmp(cmdline, "console=", 8)) {
			cmdline += 8;
			serial_console_setup(cmdline);
		}

		/* go to the next parameter */
		while (*cmdline && *cmdline != ' ')
			cmdline++;
		while (*cmdline == ' ')
			cmdline++;
	}

	cmdline_parsed = 1;
}

void test_start(void)
{
	int my_cpu_num, cpu_num, ltest;

	/* First thing, switch to main stack */
	my_cpu_num = smp_my_cpu_num();
	switch_to_main_stack(my_cpu_num);

	/* Only the boot CPU does initialization */
	if (my_cpu_num == 0) {
	    if (bsp_first == 0) {
	        bsp_first=1;
	        /* The boot CPU does initialization */
		parse_command_line();
	    	init();
#ifdef SMP
		initialise_cpus();
#endif
		v->test=0;
		test_setup();
		find_ticks_for_pass();
		dprint(LINE_INFO+2, COL_INF2-2, num_cpus, 2, 0);
		if (num_cpus == 1) {
			cprint(LINE_INFO+1,COL_INF3-6,"Single");
			cpu_mode = CPM_SINGLE;
		} else {
			if (cpu_mode == CPM_RROBIN) {
				cprint(LINE_INFO+1,COL_INF3-6,"RRobin");
			} else  if (cpu_mode == CPM_SEQ) {
				cprint(LINE_INFO+1,COL_INF3-6,"   Seq");
			} else {
				cprint(LINE_INFO+1,COL_INF3-6,"Single");
			}
		}

		/* Setup windows table for relocation & base address */
		windows[0].start = (LOW_TEST_ADR+(_end - _start)+4095) >> 12;
	
		/* Set relocation address to 32Mb if there is enough memory */
		/* Otherwise set it to 3Mb */
		/* A large relocation address allows for more testing overlap */
	        if ((int)v->pmap[v->msegs-1].end > 0x4000) {
			high_test_adr = 0x2000000;
	        } else {
			high_test_adr = 0x300000;
		} 
	        windows[1].end = (high_test_adr >> 12);
		if ((ulong)&_start != LOW_TEST_ADR) {
                        restart_internal();
                }
	    }
	}

#ifdef SMP
	/* AP init only */
	if (ap_first == 0 && my_cpu_num != 0) {
		/* Register the APs */
		smp_ap_booted(my_cpu_num);
		ap_first=1;
	}

        /* The code may have relocated so make the other CPUs spin */
	/* at the new relocated location */
	if (my_cpu_num == cpu_sel) {
                for (cpu_num = 0; cpu_num < num_cpus ; cpu_num++ ) {
                        if (cpu_num != my_cpu_num) {
                            *old_jmp_address[cpu_num] = (ulong *)&startup_32;
                            /* wait till we are sure the AP is waiting at 
                             * the new location 
                             */
                            while(jmp_address[cpu_num] != &&AP_SpinWait);
                        }
                }
	        bail=0;
	} else {
            /* Unselected CPUs spin wait, until told to proceed. */
AP_SpinWait_Start:
            paging_off();
            set_cache(1);
            old_jmp_address[my_cpu_num] = (ulong **) &jmp_address[my_cpu_num];
            jmp_address[my_cpu_num]    = (ulong *) &&AP_SpinWait;
AP_SpinWait:
            __asm__ __volatile__ (
	    "rep;nop\n\t"
            "   jmp *%0"
            :
            :"r"(jmp_address[my_cpu_num])
            );
AP_SpinWait_End:
	    bail=0;
        }
#endif
	/* Loop through all tests */
	while (1) {
	    /* Loop through all valid windows */
	    while (window < sizeof(windows)/sizeof(windows[0])) {
	        /* If we have a partial relocation finish it */
	        if (run_at_addr == (unsigned long)&_start) {
		    run_at_addr = 0xffffffff;
	        } else if (run_at_addr != 0xffffffff) {
		    __run_at(run_at_addr);
	        }

		/* Do we need to exit */
		if(restart_pending) {
		    restart_internal();
	 	}

	        /* Find the memory areas I am going to test */
	        segs = compute_segments(window);
	        if (segs == 0) {
		    window++;
		    continue;
	        }
		/* map in the window... */
		if (map_page(v->map[0].pbase_addr) < 0) {
		    window++;
		    continue;
		}
		
		do_test();
		if (bail) {
		    break;
		}
	        paging_off();
		window++;

		/* Relocate the test if required */
		if (windows[window].start < 
			(LOW_TEST_ADR + (_end - _start)) >> 12) {
			if (v->pmap[v->msegs-1].end > 
				(((high_test_adr+(_end - _start)) >> 12) +1)) {
				/* We need the high copy and we have enough
				 * memory so use it. Relocate if needed.
				 */
				run_at(high_test_adr);
			} else { 
				/* We can't use this window so skip it */
				window++;
				continue;
			}
		} else {
			/* Relocate to lower address if needed */
			if ((ulong)&_start != LOW_TEST_ADR) {
				run_at(LOW_TEST_ADR);
			}
		}
	    } /* End of window loop */
	    window = 0;
	    bail = 0;

	    /* End of test */
	    ltest = v->test;
	    /* Select advancement of CPUs and next test */
	    /* Single CPU, keep the same CPU and advance to the next test */
	    if (cpu_mode == CPM_SINGLE) {
		 v->test++;
	    /* Sequential, use the next CPU, advance test with last CPU */
	    } else if (cpu_mode == CPM_SEQ) {
		if (++cpu_sel >= num_cpus) {
		    cpu_sel = 0;
		    v->test++;
		}
	    /* Round Robin, advance both the CPU and test */
	    } else if (cpu_mode == CPM_RROBIN) {
		if (++cpu_sel >= num_cpus) {
		    cpu_sel = 0;
		}
		v->test++;
	    }

	    /* If this was the last test then we finished a pass */
	    if (v->test >= DEFTESTS ||
			(v->testsel >= 0 && cpu_sel == (num_cpus-1))) {
		v->pass++;
		dprint(LINE_INFO, COL_INF4-8, v->pass, 8, 0);
		v->total_ticks = 0;
		find_ticks_for_pass();
		cprint(0, COL_MID+8,
			"                                         ");
		if (v->ecount == 0 && v->testsel < 0) {
		    cprint(LINE_MSG, COL_MSG,
			"Pass complete, no errors, press Esc to exit");
		}
	    }
	    if (v->test >= DEFTESTS) {
		v->test = 0;
	    }

	    /* Only call setup if we are running a new test */
	    if (ltest != v->test) {
	    	test_setup();
	    }
#ifdef SMP
	    /* Setup for next CPU/test */
	    if (my_cpu_num != cpu_sel) {
		/* Start the next CPU and spin */
	        /* Revert to the default mapping and enable the cache */
	        paging_off();
	        set_cache(1);
		/* Kick the next CPU */
	        jmp_address[cpu_sel] = &&AP_SpinWait_End;
	        goto AP_SpinWait_Start;
	    }
#endif
	    bail=0;
	} /* End test loop */
}

void test_setup()
{
	/* Now setup the test parameters based on the current test number */
	/* Figure out the next test to run */
	if (v->testsel >= 0) {
		v->test = v->testsel;
	}
	if (v->pass == 0) {
		/* Reduce iterations for first pass */
		c_iter = tseq[v->test].iter/3;
	} else {
		c_iter = tseq[v->test].iter;
	}

	/* Set the number of iterations. We only do half of the iterations */
        /* on the first pass */
	dprint(LINE_INFO+1, COL_INF2-5, c_iter, 5, 0);
	test_ticks = find_ticks_for_test(find_chunks(), v->test);
	nticks = 0;
	v->tptr = 0;

	cprint(LINE_PAT, COL_PAT, "            ");
	cprint(LINE_PAT, COL_PAT-3, "   ");
	dprint(LINE_TST, COL_MID+6, v->test, 2, 1);
	cprint(LINE_TST, COL_MID+9, tseq[v->test].msg);
	cprint(1, COL_MID+8, "                                         ");

	set_cache(tseq[v->test].cache);
}

int do_test()
{
	int i=0, j=0;
	unsigned long lo, hi;

	if ((ulong)&_start > LOW_TEST_ADR) {
		/* Relocated so we need to test all selected lower memory */
		v->map[0].start = mapping(v->plim_lower);
		cprint(LINE_PAT, COL_MID+28, " Relocated");
	} else {
		cprint(LINE_PAT, COL_MID+28, "          ");
	}

	/* Update display of memory segments being tested */
	lo = page_of(v->map[0].start);
	hi = page_of(v->map[segs-1].end);
	aprint(LINE_RANGE, COL_MID+9, lo);
	cprint(LINE_RANGE, COL_MID+14, " - ");
	aprint(LINE_RANGE, COL_MID+17, hi);
	aprint(LINE_RANGE, COL_MID+24, v->selected_pages);
	dprint(3, COL_MID+41, smp_my_cpu_num(), 2,1 );
	
	switch(tseq[v->test].pat) {

	/* Now do the testing according to the selected pattern */
	case 0:	/* Moving inversions, all ones and zeros (test #2) */
		p1 = 0;
		p2 = ~p1;
		movinv1(c_iter,p1,p2);
		BAILOUT;
	
		/* Switch patterns */
		p2 = p1;
		p1 = ~p2;
		movinv1(c_iter,p1,p2);
		BAILOUT;
		break;
		
	case 1: /* Moving inversions, 8 bit walking ones and zeros (test #3) */
		p0 = 0x80;
		for (i=0; i<8; i++, p0=p0>>1) {
			p1 = p0 | (p0<<8) | (p0<<16) | (p0<<24);
			p2 = ~p1;
			movinv1(c_iter,p1,p2);
			BAILOUT;
	
			/* Switch patterns */
			p2 = p1;
			p1 = ~p2;
			movinv1(c_iter,p1,p2);
			BAILOUT
		}
		break;

	case 2: /* Moving inversions, 32 bit shifting pattern (test #6) */
		for (i=0, p1=1; p1; p1=p1<<1, i++) {
			movinv32(c_iter,p1, 1, 0x80000000, 0, i);
			BAILOUT
			movinv32(c_iter,~p1, 0xfffffffe,
				0x7fffffff, 1, i);
			BAILOUT
		}
		break;

	case 3: /* Modulo 20 check, all ones and zeros (unused) */
		p1=0;
		for (i=0; i<MOD_SZ; i++) {
			p2 = ~p1;
			modtst(i, c_iter, p1, p2);
			BAILOUT

			/* Switch patterns */
			p2 = p1;
			p1 = ~p2;
			modtst(i, c_iter, p1,p2);
			BAILOUT
		}
		break;

	case 4: /* Modulo 20 check, 8 bit pattern (unused) */
		p0 = 0x80;
		for (j=0; j<8; j++, p0=p0>>1) {
			p1 = p0 | (p0<<8) | (p0<<16) | (p0<<24);
			for (i=0; i<MOD_SZ; i++) {
				p2 = ~p1;
				modtst(i, c_iter, p1, p2);
				BAILOUT

				/* Switch patterns */
				p2 = p1;
				p1 = ~p2;
				modtst(i, c_iter, p1, p2);
				BAILOUT
			}
		}
		break;
	case 5: /* Address test, walking ones (test #0) */
		addr_tst1();
		BAILOUT;
		break;

	case 6: /* Address test, own address (test #1) */
		addr_tst2();
		BAILOUT;
		break;

	case 7: /* Block move (test #5) */
		block_move(c_iter);
		BAILOUT;
		break;
	case 8: /* MWG MOD IDLE TEST */
		if (window == 0 ) {
			mwgIdleTest();
		}
		BAILOUT;
		break;
	case 9: /* Random Data Sequence (test #7) */
		for (i=0; i < c_iter; i++) {
			movinvr();
			BAILOUT;
		}
		break;
	case 10: /* Random Data (test #4) */
		for (i=0; i < c_iter; i++) {
			p1 = rand();
			p2 = ~p1;
			movinv1(2,p1,p2);
			BAILOUT;
		}
		break;

	case 11: /* Modulo 20 check, Random pattern (test #8) */
		for (j=0; j<c_iter; j++) {
			p1 = rand();
			for (i=0; i<MOD_SZ; i++) {
				p2 = ~p1;
				modtst(i, 2, p1, p2);
				BAILOUT

				/* Switch patterns */
				p2 = p1;
				p1 = ~p2;
				modtst(i, 2, p1, p2);
				BAILOUT
			}
		}
		break;

	case 12: /* MWG MOD WRITE ONLY ALT 1/0 TEST.*/
		mwgModWriteOnlyTest(0);
		mwgModWriteOnlyTest(~0);
		BAILOUT;
		break;

	case 13: /* MWG MOD READ ONLY TEST.*/
		mwgModReadOnlyTest();
		BAILOUT;
		break;
	case 14: /* MWG MOD WRITE ONLY 1 */
		mwgModWriteOnlyTest(~0);
		BAILOUT;
		break;
	case 15: /* MWG MOD WRITE ONLY 0 */
		mwgModWriteOnlyTest(0);
		BAILOUT;
		break;
		
	case 16: /* MWG MOD WRITE 1 ONLY NO CACHE*/
		mwgModWriteOnlyTest(~0); 
		BAILOUT;
	}
	
	return(0);
}

/* Compute number of SPINSZ chunks being tested */
static int find_chunks(void) 
{
	int i, j, ch=0, sg;
	/* Compute the number of SPINSZ memory segments */
	ch = 0;
	for(j = 0; j < sizeof(windows)/sizeof(windows[0]); j++) {
		sg = compute_segments(j);
		for(i = 0; i < sg; i++) {
			unsigned long len;
			len = v->map[i].end - v->map[i].start;
			ch += (len + SPINSZ -1)/SPINSZ;
		}
	}
	return (ch);
}

/* Compute the total number of ticks per pass */
void find_ticks_for_pass(void)
{
	int i, ch;

	v->pptr = 0;
	v->pass_ticks=0;
	ch = find_chunks();
	if (v->testsel >= 0) {
		v->pass_ticks = find_ticks_for_test(ch, v->testsel);
	} else {
		for (i=0; i<DEFTESTS; i++) {
			v->pass_ticks += find_ticks_for_test(ch, i);
		}
	}
}

static int find_ticks_for_test(int ch, int test)
{
	int ticks=0, c;

	/* Set the number of iterations. We only do half of the iterations */
        /* on the first pass */
	if (v->pass == 0) {
		c = tseq[test].iter/3;
	} else {
		c = tseq[test].iter;
	}

	switch(tseq[test].pat) {
	case 0: /* Moving inversions, all ones and zeros (test #2) */
		ticks = 2 + 4 * c;
		break;
	case 1: /* Moving inversions, 8 bit walking ones and zeros (test #3) */
		ticks = 24 + 24 * c;
		break;
	case 2: /* Moving inversions, 32 bit shifting pattern, very long */
		ticks = (1 + c * 2) * 64;
		break;
	case 3: /* Modulo 20 check, all ones and zeros (unused) */
		ticks = (2 + c) * 40;
		break;
	case 4: /* Modulo 20 check, 8 bit pattern (unused) */
		ticks = (2 + c) * 40 * 8;
		break;
	case 5: /* Address test, walking ones (test #0) */
		ticks = 4;
		break;
	case 6: /* Address test, own address (test #1) */
		ticks = 2;
		break;
	case 7: /* Block move (test #5) */
		ticks = 2 + c;
		break;
	case 8: /* Bit fade test (test #9) */
		ticks = 1;
		break;
	case 9: /* Random Data Sequence (test #7) */
		ticks = 3 * c;
		break;
	case 10: /* Random Data (test #4) */
		ticks = c + 4 * c;
		break;
	case 11: /* Modulo 20 check, Random pattern (test #8) */
		ticks = 4 * 40 * c;
		break;
	}
	if (cpu_mode == CPM_SEQ) {
		ticks *= num_cpus;
	}
	return ticks*ch;
}

static int compute_segments(int win)
{
	unsigned long wstart, wend;
	int i, sg;

	/* Compute the window I am testing memory in */
	wstart = windows[win].start;
	wend = windows[win].end;
	sg = 0;

	/* Now reduce my window to the area of memory I want to test */
	if (wstart < v->plim_lower) {
		wstart = v->plim_lower;
	}
	if (wend > v->plim_upper) {
		wend = v->plim_upper;
	}
	if (wstart >= wend) {
		return(0);
	}
	/* List the segments being tested */
	for (i=0; i< v->msegs; i++) {
		unsigned long start, end;
		start = v->pmap[i].start;
		end = v->pmap[i].end;
		if (start <= wstart) {
			start = wstart;
		}
		if (end >= wend) {
			end = wend;
		}
#if 0
		cprint(LINE_SCROLL+(2*i), 0, " (");
		hprint(LINE_SCROLL+(2*i), 2, start);
		cprint(LINE_SCROLL+(2*i), 10, ", ");
		hprint(LINE_SCROLL+(2*i), 12, end);
		cprint(LINE_SCROLL+(2*i), 20, ") ");

		cprint(LINE_SCROLL+(2*i), 22, "r(");
		hprint(LINE_SCROLL+(2*i), 24, wstart);
		cprint(LINE_SCROLL+(2*i), 32, ", ");
		hprint(LINE_SCROLL+(2*i), 34, wend);
		cprint(LINE_SCROLL+(2*i), 42, ") ");

		cprint(LINE_SCROLL+(2*i), 44, "p(");
		hprint(LINE_SCROLL+(2*i), 46, v->plim_lower);
		cprint(LINE_SCROLL+(2*i), 54, ", ");
		hprint(LINE_SCROLL+(2*i), 56, v->plim_upper);
		cprint(LINE_SCROLL+(2*i), 64, ") ");

		cprint(LINE_SCROLL+(2*i+1),  0, "w(");
		hprint(LINE_SCROLL+(2*i+1),  2, windows[win].start);
		cprint(LINE_SCROLL+(2*i+1), 10, ", ");
		hprint(LINE_SCROLL+(2*i+1), 12, windows[win].end);
		cprint(LINE_SCROLL+(2*i+1), 20, ") ");

		cprint(LINE_SCROLL+(2*i+1), 22, "m(");
		hprint(LINE_SCROLL+(2*i+1), 24, v->pmap[i].start);
		cprint(LINE_SCROLL+(2*i+1), 32, ", ");
		hprint(LINE_SCROLL+(2*i+1), 34, v->pmap[i].end);
		cprint(LINE_SCROLL+(2*i+1), 42, ") ");

		cprint(LINE_SCROLL+(2*i+1), 44, "i=");
		hprint(LINE_SCROLL+(2*i+1), 46, i);
		
		cprint(LINE_SCROLL+(2*i+2), 0, 
			"                                        "
			"                                        ");
		cprint(LINE_SCROLL+(2*i+3), 0, 
			"                                        "
			"                                        ");
#endif
		if ((start < end) && (start < wend) && (end > wstart)) {
			v->map[sg].pbase_addr = start;
			v->map[sg].start = mapping(start);
			v->map[sg].end = emapping(end);
#if 0
			cprint(LINE_SCROLL+(2*i+1), 54, " sg: ");
			hprint(LINE_SCROLL+(2*i+1), 61, sg);
#endif
			sg++;
		}
	}
	return (sg);
}
