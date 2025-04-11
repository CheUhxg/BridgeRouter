CC = clang
CFLAGS = -emit-llvm
SRC = $(wildcard *.c)
LL_FILES = $(SRC:.c=.ll)
BC_FILES = $(SRC:.c=.bc)

all: $(LL_FILES) $(BC_FILES)

%.ll: %.c
	$(CC) $(CFLAGS) -S $< -o $@

%.bc: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(LL_FILES) $(BC_FILES)

.PHONY: all clean
