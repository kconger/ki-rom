/* endian.h — byte order, for the tests whose expectations depend on it.
 *
 * The suite was written for the Indy, which runs big-endian. The same tests
 * also build -EL for a little-endian R4600 (the Killer Instinct board), so
 * every expectation that depends on which byte of a register lands at which
 * address is written for both orders. The big-endian answer is always the
 * upstream one, unchanged.
 *
 * GCC defines __MIPSEB__ or __MIPSEL__ from -EB / -EL.
 */
#ifndef TESTS_ENDIAN_H
#define TESTS_ENDIAN_H

#if defined(__MIPSEB__)
#define ENDIAN_BIG 1
#elif defined(__MIPSEL__)
#define ENDIAN_BIG 0
#else
#error "neither __MIPSEB__ nor __MIPSEL__ is defined"
#endif

/*
 * The address, within an 8-aligned doubleword, of the byte / halfword / word
 * that sits at offset `o` in big-endian order. A test written as "store a u64,
 * load its third byte" names the big-endian offset; on a little-endian part
 * the same bits of the value live at o ^ 7 (byte), o ^ 6 (halfword) or o ^ 4
 * (word). Any 8-aligned base is fine, so offsets past 8 work too. These are
 * integer constant expressions, for an "n" asm operand.
 */
#define DW_B(o)  (ENDIAN_BIG ? (o) : ((o) ^ 7))
#define DW_H(o)  (ENDIAN_BIG ? (o) : ((o) ^ 6))
#define DW_W(o)  (ENDIAN_BIG ? (o) : ((o) ^ 4))

#endif /* TESTS_ENDIAN_H */
