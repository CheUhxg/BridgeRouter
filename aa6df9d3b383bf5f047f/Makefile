# 定义编译器
CC = gcc

# 定义编译选项
CFLAGS = -Wno-sign-compare -Wno-nonnull -no-pie -static -masm=intel -pthread -I./libfuse

# 定义链接选项
LDFLAGS = libfuse3.a

# 目标文件
TARGET = exploit

# 源文件列表
SRCS = chain.c fakefuse.c exploit.c

# 生成的目标文件列表
OBJS = $(SRCS:.c=.o)

# 默认目标
all: $(TARGET)

# 目标依赖于对象文件
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 编译各个源文件为对象文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 运行编译好的可执行文件
run: $(TARGET)
	gcc setup.c -o setup && sudo ./setup
	# sudo setcap cap_net_admin+ep ./$(TARGET)
	./$(TARGET)

# 清理编译生成的文件
clean:
	sudo umount bus || true
	sudo ip xfrm state flush
	sudo rm -rf bus evil setup hugetlb*
	rm -f $(TARGET) $(OBJS)

# 声明伪目标
.PHONY: all clean run
