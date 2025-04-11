#include "util.h"

struct Bridge *bridge_alloc(unsigned len) {
    struct Bridge *b;

    b = kmalloc(len);
    return b;
}

// good cases

void bridge_kern_local() {
    struct Bridge *b;
    void *buf;

    b = bridge_alloc(123);
    buf = get_heap_buf(134);
    memcpy(b->buf, buf, b->len);
}

void bridge_user_local() {
    struct Bridge *b;

    b = bridge_alloc(123);
    copy_from_user(b->buf, USER_SPACE, b->len);
}

void bridge_kern_remote() {
    struct Bridge *b;
    void *buf1, *buf2;

    b = bridge_alloc(123);
    buf1 = get_heap_buf(134);
    buf2 = get_heap_buf(134);
    memcpy(buf1, buf2, b->len);
}

void bridge_user_remote() {
    struct Bridge *b;
    void *buf;

    b = bridge_alloc(123);
    buf = kmalloc(134);
    copy_from_user(buf, USER_SPACE, b->len);
}

// bad cases

void bridge_stack_len() {
    struct Bridge b;
    void *buf1, *buf2;

    buf1 = get_heap_buf(134);
    buf2 = get_heap_buf(134);
    memcpy(buf1, buf2, b.len);
}

void bridge_stack_to() {
    struct Bridge *b;
    char to[123];
    char *from;

    b = bridge_alloc(123);
    from = get_heap_buf(134);
    memcpy(to, from, b->len);
}

void bridge_stack_from() {
    struct Bridge *b;
    char *to;
    char from[123];

    b = bridge_alloc(123);
    to = get_heap_buf(134);
    memcpy(to, from, b->len);
}