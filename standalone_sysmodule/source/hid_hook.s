@ Pokebot3DS standalone HID hook.
@ Adapted from Luma3DS Rosalina InputRedirection (GPLv3 or later).
@
@ The remote HID value 0xFFF is neutral and causes physical buttons to pass
@ through unchanged. The standalone module writes only the remote HID/touch/
@ circle words after explicit INPUT_ARM.

.section .rodata.pokebot_hid_hook, "a", %progbits
.balign 4
.arm

.global hidCodePatchFunc
.type hidCodePatchFunc, %function
hidCodePatchFunc:
    push {r4-r6, lr}

    push {r3}
    mov r5, r1
    mrc p15, 0, r4, c13, c0, 3
    mov r1, #0x10000
    str r1, [r4,#0x80]!
    ldr r0, [r0]
    svc 0x32
    mov r12, r0
    pop {r3}

    cmp r12, #0
    bne skip_touch_cp_cpy

    ldrd r0, [r4,#8]
    strd r0, [r3,#12]
    ldrd r0, [r3,#4]
    strd r0, [r5]

skip_touch_cp_cpy:
    mov r0, r3
    ldr r1, =0x1ec46000
    mov r3, #0xf00
    orr r3, #0xff

    @ HID: remote +20 -> local +0 unless remote is neutral 0xFFF.
    ldr r1, [r1]
    ldr r2, [r0,#20]
    cmp r2, r3
    movne r1, r2
    str r1, [r0]

    @ Touch: remote +24 -> local +4 unless neutral 0x02000000.
    cmp r12, #0
    mov r3, #0x2000000
    ldreq r1, [r0,#12]
    movne r1, r3
    ldr r2, [r0,#24]
    cmp r2, r3
    movne r1, r2
    str r1, [r0,#4]

    @ Circle: remote +28 -> local +8 unless neutral 0x007FF7FF.
    cmp r12, #0
    ldr r3, =0x7ff7ff
    ldreq r1, [r0,#16]
    movne r1, r3
    ldr r2, [r0,#28]
    cmp r2, r3
    movne r1, r2
    str r1, [r0,#8]

    ldr r0, [r4,#4]
    pop {r4-r6, pc}

.pool
