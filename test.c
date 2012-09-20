/* test.c - MemTest-86  Version 3.4
 *
 * Released under version 2 of the Gnu Public License.
 * By Chris Brady
 * 
 * modifications by Mark Gottscho August 2011 <mgottscho@ucla.edu>
 */
#include "test.h"
#include "config.h"
#include "smp.h"

extern volatile int segs, bail;
extern int test_ticks, nticks;
extern struct tseq tseq[];
extern void update_err_counts(void);
extern void print_err_counts(void);
void poll_errors();

int ecount = 0;

static inline ulong roundup(ulong value, ulong mask)
{
	return (value + mask) & ~mask;
}

/*
 * MARK GOTTSCHO AUGUST 2011.
 * Writes patterns repeatedly to facilitate DIMM power measurements. Can be used to write 0s, 1s, or addresses.
 * Does not check for memory correctness -- hence no reads.
 */
void mwgModWriteOnlyTest(ulong pat)
{	
	int j, done;
	ulong *p, *pe, *end, *start;
	int useAddr = 0;
	if (pat == 0)
        	cprint(LINE_PAT, COL_PAT+5, "zeros  ");
    	else if (pat == ~0)
        	cprint(LINE_PAT, COL_PAT+5, "ones   ");
    	else {
    		cprint(LINE_PAT, COL_PAT+5, "address"); 
    		useAddr = 1;
	}

	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		pe = (ulong *)start;
		p = start;
		cprint(LINE_PAT, COL_PAT, "OK  ");
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}

			//ALTERNATING 0/1 CODE
			/*for (; p <= pe; p++) {
				*p = pat;
			}*/
			
			//Use assembly for speed.	
			if (useAddr) { //Address mode
				asm __volatile__ (
					"jmp MWG91\n\t"
					".p2align 4,,7\n\t"
					"MWG90:\n\t"
					"addl $4,%%edi\n\t"
					"MWG91:\n\t"
					"movl %%edi,(%%edi)\n\t" //Write the addr to addr
					"cmpl %%edx,%%edi\n\t"
					"jb MWG90\n\t"
					: : "D" (p), "d" (pe)
				);
			}
			else { //Pattern mode
				asm __volatile__ (
					"jmp MWG31\n\t"
					".p2align 4,,7\n\t"
					"MWG30:\n\t"
					"addl $4,%%edi\n\t"
					"MWG31:\n\t"
					"movl %2,(%%edi)\n\t" //Write pat to addr
					"cmpl %%edx,%%edi\n\t"
					"jb MWG30\n\t"
					: : "D" (p), "d" (pe) , "r" (pat)
				);
			}
			
			p = pe + 1;
		} while (!done);
	}
}


/*
 * MARK GOTTSCHO AUGUST 2011.
 * Reads repeatedly to facilitate DIMM power measurements.
 * Does not initialize the memory! It will read junk if you don't write something before this test.
 */
void mwgModReadOnlyTest()
{
	int j, done;
	ulong *p, *pe, *end, *start;

	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		pe = (ulong *)start;
		p = start;
		
		cprint(LINE_PAT, COL_PAT, "OK  ");
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
                                pe += SPINSZ;
                        } else {
                                pe = end;
                        }
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
			
			//asm for speed
 			asm __volatile__ (
				"jmp MWG95\n\t"
				".p2align 4,,7\n\t"
				"MWG99:\n\t"
				"addl $4,%%edi\n\t"
				"MWG95:\n\t"
				"movl (%%edi),%%ecx\n\t" //replaced %%ecx with %2
				"cmpl %%edx,%%edi\n\t"
				"jb MWG99\n\t"
				: : "D" (p), "d" (pe)
				: "ecx"
			);
 			
			p = pe + 1;
		} while (!done);
	}
}

/*
 * Memory address test, walking ones
 */
