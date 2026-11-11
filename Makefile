# Ambre — QNX 8.0 Event Data Recorder
# Portable core: same C11 sources build for PC (host tools) and QNX 8.0.
#
#   make            build library, tests and tools
#   make test       run every unit/integration test (level 0)
#   make explore    exhaustive power-cut exploration, 10 000 states (level 1)
#   make explore-full  1 000 000 states (spec 7.7, "complet" level)
#   make crosscheck C trigger vs independent Python twin, 10 000 scenarios
#   make demo       one-command end-to-end demonstration
#   make sizecheck  house rule: files < 500 lines, functions < 50 lines
#   make clean

CC       ?= gcc
AR       ?= ar
PYTHON   ?= python3
BUILD    := build
JOBS     ?= $(shell nproc 2>/dev/null || echo 4)

WARN     := -Wall -Wextra -Wshadow -Wpedantic
CFLAGS   ?= -std=c11 -O2 -g $(WARN) -Werror -Ifmt -I. -Itests -Ihost/simdisk \
            -Ihost/explore -Itarget -Itarget/port -Itarget/ipc
LDFLAGS  ?=

FMT_SRC  := $(sort $(wildcard fmt/*.c))
# Portable half of the five target processes + the portability layer.
# *_qnx.c files are NOT part of the PC library: they are syntax-checked only
# (target `qnx-syntax`), because they include real QNX headers.
PORT_SRC := $(sort $(wildcard target/port/*.c))
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
PROCESS_BIN  := $(patsubst target/%/main.c,$(BUILD)/%,$(wildcard target/*/main.c))
TOOLS        := $(BUILD)/ambre-explore $(BUILD)/ambre-dump-truth $(PROCESS_BIN)

.PHONY: all test explore explore-full crosscheck demo sizecheck clean tools \
        scenario mcap verify help qnx-syntax processes

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

sizecheck:
	@$(PYTHON) scripts/sizecheck.py fmt host tests scripts

clean:
	rm -rf $(BUILD)
