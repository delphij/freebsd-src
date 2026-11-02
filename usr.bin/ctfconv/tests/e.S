/*
 * Assembler-only object: built with -g it carries a
 * .debug_info whose only compilation unit is not C, so ctfconv must
 * leave the file untouched and exit 0, exactly as for f.c.
 */
	.text
	.globl	asmfn
	.type	asmfn, @function
asmfn:
	.size	asmfn, .-asmfn

	.data
	.globl	asmvar
	.type	asmvar, @object
asmvar:
	.long	1
	.size	asmvar, 4