void addr_tst1()
{
	int i, j, k;
	volatile ulong *p, *pt, *end;
	ulong bad, mask, p1, bank;

	/* Test the global address bits */
	for (p1=0, j=0; j<2; j++) {
        	hprint(LINE_PAT, COL_PAT, p1);

		/* Set pattern in our lowest multiple of 0x20000 */
		p = (ulong *)roundup((ulong)v->map[0].start, 0x1ffff);
		*p = p1;
	
		/* Now write pattern compliment */
		p1 = ~p1;
		end = v->map[segs-1].end;
		for (i=0; i<100; i++) {
			mask = 4;
			do {
				pt = (ulong *)((ulong)p | mask);
				if (pt == p) {
					mask = mask << 1;
					continue;
				}
				if (pt >= end) {
					break;
				}
				*pt = p1;
				if ((bad = *p) != ~p1) {
					ad_err1((ulong *)p, (ulong *)mask,
						bad, ~p1);
					i = 1000;
				}
				mask = mask << 1;
			} while(mask);
		}
		do_tick();
		BAILR
	}

	/* Now check the address bits in each bank */
	/* If we have more than 8mb of memory then the bank size must be */
	/* bigger than 256k.  If so use 1mb for the bank size. */
	if (v->pmap[v->msegs - 1].end > (0x800000 >> 12)) {
		bank = 0x100000;
	} else {
		bank = 0x40000;
	}
	for (p1=0, k=0; k<2; k++) {
        	hprint(LINE_PAT, COL_PAT, p1);

		for (j=0; j<segs; j++) {
			p = v->map[j].start;
			/* Force start address to be a multiple of 256k */
			p = (ulong *)roundup((ulong)p, bank - 1);
			end = v->map[j].end;
			/* Redundant checks for overflow */
                        while (p < end && p > v->map[j].start && p != 0) {
				*p = p1;

				p1 = ~p1;
				for (i=0; i<50; i++) {
					mask = 4;
					do {
						pt = (ulong *)
						    ((ulong)p | mask);
						if (pt == p) {
							mask = mask << 1;
							continue;
						}
						if (pt >= end) {
							break;
						}
						*pt = p1;
						if ((bad = *p) != ~p1) {
							ad_err1((ulong *)p,
							    (ulong *)mask,
							    bad,~p1);
							i = 200;
						}
						mask = mask << 1;
					} while(mask);
				}
				if (p + bank > p) {
					p += bank;
				} else {
					p = end;
				}
				p1 = ~p1;
			}
		}
		do_tick();
		BAILR
		p1 = ~p1;
	}
}

/*
 * Memory address test, own address
 */
void addr_tst2()
{
	int j, done;
	ulong *p, *pe, *end, *start;

        cprint(LINE_PAT, COL_PAT, "address ");

	/* Write each address with it's own address */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		pe = (ulong *)start;
		p = start;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}

/* Original C code replaced with hand tuned assembly code
 *			for (; p <= pe; p++) {
 *				*p = (ulong)p;
 *			}
 */
			asm __volatile__ (
				"jmp L91\n\t"
				".p2align 4,,7\n\t"
				"L90:\n\t"
				"addl $4,%%edi\n\t"
				"L91:\n\t"
				"movl %%edi,(%%edi)\n\t"
				"cmpl %%edx,%%edi\n\t"
				"jb L90\n\t"
				: : "D" (p), "d" (pe)
			);
			p = pe + 1;
		} while (!done);
	}

	/* Each address should have its own address */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		pe = (ulong *)start;
		p = start;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
                                pe += SPINSZ;
                        } else {
                                pe = end;
                        }
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
/* Original C code replaced with hand tuned assembly code
 *			for (; p <= pe; p++) {
 *				if((bad = *p) != (ulong)p) {
 *					ad_err2((ulong)p, bad);
 *				}
 *			}
 */
			asm __volatile__ (
				"jmp L95\n\t"
				".p2align 4,,7\n\t"
				"L99:\n\t"
				"addl $4,%%edi\n\t"
				"L95:\n\t"
				"movl (%%edi),%%ecx\n\t"
				"cmpl %%edi,%%ecx\n\t"
				"jne L97\n\t"
				"L96:\n\t"
				"cmpl %%edx,%%edi\n\t"
				"jb L99\n\t"
				"jmp L98\n\t"
			
				"L97:\n\t"
				"pushl %%edx\n\t"
				"pushl %%ecx\n\t"
				"pushl %%edi\n\t"
				"call ad_err2\n\t"
				"popl %%edi\n\t"
				"popl %%ecx\n\t"
				"popl %%edx\n\t"
				"jmp L96\n\t"

				"L98:\n\t"
				: : "D" (p), "d" (pe)
				: "ecx"
			);
			p = pe + 1;
		} while (!done);
	}
}

/*
 * Test all of memory using a "half moving inversions" algorithm using random
 * numbers and their complment as the data pattern. Since we are not able to
 * produce random numbers in reverse order testing is only done in the forward
 * direction.
 */
