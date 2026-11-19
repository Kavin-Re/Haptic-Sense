# SPDX-License-Identifier: Apache-2.0
#
# Host-side unit tests for the pure-logic parts of the Haptic-Sense firmware.
#
#   make            build and run every test
#   make SAN=0      same, without AddressSanitizer / UBSan
#   make clean
#
# Nothing here is cross-compiled and nothing under firmware/ is modified:
# the firmware sources are compiled as they are, against the stub headers in
# stubs/ where they need a kernel or HAL declaration.

CC      ?= gcc
SAN     ?= 1
BUILD   := build

FW_INC  := ../../firmware/Appli/Core/Inc
FW_SRC  := ../../firmware/Appli/Core/Src
HELDOUT := ../../data/held_out_test_vectors_20260918.csv

CFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Werror
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-sanitize-recover=undefined
endif

# app_tasks.c is included as-is and was written for the target's warning
# set: its task entry points ignore their arguments, and it compares a
# signed ring counter against an unsigned length (noted in the review).
FW_TU_WARN := -Wno-unused-parameter -Wno-sign-compare

TESTS := test_velocity_lsq test_hazard_classifier test_app_tasks
BINS  := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test clean
all: test

test: $(BINS)
	@set -e; \
	echo "== test_velocity_lsq";      $(BUILD)/test_velocity_lsq; \
	echo "== test_hazard_classifier"; $(BUILD)/test_hazard_classifier $(HELDOUT); \
	echo "== test_app_tasks";         $(BUILD)/test_app_tasks; \
	echo "ALL HOST TESTS PASSED"

$(BUILD):
	mkdir -p $@

$(BUILD)/test_velocity_lsq: test_velocity_lsq.c $(FW_SRC)/velocity_lsq.c $(FW_INC)/velocity_lsq.h minunit.h | $(BUILD)
	$(CC) $(CFLAGS) -I. -I$(FW_INC) test_velocity_lsq.c $(FW_SRC)/velocity_lsq.c -o $@

$(BUILD)/test_hazard_classifier: test_hazard_classifier.c $(FW_SRC)/app_hazard_classifier.c \
		$(FW_INC)/app_hazard_classifier.h $(FW_INC)/app_hazard_classifier_weights.h minunit.h | $(BUILD)
	$(CC) $(CFLAGS) -I. -I$(FW_INC) -I$(FW_SRC) test_hazard_classifier.c -o $@

# -DHAZARD_CLASSIFIER_USE_NPU: the shipped build configuration (.cproject).
$(BUILD)/test_app_tasks: test_app_tasks.c $(FW_SRC)/app_tasks.c $(FW_SRC)/velocity_lsq.c \
		$(wildcard stubs/*.h stubs/*/*.h) minunit.h | $(BUILD)
	$(CC) $(CFLAGS) $(FW_TU_WARN) -DHAZARD_CLASSIFIER_USE_NPU -Istubs -I. -I$(FW_INC) -I$(FW_SRC) \
		test_app_tasks.c $(FW_SRC)/velocity_lsq.c -o $@

clean:
	rm -rf $(BUILD)
