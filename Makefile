# Ambre — QNX 8.0 Event Data Recorder
# Portable core: same C11 sources build for PC (host tools) and QNX 8.0.
#
#   make            build library, tests and tools
#   make test       run every unit/integration test (level 0)
#   make explore    exhaustive power-cut exploration, 10 000 states (level 1)
#   make explore-full  1 000 000 states (spec 7.7, "complet" level)
#   make crosscheck C trigger vs independent Python twin, 10 000 scenarios
#   make demo       one-command end-to-end demonstration (level 2, real cuts)
#   make e2e        one level-2 run: real processes, one SIGKILL, recovery
#   make processcut the cut campaign (200 cuts, zero loss) on the PC backend
#   make sizecheck  house rule: files < 500 lines, functions < 50 lines
#   make clean

CC       ?= gcc
CXX      ?= g++
AR       ?= ar
PYTHON   ?= python3
BUILD    := build
JOBS     ?= $(shell nproc 2>/dev/null || echo 4)

WARN     := -Wall -Wextra -Wshadow -Wpedantic
CFLAGS   ?= -std=c11 -O2 -g $(WARN) -Werror -Ifmt -I. -Itests -Ihost/simdisk \
            -Ihost/explore -Itarget -Itarget/port -Itarget/ipc
LDFLAGS  ?=
# C++17 is the language of the archive process (cahier des charges section 9).
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Werror

FMT_SRC  := $(sort $(wildcard fmt/*.c))
# Portable half of the five target processes + the portability layer.
# *_qnx.c files are NOT part of the PC library: they are syntax-checked only
# (target `qnx-syntax`), because they include real QNX headers.
PORT_SRC := $(sort $(filter-out %_qnx.c,$(wildcard target/port/*.c)))
PROC_SRC := $(sort $(filter-out %_qnx.c %/main.c %/main.cpp,$(wildcard target/*/*.c)))
IPC_SRC  := $(sort $(wildcard target/ipc/*.c))
# host tools linked into the PC library: the crash explorer and its support.
# host/explore/main.c stays out (it holds main()); it is linked by the CLI.
HOST_SRC := $(sort $(wildcard host/simdisk/*.c)) \
            $(sort $(filter-out %/main.c,$(wildcard host/explore/*.c)))
LIB_SRC  := $(FMT_SRC) $(HOST_SRC) $(PORT_SRC) $(PROC_SRC) $(IPC_SRC)
LIB_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC))
LIB      := $(BUILD)/libambre.a

TEST_SRC := $(sort $(wildcard tests/test_*.c))
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

EXPLORE_MAIN := host/explore/main.c
# One binary per process, linked from its main plus the portable library.
PROCESS_SRC  := $(filter-out target/ctl/main.c target/archive/main.cpp,\
                  $(wildcard target/*/main.c))
PROCESS_BIN  := $(patsubst target/%/main.c,$(BUILD)/edr-%,$(PROCESS_SRC)) \
                $(if $(wildcard target/ctl/main.c),$(BUILD)/edrctl)
TOOLS        := $(BUILD)/ambre-explore $(BUILD)/ambre-dump-truth \
                $(PROCESS_BIN) $(BUILD)/edr-archive

.PHONY: all test explore explore-full crosscheck demo sizecheck clean tools \
        scenario mcap verify help qnx-syntax processes image-check campaign test_archive_cpp \
        e2e e2e-scenario processcut

all: $(LIB) $(TEST_BIN) tools

help:
	@grep -E '^#   ' Makefile | sed 's/^#   //'

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	@mkdir -p $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/test_%: tests/test_%.c tests/ambre_test_util.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< tests/ambre_test_util.c $(LIB) -o $@ $(LDFLAGS)

tools: $(TOOLS)

processes: $(PROCESS_BIN)

$(BUILD)/edr-%: target/%/main.c $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS) -lpthread