void movinvr()
{
	int i, j, done, seed1, seed2;
	ulong *p;
	ulong *pe;
	ulong *start,*end;
	ulong num;

	/* Initialize memory with initial sequence of random numbers.  */
	if (v->rdtsc) {
		asm __volatile__ ("rdtsc":"=a" (seed1),"=d" (seed2));
	} else {
		seed1 = 521288629 + v->pass;
		seed2 = 362436069 - v->pass;
	}

	/* Display the current seed */
        hprint(LINE_PAT, COL_PAT, seed1);
	rand_seed(seed1, seed2);
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		pe = start;
		p = start;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
/* Original C code replaced with hand tuned assembly code */
/*
			for (; p <= pe; p++) {
				*p = rand();
			}
 */

                        asm __volatile__ (
                                "jmp L200\n\t"
                                ".p2align 4,,7\n\t"
                                "L201:\n\t"
                                "addl $4,%%edi\n\t"
                                "L200:\n\t"
                                "call rand\n\t"
				"movl %%eax,(%%edi)\n\t"
                                "cmpl %%ebx,%%edi\n\t"
                                "jb L201\n\t"
                                : : "D" (p), "b" (pe)
				: "eax"
                        );
			p = pe + 1;
		} while (!done);
	}

	/* Do moving inversions test. Check for initial pattern and then
	 * write the complement for each memory location.
	 */
	for (i=0; i<2; i++) {
		rand_seed(seed1, seed2);
		for (j=0; j<segs; j++) {
			start = v->map[j].start;
			end = v->map[j].end;
			pe = start;
			p = start;
			done = 0;
			do {
				do_tick();
				BAILR

				/* Check for overflow */
				if (pe + SPINSZ > pe && pe != 0) {
					pe += SPINSZ;
				} else {
					pe = end;
				}
				if (pe >= end) {
					pe = end;
					done++;
				}
				if (p == pe ) {
					break;
				}
/* Original C code replaced with hand tuned assembly code */
/*
				for (; p <= pe; p++) {
					num = rand();
					if (i) {
						num = ~num;
					}
					if ((bad=*p) != num) {
						error((ulong*)p, num, bad);
					}
					*p = ~num;
				}
*/
				if (i) {
					num = 0xffffffff;
				} else {
					num = 0;
				}
				asm __volatile__ (
                                        "pushl %%ebp\n\t"
					"jmp L26\n\t" \
					".p2align 4,,7\n\t" \
					"L27:\n\t" \
					"addl $4,%%edi\n\t" \
					"L26:\n\t" \
					"call rand\n\t"
					"xorl %%ebx,%%eax\n\t" \
					"movl (%%edi),%%ecx\n\t" \
					"cmpl %%eax,%%ecx\n\t" \
					"jne L23\n\t" \
					"L25:\n\t" \
					"movl $0xffffffff,%%ebp\n\t" \
					"xorl %%ebp,%%eax\n\t" \
					"movl %%eax,(%%edi)\n\t" \
					"cmpl %%esi,%%edi\n\t" \
					"jb L27\n\t" \
					"jmp L24\n" \

					"L23:\n\t" \
					"pushl %%esi\n\t" \
					"pushl %%ecx\n\t" \
					"pushl %%eax\n\t" \
					"pushl %%edi\n\t" \
					"call error\n\t" \
					"popl %%edi\n\t" \
					"popl %%eax\n\t" \
					"popl %%ecx\n\t" \
					"popl %%esi\n\t" \
					"jmp L25\n" \

					"L24:\n\t" \
                                        "popl %%ebp\n\t"
					:: "D" (p), "S" (pe), "b" (num)
					: "eax", "ecx"
				);
				p = pe + 1;
			} while (!done);
		}
	}
}

/*
 * Test all of memory using a "moving inversions" algorithm using the
 * pattern in p1 and it's complement in p2.
 */
