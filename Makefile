# GPU Health Exporter — Makefile
#
# Flags:
#   WITH_TLS=1   Link mbedTLS, enable TLS in HTTP child
#   DEBUG=1      ASan + UBSan, debug symbols, no optimisation
#   PREFIX=/usr  Install prefix (default: /usr/local)

PREFIX  ?= /usr/local
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE
LDFLAGS := -ldl -lpthread -lm

ifeq ($(DEBUG),1)
    CFLAGS += -g -O0 -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
else
    CFLAGS += -O2
endif

ifeq ($(WITH_TLS),1)
    CFLAGS  += -DWITH_TLS
    LDFLAGS += -lmbedtls -lmbedcrypto -lmbedx509
endif

SRCDIR   := src
TESTDIR  := tests
BUILDDIR := build

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

# main.c is excluded from test builds — tests provide their own entry point
TEST_SRCS := $(filter-out $(SRCDIR)/main.c, $(SRCS))
TEST_OBJS := $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(TEST_SRCS))

BINARY := gpu-health-exporter

.PHONY: all test clean install

# Binary target only available once main.c exists; until then 'all' just compiles objects.
ifneq ($(wildcard $(SRCDIR)/main.c),)
all: $(BUILDDIR)/$(BINARY)

$(BUILDDIR)/$(BINARY): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
else
all: $(OBJS)
	@echo "NOTE: src/main.c not yet written — objects compiled, no binary linked."
endif

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ---- Tests ------------------------------------------------------------------
# Each test_*.c in tests/ is compiled with all src/ objects (minus main.c)
# and run immediately. All tests must pass for the target to succeed.

TEST_BINS := $(patsubst $(TESTDIR)/%.c, $(BUILDDIR)/%, $(wildcard $(TESTDIR)/test_*.c))

test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "==> $$t"; \
		$$t || failed=$$((failed + 1)); \
	done; \
	if [ $$failed -ne 0 ]; then \
		echo "FAILED: $$failed test(s)"; exit 1; \
	else \
		echo "ALL TESTS PASSED"; \
	fi

$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(TEST_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -I$(TESTDIR) -o $@ $^ $(LDFLAGS)

# ---- Install ----------------------------------------------------------------

install: $(BUILDDIR)/$(BINARY)
	install -Dm755 $(BUILDDIR)/$(BINARY) $(PREFIX)/bin/$(BINARY)
	install -Dm644 deploy/gpu-health.service /etc/systemd/system/gpu-health.service
	install -d /etc/gpu-health/baseline
	test -f /etc/gpu-health/gpu-health.conf || \
		install -Dm644 deploy/gpu-health.conf.example /etc/gpu-health/gpu-health.conf
	# Service account — idempotent: skip if already exists
	id -u gpu-health >/dev/null 2>&1 || \
		useradd --system --no-create-home --shell /usr/sbin/nologin gpu-health
	usermod -aG render gpu-health
	# Config readable only by root and the service account
	chown root:gpu-health /etc/gpu-health/gpu-health.conf
	chmod 0640 /etc/gpu-health/gpu-health.conf
	chown root:gpu-health /etc/gpu-health/baseline
	chmod 0750 /etc/gpu-health/baseline
	# Probe runner script and units (probe binary installed separately via probe/Makefile)
	install -Dm755 deploy/gpu-health-probe-run $(PREFIX)/bin/gpu-health-probe-run
	install -Dm644 deploy/gpu-health-probe.service /etc/systemd/system/gpu-health-probe.service
	install -Dm644 deploy/gpu-health-probe.timer /etc/systemd/system/gpu-health-probe.timer
ifeq ($(WITH_TLS),1)
	# TLS cert/key directory — readable only by root and the service account
	install -d /etc/gpu-health/tls
	chown root:gpu-health /etc/gpu-health/tls
	chmod 0750 /etc/gpu-health/tls
endif

# ---- Clean ------------------------------------------------------------------

clean:
	rm -rf $(BUILDDIR)
