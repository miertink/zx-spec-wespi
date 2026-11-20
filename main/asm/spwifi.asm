; ZX-Wespi Spectrum - WiFi module (Z80, sjasmplus)
;
; Loaded by the menu at OVL when W is pressed (see "Modules" in spmenu.asm);
; built with build.cmd after the menu, whose routines it calls.
; The ESP32 sets the length word and appends, after `data`:
;   id      1 byte    list id, echoed in the reply
;   nnets   1 byte    networks in reach (max 10, letters A..J)
;   texts   W_COUNT strings, in the language in use (zx_text.c, wifi[])
;   wnow    string    the network in use, max 30 characters
;   nets    nnets x [level][name]
;             level bits 0-2: signal 1..4; bit 7: password needed
;             name  string, max 24 characters (the ESP32 keeps the full name)
;
; A letter picks a network: the module returns 1, BASIC asks for the
; password, puts it at PWBUF and calls `send` (USR 23763), which sends
; 'W', id, network, password length, password.

        DEVICE NONE
        INCLUDE "spmenu.exp"
        ORG OVL

        DW 0                    ; payload length (ESP32)
        jp main                 ; OVL+2
        jp send                 ; OVL+5
        DW list                 ; OVL+8: the menu this was built for

W_NONE      EQU 0               ; order of zx_text.c (wifi[])
W_KEYS1     EQU 1
W_KEYS2     EQU 2
W_PASS      EQU 3
W_HINT1     EQU 4
W_HINT2     EQU 5
W_SENT1     EQU 6
W_SENT2     EQU 7
W_ANYKEY    EQU 8
W_COUNT     EQU 9

main:   call cls2
        call draw_header
        ld hl,data
        ld a,'W'
        ld (WREPLY),a
        ld a,(hl)               ; list id
        ld (WREPLY+1),a
        inc hl
        ld a,(hl)
        ld (count),a
        inc hl
        ld (p_txt),hl
        ld a,W_COUNT
        call str_n
        ld bc,2*256+1           ; the network in use
        call pr_at
        call pr_str
        ld (p_list),hl
        ld a,2
        call attr_row
        ld a,A_INFO
        ld b,32
        call fill
        ld a,(count)
        or a
        jr nz,.list
        ld bc,ROW0*256+1
        call pr_at
        ld a,W_NONE
        call pr_txt
        jr .keys
.list:  ld hl,(p_list)
        ld c,0
.row:   ld a,22                 ; AT ROW0+c,0: letter, name, lock, signal
        call pr_char
        ld a,c
        add a,ROW0
        call pr_char
        xor a
        call pr_char
        ld a,c
        add a,'A'
        call pr_char
        ld a,' '
        call pr_char
        ld a,(hl)               ; level
        ld (level),a
        inc hl
        call pr_str
        push hl
        ld a,22
        call pr_char
        ld a,c
        add a,ROW0
        call pr_char
        ld a,27
        call pr_char
        ld a,(level)
        ld b,' '
        bit 7,a
        jr z,.open
        ld b,'+'                ; password needed
.open:  ld a,b
        call pr_char
        ld a,(level)            ; signal: level stars, then dots up to 4
        and 7
        ld b,a
        ld d,4
.bar:   ld a,'*'
        inc b
        dec b
        jr nz,.star
        ld a,'.'
        jr .put
.star:  dec b
.put:   call pr_char
        dec d
        jr nz,.bar
        ld a,c                  ; colours: letter, name, signal
        add a,ROW0
        call attr_row
        ld (hl),A_LETTER
        inc hl
        ld a,A_TEXT
        ld b,26
        call fill
        ld a,0x44               ; bright green ink
        ld b,5
        call fill
        pop hl
        inc c
        ld a,(count)
        cp c
        jr nz,.row
.keys:  ld bc,ROW_KEYS*256
        call pr_at
        ld a,W_KEYS1
        call pr_txt
        ld bc,(ROW_KEYS+1)*256
        call pr_at
        ld a,W_KEYS2
        call pr_txt
        ld a,ROW_KEYS
        call attr_row
        ld a,A_KEYS
        ld b,64
        call fill
.key:   call getkey
        cp 'a'
        jr c,.up
        sub 32
.up:    cp '0'
        jr z,.back
        sub 'A'
        jr c,.key
        ld c,a
        ld a,(count)
        dec a
        cp c
        jr c,.key
        ld a,c                  ; chosen: C = network index
        ld (WREPLY+2),a
        ld hl,(p_list)
        or a
        jr z,.found
        ld b,a
.skip:  inc hl                  ; level
        call skip_str
        djnz .skip
.found: inc hl
        push hl
        call cls2
        ld a,W_PASS
        call pr_txt
        pop hl
        call pr_str             ; the network
        call newline
        call newline
        ld a,W_HINT1
        call pr_txt
        call newline
        ld a,W_HINT2
        call pr_txt
        ld a,1                  ; BASIC asks for the password
        ret
.back:  xor a
        ret

; From BASIC: sends WREPLY + the password at PWBUF; waits for a key
send:   call wait_quiet
        ld ix,WREPLY
        ld a,(PWBUF)
        add a,4
        ld e,a
        ld d,0
        ld a,REPLY_FLAG
        call SA_BYTES
        call cls2
        ld a,W_SENT1
        call pr_txt
        call newline
        ld a,W_SENT2
        call pr_txt
        call newline
        call newline
        ld a,W_ANYKEY
        call pr_txt
        jp getkey

; Prints text A of the ESP32's list
pr_txt: ld hl,(p_txt)
        call str_n
        jp pr_str

newline:
        ld a,13
        jp pr_char

count:      DB 0
level:      DB 0
p_list:     DW 0
p_txt:      DW 0

data:       ; appended by the ESP32
