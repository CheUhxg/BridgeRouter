#include "util.h"

struct Router *router_alloc(unsigned len) {
    struct Router *r;

    r = kmalloc(len);
    r->buf = kmalloc(len*2);
    r->ptr = kmalloc(len*3);
    return r;
}

// good cases

void router_kern() {
    struct Router *r;

    r = router_alloc(1234);
    memcpy(r->buf, r->ptr, 1234);
}

void router_user() {
    struct Router *r;

    r = router_alloc(1234);
    copy_from_user(r->buf, USER_SPACE, 1234);
}

// bad cases

void router_kern_diff() {
    struct Router *r, *rb;

    r = router_alloc(1234);
    rb = router_alloc(1234);
    memcpy(r->buf, rb->ptr, 1234);
}

void router_user_stack() {
    struct Router r;

    r.buf = kmalloc(123);
    copy_from_user(r.buf, USER_SPACE, 1234);
}