; ZX-Wespi Spectrum - file menu (Z80, sjasmplus)
;
; Build:  build.cmd (this menu first: its labels go to spmenu.exp, which
;         the modules include)
; The binary is embedded in the ESP32 firmware (main/CMakeLists.txt), which
; wraps it in a BASIC program and appends the data below (zx_menu.c):
;
;    1 REM <this code><data>
;   10 IF USR 23760=1 THEN GO TO 20
;   15 LOAD ""
;   20 INPUT "WiFi password: "; LINE p$
;   30 IF LEN p$>63 THEN GO TO 20
;   40 LET b=PEEK 23766+256*PEEK 23767: POKE b,LEN p$
;   50 FOR i=1 TO LEN p$: POKE b+i,CODE p$(i): NEXT i
;   60 RANDOMIZE USR 23763: GO TO 10
;
; With PROG at 23755 (48K, no Interface 1) the REM text starts at
; 23755 + 2 (line number) + 2 (length) + 1 (REM token) = 23760, where a
; small table gives BASIC its entry points:
;   23760  the menu. USR returns 0: a file was chosen (LOAD "" follows);
;          1: a WiFi network was chosen (the password is asked in BASIC,
;          where the Spectrum's own editor gives every character)
;   23763  sends the WiFi choice (code of the WiFi module)
;   23766  address where BASIC puts the password: length, then characters
;
; Data, appended by the ESP32 right after this code (label `list`).
; Strings are ASCII with bit 7 set on the last character.
;   gen     1 byte    list generation, echoed in the reply
;   count   1 byte    number of files (max 200)
;   flag    FLAG_LEN  flag of the language in use (draw_flag), top right
;   msgs    M_COUNT strings, the texts below in that language (zx_text.c)
;   info    string    row 1 of the screen (IP, files), max 26 characters
;   files   count x [type][size][name]
;             type  bits 0-1: 0 TAP, 1 TZX, 2 PZX, 3 CSW; bit 7: fast load
;             size  KB, 0..255
;             name  string, max 21 characters, no extension
;
; Modules: what is not needed to pick a file (info and help, WiFi) is not
; part of this program. The menu asks for it ('O' reply) and the ESP32
; builds it on the spot, with fresh data, as one headerless block (flag
; OVL_FLAG) loaded at OVL by the ROM loader:
;   OVL+0  payload length, this word included (2: no module, the menu is
;          out of date; the ESP32 plays the menu again)
;   OVL+2  entry from the menu: returns A = 0 back to the list, 1 USR 1,
;          2 load the menu again (language changed)
;   OVL+5  entry from BASIC (USR 23763)
;   OVL+8  `list` of the menu it was assembled with (the ESP32 checks it)
; The block is shorter than OVL_MAX, so the loader stops with an error at
; its end: the length word and the checksum tell a good block.

; Screen: title and info rows, PER_PAGE files (letters A..Q) with a bar,
; two key rows. Keys: A-Q or ENTER load, 6/7 (or cursor down/up) move the
; bar, 5/8 (or cursor left/right) change page, SPACE info and help, W WiFi
; networks (both modules), 0 exit to BASIC.
;
; Reply block (flag REPLY_FLAG, flag >= 0x80 so the ROM uses the short
; ~2 s pilot):  command, gen, index low, index high. Commands:
;   'M'  menu started: the ESP32 stops the loop (sent before the list shows)
;   'L'  load file <index>: the ESP32 plays it
;   'O'  module <index>: 1 WiFi, 2 info (the ESP32 plays it)
;   'W'  WiFi (from the module): list id, network, password length, password
;   'G'  language <index> (from the info module): 0 PT, 1 ES, 2 EN
;
; Rules kept for returning to BASIC: IY and H'L' untouched, no EXX,
; interrupts left enabled (the ROM SAVE re-enables them). The colours of
; BASIC are put back before returning.

        DEVICE NONE
        ORG 23760

; ROM routines and system variables
CLS         EQU 0x0D6B
CHAN_OPEN   EQU 0x1601
SA_BYTES    EQU 0x04C2
LD_BYTES    EQU 0x0556
LAST_K      EQU 23560
BORDCR      EQU 23624
FLAGS       EQU 23611
FRAMES      EQU 23672
SCR_CT      EQU 23692
ATTR_P      EQU 23693
ATTRS       EQU 22528

; Layout
PER_PAGE    EQU 17          ; rows ROW0.. , letters A..Q
ROW0        EQU 3
ROW_KEYS    EQU 20          ; two rows of key help: 20, 21
COL_TYPE    EQU 24
COL_SIZE    EQU 28

; Colours (attribute bytes: flash, bright, paper, ink)
A_TEXT      EQU 0x07        ; white on black
A_TITLE     EQU 0x47        ; bright white on black
A_INFO      EQU 0x05        ; cyan on black
A_LETTER    EQU 0x46        ; bright yellow on black
A_DIM       EQU 0x07
A_BAR       EQU 0x4F        ; bright white on blue
A_KEYS      EQU 0x28        ; black on cyan

; Protocol
REPLY_FLAG  EQU 0xA5
QUIET       EQU 10          ; frames (200 ms) without EAR edges = gap in the loop
QUIET_MAX   EQU 500         ; frames (10 s): send anyway
EDGE_MAX    EQU 750         ; frames (15 s): no module coming (a WiFi scan takes ~2-8 s)

; Texts and flag (sent by the ESP32, see Data above)
FLAG_LEN    EQU 36          ; 4 characters: attribute, 8 pixel rows
FLAG_ROW    EQU 0
FLAG_COL    EQU 23
MSGS        EQU list+2+FLAG_LEN
M_HELLO     EQU 0           ; order of zx_text.c (menu[])
M_EMPTY     EQU 1
M_KEYS1     EQU 2           ; row 20, max 26 characters (the page number follows)
M_KEYS2     EQU 3           ; row 21, max 31
M_WAIT      EQU 4           ; "Loading " before the file name
M_MLOAD     EQU 5
M_MERR      EQU 6
M_MOLD      EQU 7
M_COUNT     EQU 8

; Modules. Free RAM while the menu runs, on 16K and 48K machines
OVL         EQU 0x7800      ; modules load here
OVL_MAX     EQU 0x0700      ; up to 0x7F00, below the 16K machine stack
OVL_FLAG    EQU 0xA6
MOD_WIFI    EQU 1
MOD_INFO    EQU 2
WREPLY      EQU 0x777D      ; WiFi reply: 'W', list id, network, then PWBUF
PWBUF       EQU 0x7780      ; password from BASIC: length, characters (max 63)

        jp start                ; 23760
        jp OVL+5                ; 23763
        dw PWBUF                ; 23766

start:
        ld a,(ATTR_P)
        ld (sv_attr),a
        ld a,(BORDCR)
        ld (sv_bord),a
        call colours_menu
        call cls2
        ld a,M_HELLO
        call pr_msg
        ; tell the ESP32 the menu is running, so it stops the loop
        ld a,'M'
        ld c,0
        call send_reply

        ; locate the data: info (after the texts), files
        ld hl,MSGS
        ld a,M_COUNT
        call str_n
        ld (p_info),hl
        call skip_str
        ld (p_files),hl

        ; pages = ceil(count / PER_PAGE), at least 1
        xor a
        ld (page),a
        ld (sel),a
        ld a,(list+1)
        ld b,0
.pg:    inc b
        sub PER_PAGE
        jr z,.pgdone
        jr nc,.pg
.pgdone:
        ld a,b
        ld (pages),a

; ---------------------------------------------------------------------------
; Main screen

redraw:
        call cls2
        call draw_header
        call draw_files
        call draw_keys
        ld a,(rows)
        or a
        jr z,keys
        ld b,a                  ; keep the bar on the page
        ld a,(sel)
        cp b
        jr c,.selok
        ld a,b
        dec a
        ld (sel),a
.selok: call bar_on

keys:
        call getkey
        cp 'a'
        jr c,.upper
        cp 'z'+1
        jr nc,.upper
        sub 32                  ; lower case to upper case
.upper:
        cp 13
        jp z,k_enter
        ld c,MOD_INFO
        cp ' '
        jp z,module
        cp '0'
        jp z,k_exit
        ld c,MOD_WIFI
        cp 'W'
        jp z,module
        cp '6'
        jr z,k_down
        cp 10                   ; cursor down
        jr z,k_down
        cp '7'
        jr z,k_up
        cp 11                   ; cursor up
        jr z,k_up
        cp '8'
        jr z,k_next
        cp 9                    ; cursor right
        jr z,k_next
        cp '5'
        jp z,k_prev
        cp 8                    ; cursor left
        jp z,k_prev
        sub 'A'
        jp c,keys
        ld c,a
        ld a,(rows)
        or a
        jp z,keys               ; empty list
        dec a
        cp c
        jp c,keys               ; no file on that row
        jr load_row

k_enter:
        ld a,(rows)
        or a
        jr z,keys
        ld a,(sel)
        ld c,a
load_row:                       ; C = row on this page
        ld a,(first)
        add a,c
        ld c,a                  ; C = file index
        jp select

k_down:
        ld a,(rows)
        ld b,a
        ld a,(sel)
        inc a
        cp b
        jr nc,.page
        call move_bar
        jr keys
.page:  ld a,(pages)        ; past the last row: next page, if any
        ld b,a
        ld a,(page)
        inc a
        cp b
        jp nc,keys
        ld (page),a
        xor a
        ld (sel),a
        jp redraw

k_up:
        ld a,(sel)
        or a
        jr z,.page
        dec a
        call move_bar
        jp keys
.page:  ld a,(page)          ; above the first row: previous page, if any
        or a
        jp z,keys
        dec a
        ld (page),a
        ld a,PER_PAGE-1
        ld (sel),a
        jp redraw

k_next:
        ld a,(pages)
        ld b,a
        ld a,(page)
        inc a
        cp b
        jp nc,keys
        ld (page),a
        xor a
        ld (sel),a
        jp redraw

k_prev:
        ld a,(page)
        or a
        jp z,keys
        dec a
        ld (page),a
        xor a
        ld (sel),a
        jp redraw

k_exit:
        call colours_basic
        call cls2
        rst 8                   ; report "0 OK": stops the BASIC line, no LOAD
        db 0xFF

; Moves the bar to row A
move_bar:
        push af
        ld a,(sel)
        call row_attr
        pop af
        ld (sel),a
        ; fall through

; Bar on row (sel)
bar_on:
        ld a,(sel)
        add a,ROW0
        call attr_row
        ld a,A_BAR
        ld b,32
        jp fill

; ---------------------------------------------------------------------------
; Drawing

; Title row (0) and info row (1), with the Spectrum stripes on the right
draw_header:
        ld hl,s_title
        call pr_str
        ld a,22
        call pr_char
        ld a,1
        call pr_char
        ld a,1
        call pr_char
        ld hl,(p_info)
        call pr_str
        ld hl,ATTRS
        ld a,A_TITLE
        ld b,32
        call fill
        ld a,A_INFO
        ld b,32
        call fill
        ld hl,list+2            ; the language's flag
        ld bc,FLAG_ROW*256+FLAG_COL
        call draw_flag
        ld hl,ATTRS+28          ; stripes: row 0 cols 28..31, row 1 cols 27..30
        call stripes
        ld hl,ATTRS+32+27
        ; fall through
stripes:
        ld de,stripe_attr
        ld b,4
.s:     ld a,(de)
        ld (hl),a
        inc hl
        inc de
        djnz .s
        ret

; The files of this page; sets first and rows
draw_files:
        ld a,(page)             ; first = page * PER_PAGE
        ld b,a
        xor a
        inc b
.mul:   dec b
        jr z,.muldone
        add a,PER_PAGE
        jr .mul
.muldone:
        ld (first),a
        xor a
        ld (rows),a
        ld a,(list+1)
        or a
        jr nz,.files
        ld bc,ROW0*256+1
        call pr_at
        ld a,M_EMPTY
        jp pr_msg
.files:
        ld a,(first)
        call find_entry         ; HL = entry
        ld c,0                  ; row on this page
.row:   ld a,(first)
        add a,c
        ld b,a
        ld a,(list+1)
        cp b
        jp z,.done              ; index == count: end of the list
        ld a,c
        cp PER_PAGE
        jr z,.done
        ld a,(hl)               ; type
        inc hl
        push hl
        ld hl,row_type
        ld e,c
        ld d,0
        add hl,de
        ld (hl),a
        pop hl
        ld a,(hl)               ; size
        ld (cur_size),a
        inc hl
        ld a,22                 ; AT ROW0+row,0: letter, space, name
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
        call pr_str
        push hl
        ld a,22                 ; AT row,COL_TYPE-1: fast mark, type
        call pr_char
        ld a,c
        add a,ROW0
        call pr_char
        ld a,COL_TYPE-1
        call pr_char
        ld hl,row_type
        ld e,c
        ld d,0
        add hl,de
        ld a,(hl)
        ld b,' '
        bit 7,a
        jr z,.nofast
        ld b,'>'
.nofast:
        and 3
        ld e,a                  ; DE = type * 3
        add a,a
        add a,e
        ld e,a
        ld a,b
        call pr_char
        ld hl,s_types
        add hl,de
        ld b,3
.ty:    ld a,(hl)
        call pr_char
        inc hl
        djnz .ty
        ld a,' '
        call pr_char
        ld a,(cur_size)
        call pr_num3
        ld a,'K'
        call pr_char
        ld a,c
        call row_attr
        pop hl
        inc c
        jp .row
.done:  ld a,c
        ld (rows),a
        ret

; The two key rows at the bottom, with the page number
draw_keys:
        ld bc,ROW_KEYS*256
        call pr_at
        ld a,M_KEYS1
        call pr_msg
        ld bc,(ROW_KEYS+1)*256
        call pr_at
        ld a,M_KEYS2
        call pr_msg
        ld a,(pages)
        cp 2
        jr c,.attr
        ld a,22
        call pr_char
        ld a,ROW_KEYS
        call pr_char
        ld a,27
        call pr_char
        ld a,(page)
        inc a
        call pr_num
        ld a,'/'
        call pr_char
        ld a,(pages)
        call pr_num
.attr:  ld a,ROW_KEYS
        call attr_row
        ld a,A_KEYS
        ld b,64
        jp fill

; Normal colours of file row A (on this page)
row_attr:
        push bc
        push de
        push hl
        ld e,a
        add a,ROW0
        call attr_row
        ld (hl),A_LETTER
        inc hl
        ld a,A_TEXT
        ld b,COL_TYPE-1
        call fill
        ld d,0                  ; type colour
        push hl
        ld hl,row_type
        add hl,de
        ld a,(hl)
        and 3
        ld e,a
        ld hl,type_attr
        add hl,de
        ld a,(hl)
        pop hl
        ld b,COL_SIZE-COL_TYPE
        call fill
        ld a,A_DIM
        ld b,32-COL_SIZE
        call fill
        pop hl
        pop de
        pop bc
        ret

; ---------------------------------------------------------------------------
; Load the chosen file (C = index)

select:
        push bc                 ; CLS and CHAN_OPEN do not keep BC
        call colours_basic
        call cls2
        pop bc
        ld a,M_WAIT
        call pr_msg
        ld a,c
        call find_entry
        inc hl
        inc hl
        call pr_str
        ld a,'L'
        call send_reply
        ld bc,0                 ; USR 23760 = 0: LOAD "" follows
        ret

; Sends the reply block: command A, list generation, index C.
; Waits for a gap on EAR first, so the ESP32 is not talking at the same time.
send_reply:
        ld (reply),a
        ld a,c
        ld (reply+2),a
        ld a,(list)
        ld (reply+1),a
        call wait_quiet
        ld ix,reply
        ld de,4
        ld a,REPLY_FLAG
        jp SA_BYTES

; Waits for QUIET frames without an edge on EAR (a gap between the blocks
; of the ESP32 loop), at most QUIET_MAX frames.
wait_quiet:
        ld hl,(FRAMES)
        ld (t_begin),hl
        ld (t_edge),hl
        call ear
        ld e,a
.loop:  call ear
        cp e
        jr z,.same
        ld e,a
        ld hl,(FRAMES)
        ld (t_edge),hl
        jr .timeout
.same:  ld hl,(FRAMES)
        ld bc,(t_edge)
        or a
        sbc hl,bc
        ld bc,QUIET
        or a
        sbc hl,bc
        ret nc                  ; quiet long enough
.timeout:
        ld hl,(FRAMES)
        ld bc,(t_begin)
        or a
        sbc hl,bc
        ld bc,QUIET_MAX
        or a
        sbc hl,bc
        jr c,.loop
        ret

; ---------------------------------------------------------------------------
; Module C from the ESP32 (see Modules above): request, load, check, run

module: push bc
        call cls2
        call draw_header
        ld bc,ROW0*256+1
        call pr_at
        ld a,M_MLOAD
        call pr_msg
        call wait_release       ; a held SPACE would BREAK the loader
        pop bc
        ld a,'O'
        call send_reply
        call wait_edge          ; the request lost (noise): no tape comes
        jr nc,.bad
        ld ix,OVL
        ld de,OVL_MAX
        ld a,OVL_FLAG
        scf
        call LD_BYTES           ; stops with an error at the end of the block
        push ix                 ; bytes stored: must be the length word + 1
        pop hl
        ld de,OVL
        or a
        sbc hl,de
        ld bc,(OVL)
        inc bc
        or a
        sbc hl,bc
        jr nz,.bad
        ld hl,OVL               ; flag xor all bytes, checksum included: 0
        ld a,OVL_FLAG
.sum:   xor (hl)
        cpi
        jp pe,.sum
        or a
        jr nz,.bad
        ld hl,(OVL)
        dec hl
        dec hl
        ld a,h
        or l
        jr z,.old
        call OVL+2
        or a
        jp z,redraw
        cp 2
        jr z,.old
        ld bc,1                 ; USR 23760 = 1: BASIC asks for the password
        ret
.old:   call cls2               ; the ESP32 plays the new menu: LOAD "" takes it
        ld bc,ROW0*256+1
        call pr_at
        ld a,M_MOLD
        call pr_msg
        call getkey
        call colours_basic
        call cls2
        ld bc,0
        ret
.bad:   ld bc,(ROW0+2)*256+1
        call pr_at
        ld a,M_MERR
        call pr_msg
        call getkey
        jp redraw

; Waits for an edge on EAR, at most EDGE_MAX frames; carry set if one came.
; (The ROM loader would wait for ever.)
wait_edge:
        ld hl,(FRAMES)
        ld (t_begin),hl
        call ear
        ld e,a
.loop:  call ear
        cp e
        scf
        ret nz
        ld hl,(FRAMES)
        ld bc,(t_begin)
        or a
        sbc hl,bc
        ld bc,EDGE_MAX
        or a
        sbc hl,bc
        jr c,.loop
        or a
        ret

; Waits until no key is pressed
wait_release:
        xor a
        in a,(0xFE)
        cpl
        and 0x1F
        jr nz,wait_release
        ret

; ---------------------------------------------------------------------------
; Helpers

colours_menu:
        ld a,A_TEXT
        ld (ATTR_P),a
        ld (BORDCR),a           ; border black, lower screen white on black
        xor a
        out (0xFE),a
        ret

colours_basic:
        ld a,(sv_attr)
        ld (ATTR_P),a
        ld a,(sv_bord)
        ld (BORDCR),a
        rrca
        rrca
        rrca
        and 7
        out (0xFE),a
        ret

; Clears the screen, prints to the upper screen, no "scroll?" prompt
cls2:   call CLS
        ld a,2
        call CHAN_OPEN
        ld a,255
        ld (SCR_CT),a
        ret

; A = EAR bit (0 or 0x40)
ear:    ld a,0xFF
        in a,(0xFE)
        and 0x40
        ret

; A = key code, waits for a new key press (needs interrupts on)
getkey: ld hl,FLAGS
        res 5,(hl)
.w:     halt
        bit 5,(hl)
        jr z,.w
        ld a,(LAST_K)
        ret

; HL = attribute address of screen row A
attr_row:
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,ATTRS
        add hl,de
        ret

; Fills B bytes at HL with A; HL ends past them
fill:   ld (hl),a
        inc hl
        djnz fill
        ret

; HL = entry A (0-based) of the file list
find_entry:
        ld hl,(p_files)
        or a
        ret z
        ld b,a
.skip:  inc hl                  ; type, size
        inc hl
        call skip_str
        djnz .skip
        ret

; HL = string A (0-based) of the strings at HL
str_n:  or a
        ret z
        push bc
        ld b,a
.s:     call skip_str
        djnz .s
        pop bc
        ret

; Prints message A of the texts sent by the ESP32
pr_msg: ld hl,MSGS
        call str_n
        jp pr_str

; Print position: row B, column C
pr_at:  ld a,22
        call pr_char
        ld a,b
        call pr_char
        ld a,c
        jp pr_char

; Draws a flag at row B, column C: 4 characters of [attribute][8 pixel
; rows] at HL, written straight to the screen. HL ends past the data.
draw_flag:
        ld a,b                  ; DE = top pixel row of the first character
        and 0x18
        or 0x40
        ld d,a
        ld a,b
        and 7
        rrca
        rrca
        rrca
        or c
        ld e,a
        ld b,4
.chr:   push bc
        push de
        ld a,d                  ; attribute: 0x58 + third of the screen, same low byte
        rrca
        rrca
        rrca
        and 3
        or 0x58
        ld b,a
        ld c,e
        ld a,(hl)
        ld (bc),a
        inc hl
        ld b,8
.pix:   ld a,(hl)
        ld (de),a
        inc hl
        inc d
        djnz .pix
        pop de
        inc e
        pop bc
        djnz .chr
        ret

; HL past the string at HL (last character has bit 7 set)
skip_str:
        ld a,(hl)
        inc hl
        bit 7,a
        jr z,skip_str
        ret

; Prints a string whose last character has bit 7 set; HL ends past it.
pr_str: ld a,(hl)
        inc hl
        push af
        and 0x7F
        call pr_char
        pop af
        bit 7,a
        jr z,pr_str
        ret

; Prints A (0..255) right-aligned in 3 columns
pr_num3:
        push bc
        ld c,' '                ; leading zeros as spaces
        ld b,100
        call .dig
        ld b,10
        call .dig
        add a,'0'
        call pr_char
        pop bc
        ret
.dig:   ld d,'0'-1
.d:     inc d
        sub b
        jr nc,.d
        add a,b
        push af
        ld a,d
        cp '0'
        jr nz,.nz
        ld a,c
        jr .out
.nz:    ld c,'0'
.out:   call pr_char
        pop af
        ret

; Prints A (0..99) in decimal
pr_num: ld b,'0'-1
.d:     inc b
        sub 10
        jr nc,.d
        add a,10+'0'
        push af
        ld a,b
        cp '0'
        call nz,pr_char
        pop af
        ; fall through

; Prints A through the current channel, all registers except AF kept
pr_char:
        push bc
        push de
        push hl
        rst 0x10
        pop hl
        pop de
        pop bc
        ret

; ---------------------------------------------------------------------------
; Texts and variables

s_title:    DC " ZX-Wespi"
s_types:    DB "TAPTZXPZXCSW"
type_attr:  DB 0x45,0x46,0x44,0x42     ; bright cyan, yellow, green, red ink
stripe_attr: DB 0x10,0x30,0x20,0x28    ; paper red, yellow, green, cyan

page:       DB 0
pages:      DB 1
first:      DB 0
rows:       DB 0
sel:        DB 0
cur_size:   DB 0
sv_attr:    DB 0x38
sv_bord:    DB 0x38
p_info:     DW 0
p_files:    DW 0
t_begin:    DW 0
t_edge:     DW 0
reply:      DB 'L', 0, 0, 0
row_type:   DS PER_PAGE

list:       ; data appended here by the ESP32

; For the modules (spmenu.exp)
        EXPORT OVL
        EXPORT OVL_FLAG
        EXPORT WREPLY
        EXPORT PWBUF
        EXPORT REPLY_FLAG
        EXPORT SA_BYTES
        EXPORT ROW0
        EXPORT ROW_KEYS
        EXPORT A_TEXT
        EXPORT A_INFO
        EXPORT A_LETTER
        EXPORT A_KEYS
        EXPORT list
        EXPORT cls2
        EXPORT draw_header
        EXPORT pr_str
        EXPORT pr_char
        EXPORT attr_row
        EXPORT fill
        EXPORT getkey
        EXPORT skip_str
        EXPORT str_n
        EXPORT pr_at
        EXPORT draw_flag
        EXPORT send_reply
        EXPORT FLAG_LEN
        EXPORT wait_quiet
