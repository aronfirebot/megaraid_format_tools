# Build the MegaRAID format tools.
#
# Every tool is still a fully standalone, single-file C program - `make`
# (or `gcc -o mega_inquiry mega_inquiry.c`, per README.md) builds each one
# exactly as before. `make megaraid_tool` additionally links all of them into
# one multicall binary; see megaraid_tool.c for how that works.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

TOOLS   = mega_inquiry mega_format512 mega_modesel mega_format_immed mega_progress check_size
UNIFIED = megaraid_tool
UNIFIED_SRCS = megaraid_tool.c mega_inquiry.c mega_format512.c mega_modesel.c mega_format_immed.c mega_progress.c check_size.c
COMMON_HDR = megaraid_common.h

.PHONY: all tools unified test clean

all: tools unified

tools: $(TOOLS)

# Every tool includes megaraid_common.h, so it must rebuild when the header
# changes even though the header is never named on the compile command line.
$(TOOLS): %: %.c $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ $<

unified: $(UNIFIED)

$(UNIFIED): $(UNIFIED_SRCS) $(COMMON_HDR)
	$(CC) $(CFLAGS) -DMEGA_MULTICALL -o $@ $(UNIFIED_SRCS)

test:
	sh run_tests.sh

clean:
	rm -f $(TOOLS) $(UNIFIED) *.o test_parse_sense test_parse_sense_asan parse_sense.inc
	rm -rf *.dSYM
