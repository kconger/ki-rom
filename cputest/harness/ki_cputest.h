/* ki_cputest.h — the contract between the KI test ROM and the CPU test suite.
 *
 * The suite is its own image, built for the n32 ABI the tests are written for,
 * while the ROM is o64. The two meet only here, so this header holds nothing
 * whose layout depends on the ABI: 64-bit fields, no pointers, and every
 * address as a plain number.
 *
 * The ROM starts the suite by calling the first word of the embedded image
 * (start.S copies it to its link address and returns when the run is over).
 * Everything the ROM wants back goes through two uncached areas that survive
 * a watchdog reset, so that a run which hangs the CPU still says where:
 *
 *   status  SRAM, just above the ROM's exception record at 0x2F000 and below
 *           VRAM bank 0. The ROM fills in `wdt_addr` before the call; the suite
 *           owns the rest.
 *   log     DRAM, clear of every window the memory benchmarks use. The same
 *           text the upstream suite prints on its serial console.
 *
 * Both sides write these areas only with whole aligned doublewords (sd). On
 * the KI board an uncached store narrower than 64 bits to SRAM or DRAM writes
 * the whole doubleword, and the bytes it did not name read back as 0: a word
 * store to one field wipes its neighbour, a byte store to the log the seven
 * bytes around it. Hence a doubleword for every field, the test's name packed
 * eight characters to a doubleword, and a log written a doubleword at a time.
 */
#ifndef KI_CPUTEST_H
#define KI_CPUTEST_H

#define CPUTEST_STATUS_ADDR  0xA002F100u
#define CPUTEST_LOG_ADDR     0xA8780000u
#define CPUTEST_LOG_SIZE     0x00040000u

#define CPUTEST_MAGIC        0x43505554u   /* 'CPUT' */

/* status.state */
#define CPUTEST_RUNNING      1u
#define CPUTEST_DONE         2u

/* status.rc when the suite refused to run: PRId named no CPU it has answers for. */
#define CPUTEST_RC_UNKNOWN_CPU 127u
/* status.rc after a harness panic; the log says why. */
#define CPUTEST_RC_PANIC     126u

#define CPUTEST_NAME_LEN     48u

typedef unsigned long long cputest_u64;

typedef struct {
    cputest_u64 magic;            /* CPUTEST_MAGIC once the suite has started   */
    cputest_u64 state;
    cputest_u64 rc;               /* failed checks, saturated at 100; or 127    */
    cputest_u64 wdt_addr;         /* in: uncached address whose read kicks the
                                     watchdog                                   */
    cputest_u64 prid;
    cputest_u64 fir;
    cputest_u64 config;
    cputest_u64 cpu_kind;         /* CPU_R4400 / CPU_R5000 / CPU_R4600, or 0    */
    cputest_u64 n_pass;           /* checks */
    cputest_u64 n_fail;
    cputest_u64 n_tests_run;
    cputest_u64 n_tests_failed;
    cputest_u64 n_skip;           /* tests that do not apply to this CPU        */
    cputest_u64 n_tests;          /* tests registered                           */
    cputest_u64 test_index;       /* the test running, while state is RUNNING   */
    cputest_u64 log_len;          /* bytes in the log so far                    */
    /* Its name, NUL-terminated, character i in byte i (little-endian). */
    cputest_u64 current[CPUTEST_NAME_LEN / 8u];
    /* in: so that the suite can name each test on screen as it starts it. The
     * board has no watchdog on every machine that runs this ROM (the MiSTer
     * core has none), and then the screen is the only place a hang shows. */
    cputest_u64 font_addr;        /* the ROM's 7x9 font: 8 bytes a glyph from
                                     ' ', top-left pixel in bit 63             */
    cputest_u64 screen0;          /* both framebuffer banks, 320x240 16-bit;   */
    cputest_u64 screen1;          /* 0 = not drawn                              */
} cputest_status_t;

#endif /* KI_CPUTEST_H */
