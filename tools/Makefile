# SPDX-License-Identifier: GPL-2.0-or-later
VMCU_SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD_DIR ?= $(VMCU_SRC)/../../../../../build/qbox-apollo-qvp/zephyr-vmcu/host
CC ?= cc
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -Wall -Wextra -Werror
SOURCES := $(VMCU_SRC)/lib/protocol.c $(VMCU_SRC)/lib/rpc.c

.PHONY: all test clean
all: $(BUILD_DIR)/vmcu-ap
$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/vmcu-ap: $(VMCU_SRC)/tools/vmcu-ap.c $(SOURCES) $(VMCU_SRC)/include/protocol.h $(VMCU_SRC)/include/rpc.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -I$(VMCU_SRC)/include $(filter %.c,$^) $(LDFLAGS) -o $@
$(BUILD_DIR)/protocol-test: $(VMCU_SRC)/tests/protocol-test.c $(SOURCES) $(wildcard $(VMCU_SRC)/include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -I$(VMCU_SRC)/include $(filter %.c,$^) $(LDFLAGS) -o $@
$(BUILD_DIR)/safety-test: $(VMCU_SRC)/tests/safety-test.c $(VMCU_SRC)/lib/safety.c $(VMCU_SRC)/lib/protocol.c $(wildcard $(VMCU_SRC)/include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -I$(VMCU_SRC)/include $(filter %.c,$^) $(LDFLAGS) -o $@
$(BUILD_DIR)/can-policy-test: $(VMCU_SRC)/tests/can-policy-test.c $(VMCU_SRC)/lib/can_policy.c $(VMCU_SRC)/include/can_policy.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -I$(VMCU_SRC)/include $(filter %.c,$^) $(LDFLAGS) -o $@
test: all $(BUILD_DIR)/protocol-test $(BUILD_DIR)/safety-test $(BUILD_DIR)/can-policy-test
	$(BUILD_DIR)/protocol-test
	$(BUILD_DIR)/safety-test
	$(BUILD_DIR)/can-policy-test
	python3 $(VMCU_SRC)/tests/test_peer.py $(BUILD_DIR)/vmcu-ap
clean:
	rm -f $(BUILD_DIR)/vmcu-ap $(BUILD_DIR)/protocol-test $(BUILD_DIR)/safety-test $(BUILD_DIR)/can-policy-test
