#!/bin/sh
# Build TELETEKS.EXE with the SymbOS C Compiler (https://github.com/danielgaskell/scc)
cc -N Teletekst -h 512 -o TELETEKS.EXE teleteks.c ttcore.c ttcell.s ttfast.s -lnet -lgfx
# guard against SCC giving string-initialised _data/_transfer arrays zero size
python3 checkmap.py TELETEKS.EXE.map
