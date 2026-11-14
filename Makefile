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
#   make web        the browser emulator: real sources -> WebAssembly (needs emcc)
#   make web-conformance  prove the browser module and the PC binary agree
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
# host tools linked into the PC library: the crash explorer and its support,
# plus the browser API (host/web/) that the website's WebAssembly build uses.
# host/explore/main.c stays out (it holds main()); it is linked by the CLI.
# host/web/wasm/main_wasm.c stays out too: it is Emscripten-only.
HOST_SRC := $(sort $(wildcard host/simdisk/*.c)) \
            $(sort $(filter-out %/main.c,$(wildcard host/explore/*.c))) \
            $(sort $(wildcard host/web/*.c))
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
        e2e e2e-scenario processcut web web-conformance qnx

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

# The browser emulator (site/emulator.html): the same C sources as the PC
# tools, compiled to WebAssembly by scripts/build-web.sh. Needs Emscripten, so
# it is never part of `make all` and never part of the 5-minute CI budget; the
# artefact is committed under site/assets/ and its sha256 next to it.
web:
	@bash scripts/build-web.sh

# Proof that what the browser runs is what the PC runs: identical states,
# identical counters, identical verdicts. Needs node and site/assets/ambre-emu.js.
web-conformance: all
	@node scripts/web-conformance.mjs --out $(BUILD)/web-conformance.json

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

# --- QNX 8.0 cross build with the real SDP ---------------------------------
# Source the SDP environment first, then build the five target binaries:
#   source ~/qnx800/qnxsdp-env.sh && make qnx
# Output: build/qnx/{edr-journal,edr-capture,edr-replay,edrctl,edr-archive}
# for nto x86_64 (override with QNX_VARIANT=gcc_ntoaarch64le).
#
# The *_qnx.c half of the portability layer REPLACES the POSIX half: no
# ambre_port_posix*.o is ever linked into a target binary, and no stub header
# is used here (the real $QNX_TARGET headers are).
QNX_CC       ?= qcc
QNX_CXX      ?= q++
QNX_VARIANT  ?= gcc_ntox86_64
QBUILD       := build/qnx
QNX_INC      := -Ifmt -I. -Itarget -Itarget/port -Itarget/ipc -Itarget/archive \
                -Itarget/capture -Itarget/journal -Itarget/ctl -Itarget/replay
QNX_CFLAGS   := -D_QNX_SOURCE -std=c11 -O2 -g $(WARN) -Werror -V$(QNX_VARIANT) $(QNX_INC)
QNX_CXXFLAGS := -D_QNX_SOURCE -std=c++17 -O2 -g -Wall -Wextra -Werror $(QNX_INC)
# slog2 lives in its own library on QNX 8.0 (libslog2.so); libm for the trigger
# arithmetic. No libsocket: VIE-03 keeps the recorder closed-loop.
QNX_LIBS     := -lm -lslog2

QNX_PORT_SRC   := target/port/ambre_port_qnx.c target/port/ambre_port_disk_qnx.c \
                  target/port/ambre_port_ipc_qnx.c
QNX_COMMON_SRC := $(FMT_SRC) $(IPC_SRC) $(QNX_PORT_SRC)
QNX_COMMON_OBJ := $(patsubst %.c,$(QBUILD)/%.o,$(QNX_COMMON_SRC))
# Les moitiés POSIX sont remplacées par leur équivalent `_qnx.c` : le filtre les
# exclut du lien de la cible, comme PORT_SRC le fait pour la couche portabilité.
# Sans ce filtre, `make qnx` lie les DEUX moitiés d'une même API (le lien
# redéfinit alors ambre_journal_serve_resmgr, mesuré le 2026-11-14).
QNX_DIR_OBJ     = $(patsubst %.c,$(QBUILD)/%.o,$(filter-out %_posix.c,$(wildcard target/$(1)/*.c)))
QNX_CPP_OBJ     = $(patsubst %.cpp,$(QBUILD)/%.o,$(wildcard target/$(1)/*.cpp))

QNX_BINS := $(QBUILD)/edr-journal $(QBUILD)/edr-capture $(QBUILD)/edr-replay \
            $(QBUILD)/edrctl $(QBUILD)/edr-archive

# Les deux sondes du banc : un VRAI client devctl() du journal (QNX-01, QNX-03,
# OBS-02, EXP-01, EXP-08) et la sonde d'intrusion de la politique noyau (SEC-01,
# SEC-02, SEC-03, VIE-03). Elles ne lient RIEN du port QNX (précédent
# name_probe.c) : elles se compilent seules, contre les vrais en-têtes de la SDP.
QNX_PROBES := $(QBUILD)/devctl_probe $(QBUILD)/secpol_probe

$(QBUILD)/devctl_probe: host/qnx_probe/devctl_probe.c
	@mkdir -p $(QBUILD)
	$(QNX_CC) $(QNX_CFLAGS) $< -o $@ $(QNX_LIBS)

$(QBUILD)/secpol_probe: host/qnx_probe/secpol_probe.c
	@mkdir -p $(QBUILD)
	$(QNX_CC) $(QNX_CFLAGS) $< -o $@ $(QNX_LIBS)

qnx-probes: $(QNX_PROBES)
	@for b in $(QNX_PROBES); do \
	  printf '  qnx %-13s %s\n' "$$(basename $$b)" \
	    "$$(file -b $$b | cut -d, -f1-2)"; done

# La politique de sécurité : `secpolcompile` est le test d'acceptation de
# image/ambre.sec (sa grammaire ne peut pas être vérifiée autrement). rc=0 veut
# dire « compilable », pas « appliquée » — l'application est démontrée sur cible
# par secpol_probe (docs/QNX-DEMO.md §Politique).
secpol: $(QBUILD)/secpol.bin
	@printf '  secpol %s (%s octets)\n' "$<" "$$(stat -c %s $<)"

$(QBUILD)/secpol.bin: image/ambre.sec
	@mkdir -p $(QBUILD)
	secpolcompile -o $@ $<

$(QBUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(QNX_CC) $(QNX_CFLAGS) -c $< -o $@

$(QBUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(QNX_CXX) $(QNX_CXXFLAGS) -c $< -o $@

qnx: $(QNX_BINS)
	@for b in $(QNX_BINS); do \
	  printf '  qnx %-13s %s\n' "$$(basename $$b)" \
	    "$$(file -b $$b | cut -d, -f1-2)"; done

$(QBUILD)/edr-journal: $(QNX_COMMON_OBJ) $(call QNX_DIR_OBJ,journal)
	@mkdir -p $(QBUILD)
	$(QNX_CC) $(QNX_CFLAGS) $^ -o $@ $(QNX_LIBS)

$(QBUILD)/edr-capture: $(QNX_COMMON_OBJ) $(call QNX_DIR_OBJ,capture)
	@mkdir -p $(QBUILD)
	$(QNX_CC) $(QNX_CFLAGS) $^ -o $@ $(QNX_LIBS)

$(QBUILD)/edr-replay: $(QNX_COMMON_OBJ) $(call QNX_DIR_OBJ,replay)
	@mkdir -p $(QBUILD)
	$(QNX_CC) $(QNX_CFLAGS) $^ -o $@ $(QNX_LIBS)

$(QBUILD)/edrctl: $(QNX_COMMON_OBJ) $(call QNX_DIR_OBJ,ctl)
	@mkdir -p $(QBUILD)
	$(QNX_CC) $(QNX_CFLAGS) $^ -o $@ $(QNX_LIBS)

$(QBUILD)/edr-archive: $(QNX_COMMON_OBJ) $(call QNX_CPP_OBJ,archive)
	@mkdir -p $(QBUILD)
	$(QNX_CXX) $(QNX_CXXFLAGS) $^ -o $@ $(QNX_LIBS)