void movinv1(int iter, ulong p1, ulong p2)
{
	int i, j, done;
	ulong *p, *pe, len, *start, *end;

	/* Display the current pattern */
        hprint(LINE_PAT, COL_PAT, p1);

	/* Initialize memory with the initial pattern.  */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;

		pe = start;
		p = start;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			len = pe - p + 1;
			if (p == pe ) {
				break;
			}
/* Original C code replaced with hand tuned assembly code
 *			for (; p <= pe; p++) {
 *				*p = p1;
 *			}
 */
			asm __volatile__ (
				"rep\n\t" \
				"stosl\n\t"
				: : "c" (len), "D" (p), "a" (p1)
			);
			p = pe + 1;
		} while (!done);
	}

	/* Do moving inversions test. Check for initial pattern and then
	 * write the complement for each memory location. Test from bottom
	 * up and then from the top down.  */
	for (i=0; i<iter; i++) {
		for (j=0; j<segs; j++) {
		    	start = v->map[j].start;
		        end = v->map[j].end;
			pe = start;
			p = start;
			done = 0;
			do {
				do_tick();
				BAILR

				/* Check for overflow */
				if (pe + SPINSZ > pe && pe != 0) {
					pe += SPINSZ;
				} else {
					pe = end;
				}
				if (pe >= end) {
					pe = end;
					done++;
				}
				if (p == pe ) {
					break;
				}
/* Original C code replaced with hand tuned assembly code
 *				for (; p <= pe; p++) {
 *					if ((bad=*p) != p1) {
 *						error((ulong*)p, p1, bad);
 *					}
 *					*p = p2;
 *				}
 */
				asm __volatile__ (
					"jmp L2\n\t" \
					".p2align 4,,7\n\t" \
					"L0:\n\t" \
					"addl $4,%%edi\n\t" \
					"L2:\n\t" \
					"movl (%%edi),%%ecx\n\t" \
					"cmpl %%eax,%%ecx\n\t" \
					"jne L3\n\t" \
					"L5:\n\t" \
					"movl %%ebx,(%%edi)\n\t" \
					"cmpl %%edx,%%edi\n\t" \
					"jb L0\n\t" \
					"jmp L4\n" \

					"L3:\n\t" \
					"pushl %%edx\n\t" \
					"pushl %%ebx\n\t" \
					"pushl %%ecx\n\t" \
					"pushl %%eax\n\t" \
					"pushl %%edi\n\t" \
					"call error\n\t" \
					"popl %%edi\n\t" \
					"popl %%eax\n\t" \
					"popl %%ecx\n\t" \
					"popl %%ebx\n\t" \
					"popl %%edx\n\t" \
					"jmp L5\n" \

					"L4:\n\t" \
					:: "a" (p1), "D" (p), "d" (pe), "b" (p2)
					: "ecx"
				);
				p = pe + 1;
			} while (!done);
		}
		for (j=segs-1; j>=0; j--) {
		    	start = v->map[j].start;
		        end = v->map[j].end;
			pe = end;
			p = end;
			done = 0;
			do {
				do_tick();
				BAILR

				/* Check for underflow */
				if (pe - SPINSZ < pe && pe != 0) {
					pe -= SPINSZ;
				} else {
					pe = start;
					done++;
				}

				/* Since we are using unsigned addresses a 
				 * redundent check is required */
				if (pe < start || pe > end) {
					pe = start;
					done++;
				}
				if (p == pe ) {
					break;
				}
/* Original C code replaced with hand tuned assembly code
 *				do {
 *					if ((bad=*p) != p2) {
 *						error((ulong*)p, p2, bad);
 *					}
 *					*p = p1;
 *				} while (p-- >= pe);
 */
				asm __volatile__ (
					"jmp L9\n\t"
					".p2align 4,,7\n\t"
					"L11:\n\t"
					"subl $4, %%edi\n\t"
					"L9:\n\t"
					"movl (%%edi),%%ecx\n\t"
					"cmpl %%ebx,%%ecx\n\t"
					"jne L6\n\t"
					"L10:\n\t"
					"movl %%eax,(%%edi)\n\t"
					"cmpl %%edi, %%edx\n\t"
					"jne L11\n\t"
					"jmp L7\n\t"

					"L6:\n\t"
					"pushl %%edx\n\t"
					"pushl %%eax\n\t"
					"pushl %%ecx\n\t"
					"pushl %%ebx\n\t"
					"pushl %%edi\n\t"
					"call error\n\t"
					"popl %%edi\n\t"
					"popl %%ebx\n\t"
					"popl %%ecx\n\t"
					"popl %%eax\n\t"
					"popl %%edx\n\t"
					"jmp L10\n"

					"L7:\n\t"
					:: "a" (p1), "D" (p), "d" (pe), "b" (p2)
					: "ecx"
				);
				p = pe - 1;
			} while (!done);
		}
	}
}

