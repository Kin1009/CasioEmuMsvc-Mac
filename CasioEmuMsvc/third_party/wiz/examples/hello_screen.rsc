# RAC/hdcompiler equivalent of the CWiz ROP example.
# Target: Casio fx-580VN X / RAC 580vnx database.
# Payload is injected at E9E0 and copied by the generic launcher to D730.
@section.main at 0xd730 backup 0xe9e0

lbl main
    setlr
    setsfr
    buffer_clear
    xr0 = 0x30, 0x01, eval(adr(text)+dist.main)
    line_print
    render.ddd4
    brk

lbl text
    str"HELLO ROP"
    hex 00

@section.launcher at 0xd180
hex FD 24
hex 30 30
setlr
xr0 = 0xd730, 0xe9e0
call 09451
hex fe 02
er14 = 0xd72e
sp = er14, pop er14