# Syntax check of the QNX glue against the stub headers: objects only, never
# linked, so a missing QNX symbol can never break the PC build (docs/QNX-PORT.md).
QNX_GLUE := $(sort $(wildcard target/*/*_qnx.c))

$(BUILD)/edrctl: target/ctl/main.c $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS) -lpthread

# --- edr-archive: the C++17 MCAP archive process (cahier section 9) ---------
ARCHIVE_SRC := $(sort $(filter-out target/archive/main.cpp,\
                 $(wildcard target/archive/*.cpp)))
ARCHIVE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(ARCHIVE_SRC))
ARCHIVE_INC := -I. -Ifmt -Itarget -Itarget/archive -Itarget/port -Itarget/ipc

# -MMD -MP: the C++ headers of the archive are tracked, so touching one header
# rebuilds every translation unit that includes it.
$(BUILD)/target/archive/%.o: target/archive/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP $(ARCHIVE_INC) -c $< -o $@

-include $(ARCHIVE_OBJ:.o=.d)

$(BUILD)/edr-archive: target/archive/main.cpp $(ARCHIVE_OBJ) $(LIB)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(ARCHIVE_INC) target/archive/main.cpp \
	  $(ARCHIVE_OBJ) $(LIB) -o $@ $(LDFLAGS) -lpthread

# The C++ test suite: it links the archive objects, the C library and calls the
# Python reader/verifier of host/ to cross-check the produced MCAP files.
$(BUILD)/test_archive_cpp: tests/test_archive_cpp.cpp $(ARCHIVE_OBJ) $(LIB)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(ARCHIVE_INC) -Itests tests/test_archive_cpp.cpp \
	  $(ARCHIVE_OBJ) $(LIB) -o $@ $(LDFLAGS) -lpthread

test_archive_cpp: $(BUILD)/test_archive_cpp $(BUILD)/edr-archive
	@$(BUILD)/test_archive_cpp

# Static consistency of image/ without a QNX SDP: process names, priorities
# (spec 4.4), security-policy types, distinct partitions, no network, no stop
# command. Cannot build or boot a real image here (docs/QNX-PORT.md).
image-check:
	@bash scripts/check-image.sh

qnx-syntax:
	@mkdir -p $(BUILD)/qnx-syntax
	@for f in $(QNX_GLUE); do \
	  printf '  qnx-syntax %-46s' "$$f"; \
	  if $(CC) -std=c11 -O0 $(WARN) -Werror -fsyntax-only \
	       -Itarget/qnx-stub/include -Ifmt -Itarget -Itarget/port -Itarget/ipc \
	       -I. "$$f" 2>$(BUILD)/qnx-syntax/$$(basename $$f).log; then \
	    echo OK; \
	  else \
	    echo FAIL; cat $(BUILD)/qnx-syntax/$$(basename $$f).log; exit 1; \
	  fi; \
	done
	@echo "qnx-syntax: $$(echo $(QNX_GLUE) | wc -w) glue file(s) compile against the stubs"

$(BUILD)/ambre-explore: $(EXPLORE_MAIN) $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(EXPLORE_MAIN) $(LIB) -o $@ $(LDFLAGS) -lm

$(BUILD)/ambre-dump-truth: tests/dump_truth.c $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) tests/dump_truth.c $(LIB) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	@fails=0; for t in $(TEST_BIN); do \
	  echo "== $$t"; $$t || fails=1; \
	done; \
	if [ $$fails -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

scenario:
	$(PYTHON) host/scenarios/generate.py --seed 1234 --duration-ms 30000 \
	  --crash-at-ms 25000 --out $(BUILD)/scenario.txt

explore: all scenario
	$(BUILD)/ambre-explore --scenario $(BUILD)/scenario.txt \
	  --states 10000 --jobs $(JOBS) --variants all --report $(BUILD)/explore.json

# Campaign of record: one scenario exhausts its own reachable state space
# (64 591 states for the 30 s reference), so the spec's one-million-state
# "complet" level is reached by summing several scenarios. Long: never on a PR.
campaign: all
	./scripts/explore-campaign.sh --scenarios 12 --jobs $(JOBS) --out $(BUILD)/campaign

explore-full: all scenario
	$(BUILD)/ambre-explore --scenario $(BUILD)/scenario.txt \
	  --states 1000000 --jobs $(JOBS) --variants all --nested \
	  --report $(BUILD)/explore-full.json

crosscheck: all
	$(PYTHON) host/verify/gen_vectors.py --scenarios 10000 --seed 4242 \
	  --out $(BUILD)/trigger_vectors.vec
	$(BUILD)/ambre-dump-truth --vectors $(BUILD)/trigger_vectors.vec \
	  > $(BUILD)/trigger_c.dec
	$(PYTHON) host/verify/trigger_crosscheck.py \
	  --vectors $(BUILD)/trigger_vectors.vec --c-output $(BUILD)/trigger_c.dec

mcap: all scenario
	$(PYTHON) host/scenarios/generate.py --seed 7 --duration-ms 30000 \
	  --crash-at-ms 20000 --out $(BUILD)/demo-scenario.txt
	$(BUILD)/ambre-explore --scenario $(BUILD)/demo-scenario.txt --states 1 \
	  --export-image $(BUILD)/journal.img
	$(PYTHON) host/mcap/export_mcap.py --image $(BUILD)/journal.img \
	  --out $(BUILD)/event.mcap

verify: mcap
	$(PYTHON) host/verify/edr_verify.py --mcap $(BUILD)/event.mcap \
	  --scenario $(BUILD)/demo-scenario.txt

demo: all
	./scripts/demo.sh

# --- level 2 on the PC backend: real processes, real files, real SIGKILL ----
# backend=posix: the "raw partition" is a file and the cut is a SIGKILL on
# edr-journal, NOT a QEMU/QNX power cut. What this proves and what it does not
# is written in host/e2e/README.md and host/processcut/README.md.
E2E_SCENARIO ?= $(BUILD)/scenario-e2e.txt
E2E_CUT      ?= acquired
E2E_CYCLES   ?= 200
E2E_JOBS     ?= 4

e2e-scenario:
	@mkdir -p $(BUILD)
	$(PYTHON) host/scenarios/generate.py --preset urban-frontal --seed 1234 \
	  --duration-ms 9000 --crash-at-ms 6000 --out $(E2E_SCENARIO)

# One end-to-end run with one cut: replay -> capture -> journal -> SIGKILL ->
# recovery -> list -> MCAP export -> verification.
e2e: tools e2e-scenario
	$(PYTHON) host/e2e/e2e_run.py --bins $(BUILD) --scenario $(E2E_SCENARIO) \
	  --cut-at $(E2E_CUT) --out $(BUILD)/e2e-report.json

# The cut campaign (spec 7.4 on this backend, spec 7.7 socle threshold:
# 200 cuts, zero loss). Sweep of 111 instants around the impact plus the four
# specification windows and random instants.
processcut: tools
	@mkdir -p $(BUILD)/processcut
	$(PYTHON) host/scenarios/generate.py --seed 1234 --duration-ms 6000 \
	  --crash-at-ms 3000 --out $(BUILD)/processcut/scenario.txt
	$(PYTHON) host/processcut/campaign.py --bins $(BUILD) \
	  --scenario $(BUILD)/processcut/scenario.txt --cycles $(E2E_CYCLES) \
	  --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 5 \
	  --jobs $(E2E_JOBS) \
	  --report $(BUILD)/processcut/report-$(E2E_CYCLES).json

sizecheck:
	@$(PYTHON) scripts/sizecheck.py --tracked-only

clean:
	rm -rf $(BUILD)

# --- quality targets (spec 12.10): coverage > 95%, random-data testing ------
# Appended by the quality agent. The targets above are untouched; `coverage`
# uses its own isolated build directory (never the shared $(BUILD)).
.PHONY: coverage coverage-gate fuzz fuzz-asan

COVBUILD ?= build-cov

coverage:
	@BUILD=$(COVBUILD) sh scripts/coverage.sh

coverage-gate:
	@BUILD=$(COVBUILD) COV_MIN=95 sh scripts/coverage.sh --list-uncovered

FUZZ_SRC := $(sort $(wildcard host/fuzz/*.c))

$(BUILD)/ambre-fuzz: $(FUZZ_SRC) $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(FUZZ_SRC) $(LIB) -o $@ $(LDFLAGS) -lm

fuzz: $(BUILD)/ambre-fuzz
	$(BUILD)/ambre-fuzz --quick

# ASan+UBSan build: every source the fuzzer links is recompiled with the
# sanitizers, and -fno-sanitize-recover=all turns any finding into a nonzero
# exit, which the fuzzer counts as a violation and saves as a regression.
FUZZ_SAN := -fsanitize=address,undefined -fno-sanitize-recover=all \
            -fno-omit-frame-pointer

$(BUILD)/ambre-fuzz-asan: $(FUZZ_SRC) $(FMT_SRC) $(HOST_SRC)
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O1 -g $(WARN) -Werror $(FUZZ_SAN) -Ifmt -I. -Itests \
	  -Ihost/simdisk -Ihost/explore -Itarget -Itarget/port -Itarget/ipc \
	  $(FUZZ_SRC) $(FMT_SRC) $(HOST_SRC) -o $@ $(LDFLAGS) -lm

fuzz-asan: $(BUILD)/ambre-fuzz-asan
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/ambre-fuzz-asan --quick

# --- latency campaign (spec 7.5) -------------------------------------------
# Software share measured, media share decomposed; every number carries
# backend=posix (docs/CAMPAIGN-LATENCY.md).  One crash is measured per
# journal boot, because the live journal holds 8 event slots.
#   make latency                 one campaign under CPU load
#   make latency LOAD=mem        one campaign under another load
#   make latency-all             the three loads of spec 7.5, plus the baseline
.PHONY: latency latency-all
LATENCY_LOAD    ?= $(or $(LOAD),cpu)
LATENCY_TRIGGERS ?= 200
LATENCY_SCENARIO = $(BUILD)/scenario-latency.txt

$(LATENCY_SCENARIO):
	$(PYTHON) host/latency/multi_shock.py --triggers 30 --cycle-ms 1000 \
	  --dv-kmh 9.0,10.8,12.6 --out $(LATENCY_SCENARIO)

latency: processes $(LATENCY_SCENARIO)
	$(PYTHON) host/latency/measure.py --bins $(BUILD) \
	  --scenario $(LATENCY_SCENARIO) --triggers $(LATENCY_TRIGGERS) \
	  --load $(LATENCY_LOAD) --report $(BUILD)/latency-$(LATENCY_LOAD).json

latency-all: processes $(LATENCY_SCENARIO)
	@for load in none cpu disk mem; do \
	  $(PYTHON) host/latency/measure.py --bins $(BUILD) \
	    --scenario $(LATENCY_SCENARIO) --triggers $(LATENCY_TRIGGERS) \
	    --load $$load --report $(BUILD)/latency-$$load.json || exit 1; \
	done