void movinv32(int iter, ulong p1, ulong lb, ulong hb, int sval, int off)
{
	int i, j, k=0, n=0, done;
	ulong *p, *pe, *start, *end, pat = 0, p3;

	p3 = sval << 31;
	/* Display the current pattern */
	hprint(LINE_PAT, COL_PAT, p1);

	/* Initialize memory with the initial pattern.  */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		pe = start;
		p = start;
		done = 0;
		k = off;
		pat = p1;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
			/* Do a SPINSZ section of memory */
/* Original C code replaced with hand tuned assembly code
 *			while (p <= pe) {
 *				*p = pat;
 *				if (++k >= 32) {
 *					pat = lb;
 *					k = 0;
 *				} else {
 *					pat = pat << 1;
 *					pat |= sval;
 *				}
 *				p++;
 *			}
 */
			asm __volatile__ (
                                "jmp L20\n\t"
                                ".p2align 4,,7\n\t"
                                "L923:\n\t"
                                "addl $4,%%edi\n\t"
                                "L20:\n\t"
                                "movl %%ecx,(%%edi)\n\t"
                                "addl $1,%%ebx\n\t"
                                "cmpl $32,%%ebx\n\t"
                                "jne L21\n\t"
                                "movl %%esi,%%ecx\n\t"
                                "xorl %%ebx,%%ebx\n\t"
                                "jmp L22\n"
                                "L21:\n\t"
                                "shll $1,%%ecx\n\t"
                                "orl %%eax,%%ecx\n\t"
                                "L22:\n\t"
                                "cmpl %%edx,%%edi\n\t"
                                "jb L923\n\t"
                                : "=b" (k), "=c" (pat)
                                : "D" (p),"d" (pe),"b" (k),"c" (pat),
                                        "a" (sval), "S" (lb)
			);
			p = pe + 1;
		} while (!done);
	}

	/* Do moving inversions test. Check for initial pattern and then
	 * write the complement for each memory location. Test from bottom
	 * up and then from the top down.  */
	for (i=0; i<iter; i++) {
		for (j=0; j<segs; j++) {
			start = v->map[j].start;
			end = v->map[j].end;
			pe = start;
			p = start;
			done = 0;
			k = off;
			pat = p1;
			do {
				do_tick();
				BAILR

				/* Check for overflow */
				if (pe + SPINSZ > pe && pe != 0) {
					pe += SPINSZ;
				} else {
					pe = end;
				}
				if (pe >= end) {
					pe = end;
					done++;
				}
				if (p == pe ) {
					break;
				}
/* Original C code replaced with hand tuned assembly code
 *				while (1) {
 *					if ((bad=*p) != pat) {
 *						error((ulong*)p, pat, bad);
 *					}
 *					*p = ~pat;
 *					if (p >= pe) break;
 *					p++;
 *
 *					if (++k >= 32) {
 *						pat = lb;
 *						k = 0;
 *					} else {
 *						pat = pat << 1;
 *						pat |= sval;
 *					}
 *				}
 */
				asm __volatile__ (
                                        "pushl %%ebp\n\t"
                                        "jmp L30\n\t"
                                        ".p2align 4,,7\n\t"
                                        "L930:\n\t"
                                        "addl $4,%%edi\n\t"
                                        "L30:\n\t"
                                        "movl (%%edi),%%ebp\n\t"
                                        "cmpl %%ecx,%%ebp\n\t"
                                        "jne L34\n\t"

                                        "L35:\n\t"
                                        "notl %%ecx\n\t"
                                        "movl %%ecx,(%%edi)\n\t"
                                        "notl %%ecx\n\t"
                                        "incl %%ebx\n\t"
                                        "cmpl $32,%%ebx\n\t"
                                        "jne L31\n\t"
                                        "movl %%esi,%%ecx\n\t"
                                        "xorl %%ebx,%%ebx\n\t"
                                        "jmp L32\n"
                                        "L31:\n\t"
                                        "shll $1,%%ecx\n\t"
                                        "orl %%eax,%%ecx\n\t"
					"L32:\n\t"
                                        "cmpl %%edx,%%edi\n\t"
                                        "jb L930\n\t"
                                        "jmp L33\n\t"

                                        "L34:\n\t" \
                                        "pushl %%esi\n\t"
                                        "pushl %%eax\n\t"
                                        "pushl %%ebx\n\t"
                                        "pushl %%edx\n\t"
                                        "pushl %%ebp\n\t"
                                        "pushl %%ecx\n\t"
                                        "pushl %%edi\n\t"
                                        "call error\n\t"
                                        "popl %%edi\n\t"
                                        "popl %%ecx\n\t"
                                        "popl %%ebp\n\t"
                                        "popl %%edx\n\t"
                                        "popl %%ebx\n\t"
                                        "popl %%eax\n\t"
                                        "popl %%esi\n\t"
                                        "jmp L35\n"

                                        "L33:\n\t"
                                        "popl %%ebp\n\t"
                                        : "=b" (k),"=c" (pat)
                                        : "D" (p),"d" (pe),"b" (k),"c" (pat),
                                                "a" (sval), "S" (lb)
				);
				p = pe + 1;
			} while (!done);
		}

                if (--k < 0) {
                        k = 31;
                }
                for (pat = lb, n = 0; n < k; n++) {
                        pat = pat << 1;
                        pat |= sval;
                }
		k++;

		for (j=segs-1; j>=0; j--) {
			start = v->map[j].start;
			end = v->map[j].end;
			p = end;
			pe = end;
			done = 0;
			do {
				do_tick();
				BAILR

				/* Check for underflow */
                                if (pe - SPINSZ < pe && pe != 0) {
                                        pe -= SPINSZ;
                                } else {
                                        pe = start;
					done++;
                                }
				/* We need this redundant check because we are
				 * using unsigned longs for the address.
				 */
				if (pe < start || pe > end) {
					pe = start;
					done++;
				}
				if (p == pe ) {
					break;
				}
/* Original C code replaced with hand tuned assembly code
 *				while(1) {
 *					if ((bad=*p) != ~pat) {
 *						error((ulong*)p, ~pat, bad);
 *					}
 *					*p = pat;
					if (p >= pe) break;
					p++;
 *					if (--k <= 0) {
 *						pat = hb;
 *						k = 32;
 *					} else {
 *						pat = pat >> 1;
 *						pat |= p3;
 *					}
 *				};
 */
				asm __volatile__ (
                                        "pushl %%ebp\n\t"
                                        "jmp L40\n\t"
                                        ".p2align 4,,7\n\t"
                                        "L49:\n\t"
                                        "subl $4,%%edi\n\t"
                                        "L40:\n\t"
                                        "movl (%%edi),%%ebp\n\t"
                                        "notl %%ecx\n\t"
                                        "cmpl %%ecx,%%ebp\n\t"
                                        "jne L44\n\t"

                                        "L45:\n\t"
                                        "notl %%ecx\n\t"
                                        "movl %%ecx,(%%edi)\n\t"
                                        "decl %%ebx\n\t"
                                        "cmpl $0,%%ebx\n\t"
                                        "jg L41\n\t"
                                        "movl %%esi,%%ecx\n\t"
                                        "movl $32,%%ebx\n\t"
                                        "jmp L42\n"
                                        "L41:\n\t"
                                        "shrl $1,%%ecx\n\t"
                                        "orl %%eax,%%ecx\n\t"
					"L42:\n\t"
                                        "cmpl %%edx,%%edi\n\t"
                                        "ja L49\n\t"
                                        "jmp L43\n\t"

                                        "L44:\n\t" \
                                        "pushl %%esi\n\t"
                                        "pushl %%eax\n\t"
                                        "pushl %%ebx\n\t"
                                        "pushl %%edx\n\t"
                                        "pushl %%ebp\n\t"
                                        "pushl %%ecx\n\t"
                                        "pushl %%edi\n\t"
                                        "call error\n\t"
                                        "popl %%edi\n\t"
                                        "popl %%ecx\n\t"
                                        "popl %%ebp\n\t"
                                        "popl %%edx\n\t"
                                        "popl %%ebx\n\t"
                                        "popl %%eax\n\t"
                                        "popl %%esi\n\t"
                                        "jmp L45\n"

                                        "L43:\n\t"
                                        "popl %%ebp\n\t"
                                        : "=b" (k), "=c" (pat)
                                        : "D" (p),"d" (pe),"b" (k),"c" (pat),
                                                "a" (p3), "S" (hb)
				);
				p = pe - 1;
			} while (!done);
		}
	}
}

