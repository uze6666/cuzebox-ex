; Tiny synthetic routine used by the RSP test harness to exercise
; breakpoints and watchpoints without depending on the layout of any
; particular test ROM. Assembled standalone and injected into unused flash
; at runtime via M/X packets, with $pc pointed at it directly.
; No .global: this is assembled standalone and never linked (raw .text
; bytes are extracted straight out of the .o), so "probe" must stay a local
; symbol or the intra-file rjmp below is left as an unresolved relocation
; (encoded as a k=0 placeholder) instead of being resolved at assembly time.
.text
probe:
    ldi  r16, 0x42
    sts  0x0200, r16   ; data-space write watchpoint target
    lds  r17, 0x0200   ; data-space read watchpoint target
    nop                ; software breakpoint target
    rjmp probe         ; loop back to the top, not a self-loop: the emulator
                        ; keeps running between RSP packets (no --gdb-wait),
                        ; so a breakpoint/watchpoint installed after $pc was
                        ; already redirected here must still get a chance to
                        ; fire on the next pass rather than being stranded
                        ; past its target forever
