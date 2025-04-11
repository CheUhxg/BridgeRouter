#include "util.h"

struct Type ty;

void alloc_func1() {
    struct Type *pty;

    pty = kmalloc(100);
}

void alloc_func2() {  // not need
    ty.ptr = kmalloc(100);
}

void alloc_func3() {
    struct Type *pty;

    pty = kmalloc(100);
    pty->ptr = kmalloc(100);
}

void alloc_func4() {
    struct Type *pty;

    pty = kmalloc(100);
    pty->buf = kmalloc(100);
}