/*
 * Test all of memory using modulo X access pattern.
 */
void modtst(int offset, int iter, ulong p1, ulong p2)
{
	int j, k, l, done;
	ulong *p;
	ulong *pe;
	ulong *start, *end;

	/* Display the current pattern */
	hprint(LINE_PAT, COL_PAT-2, p1);
	cprint(LINE_PAT, COL_PAT+6, "-");
       	dprint(LINE_PAT, COL_PAT+7, offset, 2, 1);

	/* Write every nth location with pattern */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		end -= MOD_SZ;	/* adjust the ending address */
		pe = (ulong *)start;
		p = start+offset;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
/* Original C code replaced with hand tuned assembly code
 *			for (; p <= pe; p += MOD_SZ) {
 *				*p = p1;
 *			}
 */
			asm __volatile__ (
				"jmp L60\n\t" \
				".p2align 4,,7\n\t" \

				"L60:\n\t" \
				"movl %%eax,(%%edi)\n\t" \
				"addl $80,%%edi\n\t" \
				"cmpl %%edx,%%edi\n\t" \
				"jb L60\n\t" \
				: "=D" (p)
				: "D" (p), "d" (pe), "a" (p1)
			);
		} while (!done);
	}

	/* Write the rest of memory "iter" times with the pattern complement */
	for (l=0; l<iter; l++) {
		for (j=0; j<segs; j++) {
			start = v->map[j].start;
			end = v->map[j].end;
			pe = (ulong *)start;
			p = start;
			done = 0;
			k = 0;
			do {
				do_tick();
				BAILR

				/* Check for overflow */
				if (pe + SPINSZ > pe && pe != 0) {
					pe += SPINSZ;
				} else {
					pe = end;
				}
				if (pe >= end) {
					pe = end;
					done++;
				}
				if (p == pe ) {
					break;
				}
/* Original C code replaced with hand tuned assembly code
 *				for (; p <= pe; p++) {
 *					if (k != offset) {
 *						*p = p2;
 *					}
 *					if (++k > MOD_SZ-1) {
 *						k = 0;
 *					}
 *				}
 */
				asm __volatile__ (
					"jmp L50\n\t" \
					".p2align 4,,7\n\t" \

					"L54:\n\t" \
					"addl $4,%%edi\n\t" \
					"L50:\n\t" \
					"cmpl %%ebx,%%ecx\n\t" \
					"je L52\n\t" \
					  "movl %%eax,(%%edi)\n\t" \
					"L52:\n\t" \
					"incl %%ebx\n\t" \
					"cmpl $19,%%ebx\n\t" \
					"jle L53\n\t" \
					  "xorl %%ebx,%%ebx\n\t" \
					"L53:\n\t" \
					"cmpl %%edx,%%edi\n\t" \
					"jb L54\n\t" \
					: "=b" (k)
					: "D" (p), "d" (pe), "a" (p2),
						"b" (k), "c" (offset)
				);
				p = pe + 1;
			} while (!done);
		}
	}

	/* Now check every nth location */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
		end -= MOD_SZ;	/* adjust the ending address */
		pe = (ulong *)start;
		p = start+offset;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
/* Original C code replaced with hand tuned assembly code
 *			for (; p <= pe; p += MOD_SZ) {
 *				if ((bad=*p) != p1) {
 *					error((ulong*)p, p1, bad);
 *				}
 *			}
 */
			asm __volatile__ (
				"jmp L70\n\t" \
				".p2align 4,,7\n\t" \

				"L70:\n\t" \
				"movl (%%edi),%%ecx\n\t" \
				"cmpl %%eax,%%ecx\n\t" \
				"jne L71\n\t" \
				"L72:\n\t" \
				"addl $80,%%edi\n\t" \
				"cmpl %%edx,%%edi\n\t" \
				"jb L70\n\t" \
				"jmp L73\n\t" \

				"L71:\n\t" \
				"pushl %%edx\n\t"
				"pushl %%ecx\n\t"
				"pushl %%eax\n\t"
				"pushl %%edi\n\t"
				"call error\n\t"
				"popl %%edi\n\t"
				"popl %%eax\n\t"
				"popl %%ecx\n\t"
				"popl %%edx\n\t"
				"jmp L72\n"

				"L73:\n\t" \
				: "=D" (p)
				: "D" (p), "d" (pe), "a" (p1)
				: "ecx"
			);
		} while (!done);
	}
}

