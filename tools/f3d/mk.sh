#!/bin/bash
# private build dirs for the lead's work
cd "$(dirname "$0")"
make OBJDIR=$PWD/../../test-output/f3d/objA BIN=$PWD/../../test-output/f3dA "$@"
