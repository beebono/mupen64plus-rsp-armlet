#!/bin/bash
# Builds rspdis (the decoder ucodemap.py runs: the fork's disassembler plus a small control-flow decoder) with the host compiler.
set -e
cd "$(dirname "$0")"
${CXX:-g++} -O2 -std=c++14 -I../../ext/parallel-rsp -o rspdis rspdis.cpp ../../ext/parallel-rsp/rsp_disasm.cpp
echo "built $PWD/rspdis"
