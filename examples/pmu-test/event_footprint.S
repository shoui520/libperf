.syntax unified
.cpu cortex-a9
.thumb
.section .text.instructionFootprint,"ax",%progbits
.balign 16
.global instructionFootprint
.type instructionFootprint,%function
.thumb_func
instructionFootprint:
.rept 32768
	nop
.endr
	bx lr
.size instructionFootprint, .-instructionFootprint

.section .text.coverageLoads,"ax",%progbits
.balign 4
.global coverageLoads
.type coverageLoads,%function
.thumb_func
coverageLoads:
	cmp r1, #0
	beq 2f
1:  ldr r2, [r0]
	subs r1, r1, #1
	bne 1b
	mov r0, r2
	bx lr
2:  movs r0, #0
	bx lr
.size coverageLoads, .-coverageLoads

.section .text.coverageDmbVariants,"ax",%progbits
.balign 4
.global coverageDmbVariants
.type coverageDmbVariants,%function
.thumb_func
coverageDmbVariants:
	cmp r1, #0
	beq 2f
1:  str r1, [r0]
	dmb sy
	dmb ish
	dmb ishst
	dmb st
	subs r1, r1, #1
	bne 1b
2:  bx lr
.size coverageDmbVariants, .-coverageDmbVariants

.fpu neon

.section .text.coverageVfp,"ax",%progbits
.balign 4
.global coverageVfp
.type coverageVfp,%function
.thumb_func
coverageVfp:
	cmp r0, #0
	beq 2f
1:
.rept 8
	vadd.f32 s0, s0, s0
.endr
	subs r0, r0, #1
	bne 1b
2:  bx lr
.size coverageVfp, .-coverageVfp

.section .text.coverageNeon,"ax",%progbits
.balign 4
.global coverageNeon
.type coverageNeon,%function
.thumb_func
coverageNeon:
	cmp r0, #0
	beq 2f
1:
.rept 8
	vadd.i32 q1, q1, q1
.endr
	subs r0, r0, #1
	bne 1b
2:  bx lr
.size coverageNeon, .-coverageNeon

.section .note.GNU-stack,"",%progbits