/*
 * Test memory using block moves 
 * Adapted from Robert Redelmeier's burnBX test
 */
void block_move(int iter)
{
	int i, j, done;
	ulong len;
	ulong *p, *pe, pp;
	ulong *start, *end;

        cprint(LINE_PAT, COL_PAT-2, "          ");

	/* Initialize memory with the initial pattern.  */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
#ifdef USB_WAR
		/* We can't do the block move test on low memory because
		 * BIOS USB support clobbers location 0x410 and 0x4e0
		 */
		if (start < (ulong *)0x4f0) {
			start = (ulong *)0x4f0;
		}
#endif
		pe = start;
		p = start;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
			len  = ((ulong)pe - (ulong)p) / 64;
			len++;
			asm __volatile__ (
				"jmp L100\n\t"

				".p2align 4,,7\n\t"
				"L100:\n\t"
				"movl %%eax, %%edx\n\t"
				"notl %%edx\n\t"
				"movl %%eax,0(%%edi)\n\t"
				"movl %%eax,4(%%edi)\n\t"
				"movl %%eax,8(%%edi)\n\t"
				"movl %%eax,12(%%edi)\n\t"
				"movl %%edx,16(%%edi)\n\t"
				"movl %%edx,20(%%edi)\n\t"
				"movl %%eax,24(%%edi)\n\t"
				"movl %%eax,28(%%edi)\n\t"
				"movl %%eax,32(%%edi)\n\t"
				"movl %%eax,36(%%edi)\n\t"
				"movl %%edx,40(%%edi)\n\t"
				"movl %%edx,44(%%edi)\n\t"
				"movl %%eax,48(%%edi)\n\t"
				"movl %%eax,52(%%edi)\n\t"
				"movl %%edx,56(%%edi)\n\t"
				"movl %%edx,60(%%edi)\n\t"
				"rcll $1, %%eax\n\t"
				"leal 64(%%edi), %%edi\n\t"
				"decl %%ecx\n\t"
				"jnz  L100\n\t"
				: "=D" (p)
				: "D" (p), "c" (len), "a" (1)
				: "edx"
			);
		} while (!done);
	}

	/* Now move the data around 
	 * First move the data up half of the segment size we are testing
	 * Then move the data to the original location + 32 bytes
	 */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
#ifdef USB_WAR
		/* We can't do the block move test on low memory beacuase
		 * BIOS USB support clobbers location 0x410 and 0x4e0
		 */
		if (start < (ulong *)0x4f0) {
			start = (ulong *)0x4f0;
		}
#endif
		pe = start;
		p = start;
		done = 0;
		do {

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = (ulong *)((ulong)end & 0xfffffff0);
			}
			if (pe >= end) {
				pe = (ulong *)((ulong)end & 0xfffffff0);
				done++;
			}
			if (p == pe ) {
				break;
			}
			pp = (ulong)p + (((ulong)pe - (ulong)p) / 2);
			len  = ((ulong)pe - (ulong)p) / 8;
			for(i=0; i<iter; i++) {
				do_tick();
				BAILR
				asm __volatile__ (
					"cld\n"
					"jmp L110\n\t"

					".p2align 4,,7\n\t"
					"L110:\n\t"
					"movl %1,%%edi\n\t"
					"movl %0,%%esi\n\t"
					"movl %2,%%ecx\n\t"
					"rep\n\t"
					"movsl\n\t"
					"movl %0,%%edi\n\t"
					"addl $32,%%edi\n\t"
					"movl %1,%%esi\n\t"
					"movl %2,%%ecx\n\t"
					"subl $8,%%ecx\n\t"
					"rep\n\t"
					"movsl\n\t"
					"movl %0,%%edi\n\t"
					"movl $8,%%ecx\n\t"
					"rep\n\t"
					"movsl\n\t"
					:: "g" (p), "g" (pp), "g" (len)
					: "edi", "esi", "ecx"
				);
			}
			p = pe;
		} while (!done);
	}

	/* Now check the data 
	 * The error checking is rather crude.  We just check that the
	 * adjacent words are the same.
	 */
	for (j=0; j<segs; j++) {
		start = v->map[j].start;
		end = v->map[j].end;
#ifdef USB_WAR
		/* We can't do the block move test on low memory beacuase
		 * BIOS USB support clobbers location 0x4e0 and 0x410
		 */
		if (start < (ulong *)0x4f0) {
			start = (ulong *)0x4f0;
		}
#endif
		pe = start;
		p = start;
		done = 0;
		do {
			do_tick();
			BAILR

			/* Check for overflow */
			if (pe + SPINSZ > pe && pe != 0) {
				pe += SPINSZ;
			} else {
				pe = end;
			}
			if (pe >= end) {
				pe = end;
				done++;
			}
			if (p == pe ) {
				break;
			}
			pe--;	/* adjust the end since we are testing pe+1 */
			asm __volatile__ (
				"jmp L120\n\t"

				".p2align 4,,7\n\t"
				"L124:\n\t"
				"addl $8,%%edi\n\t"
				"L120:\n\t"
				"movl (%%edi),%%ecx\n\t"
				"cmpl 4(%%edi),%%ecx\n\t"
				"jnz L121\n\t"

				"L122:\n\t"
				"cmpl %%edx,%%edi\n\t"
				"jb L124\n"
				"jmp L123\n\t"

				"L121:\n\t"
				"pushl %%edx\n\t"
				"pushl 4(%%edi)\n\t"
				"pushl %%ecx\n\t"
				"pushl %%edi\n\t"
				"call error\n\t"
				"popl %%edi\n\t"
				"addl $8,%%esp\n\t"
				"popl %%edx\n\t"
				"jmp L122\n"
				"L123:\n\t"
				: "=D" (p)
				: "D" (p), "d" (pe)
				: "ecx"
			);
		} while (!done);
	}
}

