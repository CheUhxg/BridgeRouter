CC = gcc
CFLAGS = -Wno-sign-compare -Wno-nonnull -no-pie -static -masm=intel -pthread -I./libfuse
LDFLAGS = libfuse3.a

TARGET = exploit
SRCS = chain.c fakefuse.c exploit.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	gcc setup.c -o setup && sudo ./setup
	# sudo setcap cap_net_admin+ep ./$(TARGET)
	./$(TARGET)

clean:
	sudo umount bus || true
	sudo ip xfrm state flush
	sudo rm -rf bus evil setup hugetlb*
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean run
