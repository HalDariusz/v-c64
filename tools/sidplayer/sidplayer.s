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

; --- adresy "skrzynki pocztowej" hypercalla SIDLOAD (musza byc zgodne z
; guest/kernel.c:do_sid_load) ---
SID_MB_LOAD     = $02      ; +0/+1: adres zaladowania danych utworu
SID_MB_INIT     = $04      ; +0/+1: adres rutyny INIT
SID_MB_PLAY     = $06      ; +0/+1: adres rutyny PLAY (0 = self-driven)
SID_MB_SONGS    = $08      ; liczba podutworow w pliku
SID_MB_START    = $09      ; domyslny podutwor, juz 0-based
SID_MB_STATUS   = $0A      ; 1 = OK, 0 = blad

SID_LOAD_HYPERCALL = $F700

; Bezpieczna kopia adresu PLAY, POZA strona zerowa. Wiele odtwarzanych
; utworow agresywnie uzywa niskiej strony zerowej na wlasne zmienne, a
; JSR_PLAY czyta adres docelowy PRZY KAZDYM przerwaniu (nie tylko raz) -
; gdyby czytal wprost z mailboxa SID_MB_PLAY ($06/$07), po pierwszym
; nadpisaniu przez sam utwor skoczylby w losowe miejsce. $0340 to klasyczny,
; bezpieczny obszar "bufora kasetowego" ($033C-$03FB), nieuzywany bez
; faktycznego napedu tasmowego.
SAFE_PLAY       = $0340

.segment "CODE"

; --- naglowek kartridza C64: cold-start vector, NMI vector, sygnatura ------
        .word START
        .word NMI_HANDLER
        .byte "CBM80"

START:
        SEI
        LDX     #$FF
        TXS
        CLD

        ; --- wyczysc ekran (spacja) i pamiec kolorow (niebieski tlo) -------
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
        STA     VIC+$21         ; tlo (VIC_BGCOLOR0, $D021)
        LDA     #0
        STA     $D020           ; ramka

        ; --- tytul (kody ekranowe, NIE ascii/petscii) -----------------------
        LDX     #0
PRINT_TITLE:
        LDA     TITLE_TEXT,X
        BEQ     TITLE_DONE
        STA     $0450,X         ; wiersz 2, od kolumny 0
        LDA     #1              ; bialy
        STA     $D850,X
        INX
        JMP     PRINT_TITLE
TITLE_DONE:

        ; --- SIDLOAD: ustaw nazwe pliku i wywolaj hypercall hosta ----------
        LDA     #<FILENAME
        STA     FNAM
        LDA     #>FILENAME
        STA     FNAM+1
        LDA     #FILENAME_LEN
        STA     FNAM_LEN

        JSR     SID_LOAD_HYPERCALL

        LDA     SID_MB_STATUS
        BNE     LOAD_OK

        ; --- blad wczytywania: czerwona ramka, petla ------------------------
        LDA     #2
        STA     $D020
HANG_ERR:
        JMP     HANG_ERR

LOAD_OK:
        ; --- zabezpiecz adres PLAY POZA strona zerowa, zanim cokolwiek z
        ; kodu utworu zdazy wystartowac i ewentualnie nadpisac $06/$07 -----
        LDA     SID_MB_PLAY
        STA     SAFE_PLAY
        LDA     SID_MB_PLAY+1
        STA     SAFE_PLAY+1

        ; --- INIT(A = numer podutworu 0-based) -----------------------------
        LDA     SID_MB_START
        LDX     #0
        LDY     #0
        JSR     JSR_INIT

        ; --- jawny adres PLAY, czy self-driven? -----------------------------
        LDA     SID_MB_PLAY
        ORA     SID_MB_PLAY+1
        BEQ     SELF_DRIVEN

        ; --- jawny PLAY: wlasne przerwanie rastra, CIA wylaczone -----------
        SEI
        LDA     #$7F
        STA     CIA1_ICR
        STA     CIA2_ICR
        LDA     CIA1_ICR        ; odczyt kasuje ewentualny stary stan ICR
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
        INC     $D020           ; delikatnie migajaca ramka = oznaka zycia
        LDX     #0
IDLE_DELAY:
        LDY     #0
IDLE_DELAY2:
        DEY
        BNE     IDLE_DELAY2
        DEX
        BNE     IDLE_DELAY
        JMP     IDLE

; --- trampoliny posredniego JSR (klasyczna sztuczka: adres-1 na stos + RTS) -
; RTS zawsze dodaje 1 do zdjetego ze stosu adresu, wiec trzeba wepchnac
; (cel - 1), nie sam cel - std. 16-bitowe odejmowanie z propagacja pozyczki.
; UWAGA na kolejnosc PHA: RTS zdejmuje PCL jako PIERWSZY bajt (to ten
; wypchniety NAJPOZNIEJ), wiec wysoki bajt trzeba wepchnac PIERWSZY, a niski
; DRUGI (dokladnie odwrotnie niz naturalna kolejnosc liczenia odejmowania,
; ktora zaczyna sie od niskiego bajtu ze wzgledu na pozyczke) - stad TAX/TXA
; jako tymczasowa schowka na niski bajt.
JSR_INIT:
        SEC
        LDA     SID_MB_INIT
        SBC     #1
        TAX                     ; X = niski bajt (cel-1), na potem
        LDA     SID_MB_INIT+1
        SBC     #0
        PHA                     ; wysoki bajt PIERWSZY -> RTS zdejmie go DRUGI (PCH)
        TXA
        PHA                     ; niski bajt DRUGI -> RTS zdejmie go PIERWSZY (PCL)
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
        STA     VIC_IRR         ; potwierdz przerwanie rastra
        PLA
        TAY
        PLA
        TAX
        PLA
        RTI

NMI_HANDLER:
        RTI

; --- dane --------------------------------------------------------------------
TITLE_TEXT:
        .byte 19,9,4,32,16,12,1,25,5,18,0      ; "SID PLAYER" (kody ekranowe)

FILENAME:
        .byte "Armalyte.sid"                    ; <-- podmien nazwe pliku tutaj
FILENAME_LEN = * - FILENAME