/* MARK GOTTSCHO 2011
 * Idles memory for power measurement.
 */
#ifndef STIME
	#define STIME 5400
#endif
void mwgIdleTest()
{
	int j;
	volatile ulong *p, *pe;
	volatile ulong p1, bad;
	volatile ulong *start,*end;

	test_ticks += (STIME * 2);
	v->pass_ticks += (STIME * 2);

	while (1) {

		/* Display the current pattern */
		hprint(LINE_PAT, COL_PAT, p1);

		//Do not initialize memory, all we want is to sleep and measure idle power.

		/* Snooze for 90 minutes */
		sleep (STIME);

		/* Make sure that nothing changed while sleeping */
		for (j=0; j<segs; j++) {
			start = v->map[j].start;
			end = v->map[j].end;
			pe = start;
			p = start;
			for (p=start; p<end; p++) {
 				if ((bad=*p) != p1) {
					error((ulong*)p, p1, bad);
				}
			}
			do_tick();
			BAILR
		}
		if (p1 == 0) {
			p1=-1;
		} else {
			break;
		}
	}
}
/*
 * Test memory for bit fade.
 */
#ifndef STIME
	#define STIME 5400
#endif
void bit_fade()
{
	int j;
	volatile ulong *p, *pe;
	volatile ulong p1, bad;
	volatile ulong *start,*end;

	test_ticks += (STIME * 2);
	v->pass_ticks += (STIME * 2);

	/* Do -1 and 0 patterns */
	p1 = 0;
	while (1) {

		/* Display the current pattern */
		hprint(LINE_PAT, COL_PAT, p1);

		/* Initialize memory with the initial pattern.  */
	
		for (j=0; j<segs; j++) {
			start = v->map[j].start;
			end = v->map[j].end;
			pe = start;
			p = start;
			for (p=start; p<end; p++) {
				*p = p1;
			}
			do_tick();
			BAILR
		}
		/* Snooze for 90 minutes */
		sleep (STIME);

		/* Make sure that nothing changed while sleeping */
		for (j=0; j<segs; j++) {
			start = v->map[j].start;
			end = v->map[j].end;
			pe = start;
			p = start;
			for (p=start; p<end; p++) {
 				if ((bad=*p) != p1) {
					error((ulong*)p, p1, bad);
				}
			}
			do_tick();
			BAILR
		}
		if (p1 == 0) {
			p1=-1;
		} else {
			break;
		}
	}
}

void sleep(int n)
{
	int i, ip=0;
	ulong sh, sl, l, h, t;

	/* save the starting time */
	asm __volatile__(
		"rdtsc":"=a" (sl),"=d" (sh));

	/* loop for n seconds */
	while (1) {
		asm __volatile__(
			"rdtsc":"=a" (l),"=d" (h));
		asm __volatile__ (
			"subl %2,%0\n\t"
			"sbbl %3,%1"
			:"=a" (l), "=d" (h)
			:"g" (sl), "g" (sh),
			"0" (l), "1" (h));
		t = h * ((unsigned)0xffffffff / v->clks_msec) / 1000;
		t += (l / v->clks_msec) / 1000;

		/* Is the time up? */
		if (t >= n) {
			break;
		}

		/* Display the elapsed time on the screen */
		i = t % 60;
		dprint(LINE_INFO, COL_INF1-1, i%10, 1, 0);
		dprint(LINE_INFO, COL_INF1-2, i/10, 1, 0);
		if (i != ip) {
			do_tick();
			BAILR
			ip = i;
		}
		t /= 60;
		i = t % 60;
		dprint(LINE_INFO, COL_INF1-4, i % 10, 1, 0);
		dprint(LINE_INFO, COL_INF1-5, i / 10, 1, 0);
		t /= 60;
		dprint(LINE_INFO, COL_INF1-10, t, 4, 0);
	}
}
