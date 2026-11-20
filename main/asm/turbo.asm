; ZX-Wespi Spectrum - load-on-request routine with a turbo loader (Z80, sjasmplus)
;
; The ESP32 appends this code, in a REM line, to every BASIC program it
; sends from a .tap (zx_turbo.c), and replaces each LOAD statement of the
; program with a call to it:
;
;   LOAD ""CODE / SCREEN$  ->  RANDOMIZE USR <stub k>             (turbo)
;   other LOADs            ->  RANDOMIZE USR <stub k>: LOAD ...    (ROM)
;
; The ESP32 places its own stubs right before this code, one per LOAD
; statement:  LD A,k (bit 7 set: ROM load)  /  JP entry  (5 bytes each).
; The code is relocated by the ESP32 (relocation table from tools/mkturbo.py,
; which assembles this file at two addresses), and the request block below
; gets the program id written into it.
;
; entry, A = statement:
;   1. sends a request through MIC: the ROM SAVE routine entered at SA-FLAG
;      with a short pilot (REQ_PILOT half-cycles, ~0.25 s instead of ~2 s),
;      flag REQ_FLAG, 4 bytes: 'T', program id, statement, 0;
;   2. ROM load (bit 7): returns, the original LOAD statement follows;
;   3. turbo: receives the block and returns, or stops with
;      "R Tape loading error" / "D BREAK - CONT repeats".
; A turbo block that fails (checksum, signal lost) is asked for again, up to
; RETRIES times: once the signal is over, an error request goes out, 'E',
; Bp (measured pilot), bytes left (2; 0 = checksum), and the ESP32 plays the
; block again. Only then "R Tape loading error".
;
; Turbo signal. Edges are timed with the ROM's own LD-EDGE-1 (in ROM, so not
; slowed down by contended RAM): B = number of 59 T-state samples after its
; start delay. Two bits per pulse: symbols 0..3 are pulses SPACING samples
; apart.
;   pilot  pulses as long as symbol 3: >= PIL_COUNT of them, the last
;          CAL_COUNT measured to place the thresholds (any clock, any offset).
;          The IN of LD-EDGE-1 (port 0x7FFE) is slowed down by the ULA while
;          it draws the screen, so the same pulse counts lower there: the
;          measurement spans about a whole frame (32 pulses) to average it.
;   sync   one pulse with B >= SYNC_MIN
;   bytes  4 symbols each, high bits first: address (2), length (2), data,
;          checksum (XOR of all the others)
; The time this code takes between two edges depends on the path (symbol,
; end of a byte...). The ESP32 adds the difference to the next pulse, so
; every symbol reaches LD-EDGE-1 with the same timing. The T-states of each
; path are noted below (ESP32 side: TB_* in zx_turbo.c); keep them in step.
;
; Kept for BASIC: IY and H'L' untouched, no EXX, interrupts enabled on
; return.

        DEVICE NONE
        IFNDEF BASE
BASE    EQU 0x8000
        ENDIF
        ORG BASE

SA_FLAG     EQU 0x04D0          ; SA-BYTES after its pilot length choice
SA_LD_RET   EQU 0x053F
LD_EDGE_1   EQU 0x05E7
REQ_FLAG    EQU 0xA5
REQ_PILOT   EQU 0x0190          ; H = 1, L = 0x90: 256 + 144 = 400 half-cycles

SPACING     EQU 10              ; samples between symbols (ESP32: TB_STEP = 59 * SPACING)
PIL_MIN     EQU 20              ; pilot (symbol 3): count ~34
PIL_MAX     EQU 44
SYNC_MIN    EQU 52
PIL_COUNT   EQU 32              ; pilot pulses before the measurement
CAL_COUNT   EQU 32              ; pilot pulses measured (sum in L:E; the shift below)
RETRIES     EQU 2

entry:
        ld (req+2),a
        ld a,RETRIES
        ld (tries),a
        ld ix,req
        call send
        ld a,(req+2)
        bit 7,a
        ret nz                  ; ROM load: the LOAD statement follows

again:  di
        ld a,0x7F               ; C: border colour and the current EAR level,
        in a,(0xFE)             ; as LD-EDGE-1 expects
        rra
        and 0x20
        or 0x02
        ld c,a

; Pilot: H counts good pulses up to PIL_COUNT + CAL_COUNT, L:E adds up the
; counts of the last CAL_COUNT. Path after LD-EDGE-1, normal pilot pulse:
;   counting 141, measuring 160 (159 on a carry), afterwards 104 T-states;
;   the ESP32 times the pilot for the measured ones (TB_OV_PILOT)
pilot0: ld h,0
        ld l,0
        ld e,0
pilot:  ld b,0
        call LD_EDGE_1
        jr nc,.nc
        ld a,b
        cp SYNC_MIN
        jr nc,.sync
        cp PIL_MIN
        jr c,pilot0
        cp PIL_MAX+1
        jr nc,pilot0
        ld d,a
        ld a,h
        cp PIL_COUNT+CAL_COUNT
        jr nc,pilot             ; enough: wait for the sync
        cp PIL_COUNT
        jr c,.count
        ld a,e                  ; measuring
        add a,d
        ld e,a
        jr nc,.count
        inc l
