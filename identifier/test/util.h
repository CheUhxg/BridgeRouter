struct Type {
    int len;
    int *ptr;
    void *buf;
};

struct Bridge {
    int len;
    char buf[];
};

struct Router {
    int *ptr;
    void *buf;
};

void *USER_SPACE;

void *kmalloc(unsigned size) {
    return &size;
}

void memcpy(void *to, void *from, unsigned size) {
    to = (char *)from + size;
}

void copy_from_user(void *to, void *from, unsigned size) {
    to = (char *)from + size;
}

void *get_heap_buf(unsigned len) {
    return kmalloc(len);
}
