# builds the F3D routine tests: needs the fork's objects (built by ../../scripts? no: see $(OBJDIR))
R=$(abspath ../..)
P=$(R)/ext/parallel-rsp
API=$(R)/../mupen64plus-core/src/api
NATIVE_H=$(or $(NATIVE_INC),$(R)/src/native)
APRSP=$(or $(NATIVE_PRSP),$(R)/ext/parallel-rsp)
OBJDIR?=$(R)/test-output/f3d/obj$(TRACE)
ifneq ($(TRACE),)
TRC=-DPRSP_HOOKS -DPRSP_PROFILE -DF3D_TRACE
endif
CXXF=-O2 -g -std=c++14 -DPARALLEL_INTEGRATION -DM64P_PLUGIN_API -DPARALLEL_RSP_LIGHTNING -I$(P) -I$(P)/arch/simd/rsp -I$(P)/lightning/include -I$(API) -I$(NATIVE_H) -I$(R)/src/native -I$(APRSP) -ffast-math -w -msse4.2 -DHAVE_MMAP=1 $(TRC)
SRC=rsp/vfunctions.cpp rsp_jit.cpp jit_allocator.cpp rsp_disasm.cpp rsp/ls.cpp rsp/reciprocal.cpp rsp/cp0.cpp rsp/cp2.cpp shadow.cpp rsp/vu_inline.cpp arch/simd/rsp/rsp_core.cpp parallel.cpp
LSRC=lightning/lib/jit_disasm.c lightning/lib/jit_memory.c lightning/lib/jit_names.c lightning/lib/jit_note.c lightning/lib/jit_print.c lightning/lib/jit_size.c lightning/lib/lightning.c
OBJ=$(patsubst %.cpp,$(OBJDIR)/%.o,$(SRC)) $(patsubst %.c,$(OBJDIR)/%.o,$(LSRC))
NAT=$(patsubst $(R)/src/native/%.c,$(OBJDIR)/%.o,$(wildcard $(R)/src/native/f3d*.c))
HLEO=$(patsubst $(R)/src/hle/%.c,$(OBJDIR)/hle_%.o,$(wildcard $(R)/src/hle/f3d*.c))
BIN?=$(R)/test-output/f3d$(TRACE)
TESTS=$(patsubst test_%.cpp,%,$(wildcard test_*.cpp))
all: $(foreach t,$(TESTS),$(BIN)/test_$(t))
$(OBJDIR)/%.o: $(P)/%.cpp
	@mkdir -p $(dir $@); g++ $(CXXF) -c $< -o $@
$(OBJDIR)/%.o: $(P)/%.c
	@mkdir -p $(dir $@); gcc -O2 -w -DHAVE_MMAP=1 -I$(P)/lightning/include -c $< -o $@
$(OBJDIR)/%.o: $(R)/src/native/%.c $(wildcard $(R)/src/native/*.h)
	@mkdir -p $(dir $@); gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I$(NATIVE_H) -I$(R)/src/native -I$(APRSP) -I$(P) -c $< -o $@
$(OBJDIR)/hle_%.o: $(R)/src/hle/%.c $(wildcard $(R)/src/hle/*.h)
	@mkdir -p $(dir $@); gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(FXFLAGS) -I$(R)/src/hle -I$(P) -c $< -o $@
$(foreach t,$(TESTS),$(BIN)/test_$(t)): $(BIN)/test_%: test_%.cpp $(OBJ) $(NAT) $(HLEO) fh.hpp fhtest.hpp
	@mkdir -p $(BIN); g++ $(CXXF) -I$(R)/src/hle $< $(OBJ) $(NAT) $(HLEO) -o $@ -ldl

# replay tool: the native routines against a snapshot file, no JIT. `make replay` for the host, `make replay CROSS=aarch64-linux-gnu- RTAG=a64`
# for aarch64 (run it with qemu-aarch64-static -L /usr/aarch64-linux-gnu)
CROSS?=
RTAG?=host
RDIR=$(R)/test-output/f3d/replay-$(RTAG)
NATSRC=$(wildcard $(R)/src/native/f3d*.c)
RFLAGS=-O3 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$(NATIVE_H) -I$(R)/src/native -I$(APRSP) -I. -fno-strict-aliasing $(MUTFLAG)
.PHONY: replay
replay: $(RDIR)/replay
$(RDIR)/replay: replay.c snap.h $(NATSRC) $(wildcard $(R)/src/native/*.h)
	@mkdir -p $(RDIR)
	$(CROSS)gcc $(RFLAGS) -c replay.c -o $(RDIR)/replay.o
	@for f in $(NATSRC); do $(CROSS)gcc $(RFLAGS) -c $$f -o $(RDIR)/$$(basename $$f .c).o || exit 1; done
	$(CROSS)g++ -O2 -std=c++14 -I$(P) -c $(P)/rsp/reciprocal.cpp -o $(RDIR)/reciprocal.o
	$(CROSS)g++ $(RDIR)/*.o -o $@

# the NEON-vs-scalar test of the struct-level functions: plain C, no JIT; `make fxn CROSS=aarch64-linux-gnu- FTAG=a64` and run it with qemu-aarch64-static -L /usr/aarch64-linux-gnu
FTAG?=host
FDIR=$(R)/test-output/f3d/fxn-$(FTAG)
FXSRC=$(wildcard $(R)/src/hle/f3d*_fx.c)
FXOBJ=$(patsubst $(R)/src/hle/%.c,$(FDIR)/%.o,$(FXSRC)) $(FDIR)/reciprocal.o
.PHONY: fxn
fxn: $(FDIR)/test_fxn
$(FDIR)/test_fxn: test_fxn.c $(FXSRC) $(wildcard $(R)/src/hle/*.h)
	@mkdir -p $(FDIR)
	@for f in test_fxn.c $(FXSRC); do $(CROSS)gcc -O3 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$(R)/src/hle $(FXFLAGS) -c $$f -o $(FDIR)/$$(basename $$f .c).o || exit 1; done
	$(CROSS)g++ -O2 -std=c++14 -I$(P) -c $(P)/rsp/reciprocal.cpp -o $(FDIR)/reciprocal.o
	$(CROSS)g++ $(FDIR)/test_fxn.o $(FXOBJ) -o $@

# real-data check and timing on snapshots: `make bench CROSS=aarch64-linux-gnu- FTAG=a64` (links the same objects as fxn)
.PHONY: bench
bench: $(FDIR)/bench_fx
$(FDIR)/bench_fx: bench_fx.c $(FXSRC) $(wildcard $(R)/src/hle/*.h)
	@mkdir -p $(FDIR)
	@for f in bench_fx.c $(FXSRC); do $(CROSS)gcc -O3 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$(R)/src/hle -I$(P) $(FXFLAGS) -c $$f -o $(FDIR)/$$(basename $$f .c).o || exit 1; done
	$(CROSS)g++ -O2 -std=c++14 -I$(P) -c $(P)/rsp/reciprocal.cpp -o $(FDIR)/reciprocal.o
	$(CROSS)g++ $(LDSTATIC) $(FDIR)/bench_fx.o $(FXOBJ) -o $@
