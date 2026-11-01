TARGET  := gokusaishiki

CC      := gcc
WARN    := -std=c11 -Wall -Wextra -Wpedantic
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 gio-2.0 gio-unix-2.0 libpipewire-0.3)
LDLIBS  := $(shell pkg-config --libs libusb-1.0 gio-2.0 gio-unix-2.0 libpipewire-0.3)

# Build mode: release (default) or debug
MODE ?= release
# Set SAN=1 to add ASan+UBSan to the debug build
SAN  ?= 0

ifeq ($(MODE),release)
  MODE_FLAGS := -O2
else ifeq ($(MODE),debug)
  MODE_FLAGS := -g3 -O0 -fno-omit-frame-pointer -fno-inline
  ifeq ($(SAN),1)
    MODE_FLAGS  += -fsanitize=address,undefined
    MODE_LDFLAGS := -fsanitize=address,undefined
  endif
else
  $(error Unknown MODE '$(MODE)' (use release or debug))
endif

CFLAGS  := $(WARN) $(MODE_FLAGS) -Iinclude $(LIBUSB_CFLAGS) 
LDFLAGS := $(MODE_LDFLAGS)

BUILD   := build/$(MODE)
BIN     := $(BUILD)/$(TARGET)

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=$(BUILD)/%.o)
DEP     := $(OBJ:.o=.d)

PREFIX  ?= /usr/local
ARGS    ?=
BREAK   ?= main
GDB     ?= gdb
GDBINIT ?= .gdbinit.local

.PHONY: all bin run debug gdb gdb-tui gdb-attach gdb-core clean install uninstall

all:
	$(MAKE) MODE=release bin

bin: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $@

run:
	build/release/gokusaishiki

# ---- Debug workflow (gdb) ----

# Debug build only
debug:
	$(MAKE) MODE=debug bin

# Build and start gdb, stopping at BREAK (default: main)
#   make gdb ARGS="-v"
#   make gdb BREAK=usb_open
gdb: debug
	$(GDB) -q $(if $(wildcard $(GDBINIT)),-x $(GDBINIT)) \
	       -ex 'break $(BREAK)' -ex run \
	       --args ./build/debug/$(TARGET) $(ARGS)

# Same, with gdb's TUI source view
gdb-tui: debug
	$(GDB) -q -tui $(if $(wildcard $(GDBINIT)),-x $(GDBINIT)) \
	       -ex 'break $(BREAK)' -ex run \
	       --args ./build/debug/$(TARGET) $(ARGS)

# Attach to an already-running instance:  make gdb-attach PID=1234
gdb-attach: debug
	@test -n "$(PID)" || { echo "usage: make gdb-attach PID=<pid>"; exit 1; }
	sudo $(GDB) -q ./build/debug/$(TARGET) -p $(PID)

# Post-mortem on a core file:  make gdb-core CORE=core
gdb-core: debug
	@test -n "$(CORE)" || { echo "usage: make gdb-core CORE=<corefile>"; exit 1; }
	$(GDB) -q ./build/debug/$(TARGET) $(CORE)

# ---- Housekeeping ----

clean:
	rm -rf build

install: all
	install -Dm755 build/release/$(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

-include $(DEP)