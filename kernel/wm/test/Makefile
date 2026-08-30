# Host test for the tiling window manager. Runs on the Mac, no hardware needed.
#   make        build and run
#   make san    rebuild under address+UB sanitizers and run
CC      ?= cc
CFLAGS  := -std=c99 -Wall -Wextra -O1 -g -I../include
SRC     := ../eos_wm.c test_wm.c

.PHONY: all san clean
all:
	@$(CC) $(CFLAGS) -o /tmp/eos_test_wm $(SRC) && /tmp/eos_test_wm

san:
	@$(CC) $(CFLAGS) -fsanitize=address,undefined -o /tmp/eos_test_wm_san $(SRC) && /tmp/eos_test_wm_san

clean:
	@rm -f /tmp/eos_test_wm /tmp/eos_test_wm_san
