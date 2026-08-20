@ Minimal Luma custom SVC stubs required by the standalone Pokebot3DS sysmodule.
.arm
.balign 4

.macro SVC_BEGIN name
    .section .text.\name, "ax", %progbits
    .global \name
    .type \name, %function
    .align 2
\name:
.endm

.macro SVC_END
.endm

SVC_BEGIN svcConvertVAToPA
    svc 0x90
    bx lr
SVC_END

SVC_BEGIN svcMapProcessMemoryEx
    push {r4, r5, r6}
    ldr r4, [sp, #12]
    ldr r5, [sp, #16]
    mov r6, r0
    mov r0, #0xFFFFFFF2
    svc 0xA0
    pop {r4, r5, r6}
    bx lr
SVC_END

SVC_BEGIN svcUnmapProcessMemoryEx
    svc 0xA1
    bx lr
SVC_END