.count: inc h
        jr pilot
.nc:    jr z,pilot0             ; no edge for a while: keep waiting
        jp brk                  ; SPACE pressed

; Sync seen. Thresholds from the measured pilot (= symbol 3): symbols are
; SPACING samples apart (Bp-30, Bp-20, Bp-10, Bp); a count below th1 = Bp-25
; is a 0, below th2 = Bp-15 a 1, below th3 = Bp-5 a 2, else a 3.
; Path from the sync edge to the first data edge: 263 T-states (TB_OV_SYNC)
.sync:  ld a,h
        cp PIL_COUNT+CAL_COUNT
        jr c,pilot0
        ld h,l                  ; Bp = (sum + 16) / 32
        ld l,e
        ld de,16
        add hl,de
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,h
        sub SPACING/2
        ld (th3+1),a
        sub SPACING
        ld (th2+1),a
        sub SPACING
        jr nc,.th1
        ld a,1                  ; pilot too short to place th1: keep it sane
.th1:   ld (th1+1),a

        ld ix,0xFFFF            ; bytes left, for an error request: header
        xor a                   ; checksum in A' (no RAM access between edges)
        ex af,af'
        call get_byte
        jr nc,fail
        ld a,e
        ld (dest),a
        call get_byte
        jr nc,fail
        ld a,e
        ld (dest+1),a
        call get_byte
        jr nc,fail
        ld a,e
        ld (count),a
        call get_byte
        jr nc,fail
        ld a,e
        ld (count+1),a
        ld hl,(dest)
        ld ix,(count)
.byte:  ld a,ixh
        or ixl
        jr z,.end
        call get_byte
        jr nc,fail
        ld (hl),e
        inc hl
        dec ix
        jr .byte
.end:   call get_byte           ; checksum
        jr nc,fail
        ex af,af'
        or a
        jr nz,bad
        ei
        ret

fail:   jr nz,brk               ; NC, NZ: SPACE pressed; NC, Z: signal lost
bad:    ld b,0                  ; wait for the end of the signal: no edge for
        call LD_EDGE_1          ; ~15000 T-states (the ESP32 would not hear
        jr c,bad                ; the request while it plays)
        jr nz,brk
        ld a,(tries)
        sub 1
        jr c,.give
        ld (tries),a
        ld (ereq+2),ix          ; bytes left
        ld a,(th3+1)
        add a,SPACING/2
        ld (ereq+1),a           ; Bp
        ld ix,ereq
        call send
        jp again
.give:  ei
        rst 8
        db 0x1A                 ; R Tape loading error
brk:    ei
        rst 8
        db 0x14                 ; D BREAK - CONT repeats

; E = next byte (4 symbols of 2 bits, high bits first), added to the checksum.
; Returns NC (flags of LD-EDGE-1) if the signal stops or SPACE is pressed.
; Paths after LD-EDGE-1 (T-states), class(s) = 19, 37, 55, 54 for s = 0..3:
;   within a byte:  ret 5, ld 4, ld 7, class, gb_got 43, call 17 = 76 + class
;   end of a byte:  ret 5, ld 4, ld 7, class, gb_got 38, checksum 26 = 80 + class,
;                   then the caller up to the next edge (see zx_turbo.c)
get_byte:
        ld e,0                  ; 7
        ld d,4                  ; 7
        ld b,0                  ; 7
gb_sym: call LD_EDGE_1          ; 17
        ret nc                  ; 5
        ld a,b                  ; 4
        ld b,0                  ; 7
th1:    cp 0                    ; 7   patched
        jr c,gb_got               ; 12/7
        inc b                   ; 4
th2:    cp 0                    ; 7   patched
        jr c,gb_got
        inc b
th3:    cp 0                    ; 7   patched
        jr c,gb_got
        inc b
gb_got: ld a,e                  ; 4
        add a,a                 ; 4
        add a,a                 ; 4
        or b                    ; 4
        ld e,a                  ; 4
        ld b,0                  ; 7
        dec d                   ; 4
        jr nz,gb_sym             ; 12/7
        ex af,af'               ; 4   checksum
        xor e                   ; 4
        ex af,af'               ; 4
        scf                     ; 4
        ret                     ; 10

; Sends the 4 bytes at IX through MIC: the ROM SAVE routine entered at
; SA-FLAG with a short pilot. Returns through SA/LD-RET (border back, EI);
; SPACE stops it with "D BREAK - CONT repeats".
send:   ld hl,SA_LD_RET
        push hl
        ld hl,REQ_PILOT
        ld de,4
        ld a,REQ_FLAG
        jp SA_FLAG

req:    db 'T', 0, 0, 0         ; byte 1 (program id) written by the ESP32
ereq:   db 'E', 0, 0, 0         ; error request: Bp, bytes left
tries:  db 0
dest:   dw 0
count:  dw 0
