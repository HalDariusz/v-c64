; tools/sidplayer/sidplayer.s - the first "real" program for the C64-on-KVM
; emulator.
;
; Runs as an autostart cartridge (CBM80 signature at $8004-$8008), so it
; starts up bypassing the KERNAL/BASIC entirely (see the RESET vector
; redirection mechanism in guest/memory_pla.c). Loads a .sid file
; (PSID/RSID format) via a custom host "hypercall" (JSR $F700 - see
; guest/kernel.c:do_sid_load and host/kvm_host.c:fastload_sid_execute),
; calls its INIT routine, and then either:
;   - if the tune has an explicit PLAY address: installs its own VIC-II
;     raster interrupt that calls PLAY every frame (~50 Hz),
;   - if PLAY == 0: the tune already installed its own interrupt during
;     INIT (typical of older/more "clever" players, e.g. Martin Galway's)
;     - just unmasking interrupts (CLI) is enough.
;
; The filename to load is hardcoded in the FILENAME constant below -
; change it and rebuild (see this directory's README.md) to play a
; different tune from disk/.
;
; Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
; on Linux /dev/kvm, with no QEMU involved.
;
; Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>

.include "c64.inc"

; --- SIDLOAD hypercall "mailbox" addresses (must match
; guest/kernel.c:do_sid_load) ---
SID_MB_LOAD     = $02      ; +0/+1: tune data load address
SID_MB_INIT     = $04      ; +0/+1: INIT routine address
SID_MB_PLAY     = $06      ; +0/+1: PLAY routine address (0 = self-driven)
SID_MB_SONGS    = $08      ; number of subsongs in the file
SID_MB_START    = $09      ; default subsong, already 0-based
SID_MB_STATUS   = $0A      ; 1 = OK, 0 = error

SID_LOAD_HYPERCALL = $F700

; Safe copy of the PLAY address, OUTSIDE the zero page. Many played tunes
; aggressively use low zero-page addresses for their own variables, and
; JSR_PLAY reads the target address on EVERY interrupt (not just once) -
; if it read directly from the SID_MB_PLAY mailbox ($06/$07), after the
; tune itself first overwrote it, it would jump to a random location.
; $0340 is the classic, safe "cassette buffer" area ($033C-$03FB), unused
; without an actual tape drive.
SAFE_PLAY       = $0340

.segment "CODE"

; --- C64 cartridge header: cold-start vector, NMI vector, signature ------
        .word START
        .word NMI_HANDLER
        .byte "CBM80"

START:
        SEI
        LDX     #$FF
        TXS
        CLD

        ; --- clear the screen (space) and color memory (blue background) -------
        LDX     #0
CLR_SCREEN:
        LDA     #32
        STA     $0400,X
        STA     $0500,X
        STA     $0600,X
        STA     $0700,X
        LDA     #6
        STA     $D800,X
        STA     $D900,X
        STA     $DA00,X
        STA     $DB00,X
        INX
        BNE     CLR_SCREEN

        LDA     #6
        STA     VIC+$21         ; background (VIC_BGCOLOR0, $D021)
        LDA     #0
        STA     $D020           ; border

        ; --- title (screen codes, NOT ascii/petscii) -----------------------
        LDX     #0
PRINT_TITLE:
        LDA     TITLE_TEXT,X
        BEQ     TITLE_DONE
        STA     $0450,X         ; row 2, from column 0
        LDA     #1              ; white
        STA     $D850,X
        INX
        JMP     PRINT_TITLE
TITLE_DONE:

        ; --- SIDLOAD: set the filename and call the host hypercall ----------
        LDA     #<FILENAME
        STA     FNAM
        LDA     #>FILENAME
        STA     FNAM+1
        LDA     #FILENAME_LEN
        STA     FNAM_LEN

        JSR     SID_LOAD_HYPERCALL

        LDA     SID_MB_STATUS
        BNE     LOAD_OK

        ; --- load error: red border, loop ------------------------
        LDA     #2
        STA     $D020
HANG_ERR:
        JMP     HANG_ERR

LOAD_OK:
        ; --- save the PLAY address OUTSIDE the zero page, before any of the
        ; tune's code gets to run and possibly overwrite $06/$07 -----
        LDA     SID_MB_PLAY
        STA     SAFE_PLAY
        LDA     SID_MB_PLAY+1
        STA     SAFE_PLAY+1

        ; --- INIT(A = 0-based subsong number) -----------------------------
        LDA     SID_MB_START
        LDX     #0
        LDY     #0
        JSR     JSR_INIT

        ; --- explicit PLAY address, or self-driven? -----------------------------
        LDA     SID_MB_PLAY
        ORA     SID_MB_PLAY+1
        BEQ     SELF_DRIVEN

        ; --- explicit PLAY: our own raster interrupt, CIA disabled -----------
        SEI
        LDA     #$7F
        STA     CIA1_ICR
        STA     CIA2_ICR
        LDA     CIA1_ICR        ; reading clears any old ICR state
        LDA     CIA2_ICR

        LDA     #<MY_IRQ
        STA     IRQVec
        LDA     #>MY_IRQ
        STA     IRQVec+1

        LDA     #$FF
        STA     VIC_HLINE
        LDA     VIC_CTRL1
        AND     #$7F
        STA     VIC_CTRL1
        LDA     #$01
        STA     VIC_IMR
        STA     VIC_IRR

SELF_DRIVEN:
        CLI

IDLE:
        INC     $D020           ; gently flashing border = sign of life
        LDX     #0
IDLE_DELAY:
        LDY     #0
IDLE_DELAY2:
        DEY
        BNE     IDLE_DELAY2
        DEX
        BNE     IDLE_DELAY
        JMP     IDLE

; --- indirect JSR trampolines (classic trick: push address-1 + RTS) -
; RTS always adds 1 to the address popped off the stack, so what must be
; pushed is (target - 1), not the target itself - a standard 16-bit
; subtraction with borrow propagation. NOTE the PHA order: RTS pops PCL
; as the FIRST byte (the one pushed LAST), so the high byte must be
; pushed FIRST, and the low byte SECOND (exactly the reverse of the
; natural order of computing the subtraction, which starts from the low
; byte because of the borrow) - hence TAX/TXA as a temporary stash for
; the low byte.
JSR_INIT:
        SEC
        LDA     SID_MB_INIT
        SBC     #1
        TAX                     ; X = low byte (target-1), for later
        LDA     SID_MB_INIT+1
        SBC     #0
        PHA                     ; high byte FIRST -> RTS pops it SECOND (PCH)
        TXA
        PHA                     ; low byte SECOND -> RTS pops it FIRST (PCL)
        RTS

JSR_PLAY:
        SEC
        LDA     SAFE_PLAY
        SBC     #1
        TAX
        LDA     SAFE_PLAY+1
        SBC     #0
        PHA
        TXA
        PHA
        RTS

MY_IRQ:
        PHA
        TXA
        PHA
        TYA
        PHA
        JSR     JSR_PLAY
        LDA     #$01
        STA     VIC_IRR         ; acknowledge the raster interrupt
        PLA
        TAY
        PLA
        TAX
        PLA
        RTI

NMI_HANDLER:
        RTI

; --- data --------------------------------------------------------------------
TITLE_TEXT:
        .byte 19,9,4,32,16,12,1,25,5,18,0      ; "SID PLAYER" (screen codes)

FILENAME:
        .byte "Armalyte.sid"                    ; <-- change the filename here
FILENAME_LEN = * - FILENAME
