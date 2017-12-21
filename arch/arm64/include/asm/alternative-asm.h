#ifndef __ASM_ALTERNATIVE_ASM_H
#define __ASM_ALTERNATIVE_ASM_H

#ifdef __ASSEMBLY__

.macro altinstruction_entry orig_offset alt_offset feature orig_len alt_len
	.word \orig_offset - .
	.word \alt_offset - .
	.hword \feature
	.byte \orig_len
	.byte \alt_len
.endm

.macro alternative_insn insn1 insn2 cap
661:	\insn1
662:	.pushsection .altinstructions, "a"
	altinstruction_entry 661b, 663f, \cap, 662b-661b, 664f-663f
	.popsection
	.pushsection .altinstr_replacement, "ax"
663:	\insn2
664:	.popsection
	.if ((664b-663b) != (662b-661b))
		.error "Alternatives instruction length mismatch"
	.endif
.endm

/*
 * Begin an alternative code sequence.
 *
 * The code that follows this macro will be assembled and linked as
 * normal. There are no restrictions on this code.
 */
.macro alternative_if_not cap, enable = 1
	.if \enable
	.pushsection .altinstructions, "a"
	altinstruction_entry 661f, 663f, \cap, 662f-661f, 664f-663f
	.popsection
661:
	.endif
.endm

/*
 * Provide the alternative code sequence.
 *
 * The code that follows this macro is assembled into a special
 * section to be used for dynamic patching. Code that follows this
 * macro must:
 *
 * 1. Be exactly the same length (in bytes) as the default code
 *    sequence.
 *
 * 2. Not contain a branch target that is used outside of the
 *    alternative sequence it is defined in (branches into an
 *    alternative sequence are not fixed up).
 */
.macro alternative_else, enable = 1
	.if \enable
662:	.pushsection .altinstr_replacement, "ax"
663:
	.endif
.endm

/*
 * Complete an alternative code sequence.
 */
.macro alternative_endif, enable = 1
	.if \enable
664:	.popsection
	.org	. - (664b-663b) + (662b-661b)
	.org	. - (662b-661b) + (664b-663b)
	.endif
.endm

#endif  /*  __ASSEMBLY__  */

#endif /* __ASM_ALTERNATIVE_ASM_H */
