; ZX-Wespi Spectrum - info, help and language module (Z80, sjasmplus)
;
; Loaded by the menu at OVL when SPACE is pressed (see "Modules" in
; spmenu.asm); built with build.cmd after the menu, whose routines it calls.
; The ESP32 sets the length word and appends, after `data`:
;   lang    1 byte    language in use: 0 PT, 1 ES, 2 EN
;   flags   3 x FLAG_LEN, in that order
;   texts   T_COUNT strings (zx_text.c, info[]): language label, key row,
;           NHELP help lines
;   nsys    1 byte    number of lines
;   sys     nsys x string, max 30 characters each (web address, WiFi,
;           MAC, disk, tapes, version)
; Keys 1-3 choose the language: the module sends 'G' and returns 2 (the
; menu then loads again, in the new language). Any other key returns 0.

        DEVICE NONE
        INCLUDE "spmenu.exp"
        ORG OVL

        DW 0                    ; payload length (ESP32)
        jp main                 ; OVL+2
        ret                     ; OVL+5: not used
        nop
        nop
        DW list                 ; OVL+8: the menu this was built for

NHELP       EQU 6
T_LABEL     EQU 0
T_KEYS      EQU 1
T_HELP      EQU 2
T_COUNT     EQU 2+NHELP
LANG_ROW    EQU 18
FLAGS       EQU data+1
TEXTS       EQU data+1+3*FLAG_LEN

main:   call cls2
        call draw_header
        ld hl,TEXTS             ; system lines, from row ROW0, column 1
        ld a,T_COUNT
        call str_n
        ld b,(hl)
        inc hl
        ld c,ROW0
.line:  ld a,b
        or a
        jr z,.help
        push bc
        ld b,c
        ld c,1
        call pr_at
        pop bc
        call pr_str
        inc c
        dec b
        jr .line
.help:  inc c                   ; help lines, after a blank row
        ld hl,TEXTS
        ld a,T_HELP
        call str_n
        ld b,NHELP
.hl:    push bc
        ld b,c
        ld c,0
        call pr_at
        pop bc
        call pr_str
        inc c
        djnz .hl

        ld bc,(LANG_ROW-1)*256+1    ; the languages: >1 [flag]  2 [flag]  3 [flag]
        call pr_at
        ld hl,TEXTS
        call pr_str
        ld hl,FLAGS
        ld bc,LANG_ROW*256+1
        ld e,'1'
.fl:    call pr_at
        ld a,(data)
        add a,'1'
        cp e
        ld a,' '
        jr nz,.mark
        ld a,'>'
.mark:  call pr_char
        ld a,e
        call pr_char
        push bc
        push de
        inc c
        inc c
        inc c
        call draw_flag
        pop de
        pop bc
        ld a,c
        add a,10
        ld c,a
        inc e
        ld a,e
        cp '4'
        jr nz,.fl

        ld bc,(ROW_KEYS+1)*256
        call pr_at
        ld hl,TEXTS
        ld a,T_KEYS
        call str_n
        call pr_str
        ld a,ROW_KEYS+1
        call attr_row
        ld a,A_KEYS
        ld b,32
        call fill

        call getkey             ; 1-3: another language
        sub '1'
        jr c,.back
        cp 3
        jr nc,.back
        ld hl,data
        cp (hl)
        jr z,.back
        ld c,a
        ld a,'G'
        call send_reply
        ld a,2                  ; the menu loads again
        ret
.back:  xor a
        ret

data:       ; appended by the ESP32
