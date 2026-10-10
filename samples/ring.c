/* ring.c: a fixed-size ring buffer of bytes and a small test.
   Build: cl /W4 ring.c */
#include <stdbool.h>
#include <stdio.h>

#define RING_SIZE 16u // a power of two

typedef struct Ring {
    unsigned char data[RING_SIZE];
    unsigned int head, tail;
} Ring;

static bool ring_push(Ring *r, unsigned char byte) {
    if (r->head - r->tail == RING_SIZE) return false; // full
    r->data[r->head++ & (RING_SIZE - 1)] = byte;
    return true;
}

static bool ring_pop(Ring *r, unsigned char *out) {
    if (r->head == r->tail) return false; // empty
    *out = r->data[r->tail++ & (RING_SIZE - 1)];
    return true;
}

int main(void) {
    Ring ring = { 0 };
    const char *text = "hello, ring\n";
    for (const char *p = text; *p; p++) {
        if (!ring_push(&ring, (unsigned char)*p)) {
            fprintf(stderr, "ring full at '%c'\n", *p);
            break;
        }
    }
    double fill = (ring.head - ring.tail) / (double)RING_SIZE;
    printf("fill %.2f, mask 0x%02X\n", fill, RING_SIZE - 1);
    unsigned char c;
    while (ring_pop(&ring, &c)) putchar(c);
    return 0;
}
