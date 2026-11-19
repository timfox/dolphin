// Copyright 1998 Toby J Sargeant
// Copyright 1998-2004 Secret Labs AB
// Copyright 2010 Jeffrey 'Alex' Clark and contributors (Pillow)
// SPDX-License-Identifier: MIT-CMU

// Pillow's median-cut quantizer (libImaging/Quant.c, QuantHash.c and QuantHeap.c from
// Pillow 12.3.0), compiled as C++ so Image.quantize(method=MEDIANCUT) gives the same palette and
// indices here as in the Python GxBake tool. The algorithm is unchanged; only the allocation
// casts and the entry point (GxBake::MedianCutQuantize) are new.
//
// Pillow is licensed under the MIT-CMU license:
//
//   Permission to use, copy, modify and distribute this software and its documentation for any
//   purpose and without fee is hereby granted, provided that the above copyright notice appears
//   in all copies, and that both that copyright notice and this permission notice appear in
//   supporting documentation, and that the name of Secret Labs AB or the author not be used in
//   advertising or publicity pertaining to distribution of the software without specific,
//   written prior permission.
//
//   SECRET LABS AB AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
//   INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL SECRET
//   LABS AB OR THE AUTHOR BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY
//   DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
//   CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
//   OR PERFORMANCE OF THIS SOFTWARE.

#include "GxBake/PilQuant.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GxBake
{
namespace
{
// C's implicit conversion from void* for the allocations below.
struct Allocation
{
  void* p;
  template <typename T>
  operator T*() const
  {
    return static_cast<T*>(p);
  }
};
Allocation Malloc(size_t n)
{
  return {std::malloc(n)};
}
Allocation Calloc(size_t n, size_t size)
{
  return {std::calloc(n, size)};
}
#define malloc Malloc
#define calloc Calloc
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

// clang-format off
typedef union {
    struct {
        unsigned char r, g, b, a;
    } c;
    struct {
        unsigned char v[4];
    } a;
    uint32_t v;
} Pixel;

typedef struct _HashTable HashTable;
typedef Pixel HashKey_t;
typedef uint32_t HashVal_t;

typedef uint32_t (*HashFunc)(const HashTable *, const HashKey_t);
typedef int (*HashCmpFunc)(const HashTable *, const HashKey_t, const HashKey_t);
typedef void (*IteratorFunc)(
    const HashTable *, const HashKey_t, const HashVal_t, void *
);
typedef void (*IteratorUpdateFunc)(
    const HashTable *, const HashKey_t, HashVal_t *, void *
);
typedef void (*ComputeFunc)(const HashTable *, const HashKey_t, HashVal_t *);
typedef void (*CollisionFunc)(
    const HashTable *, HashKey_t *, HashVal_t *, HashKey_t, HashVal_t
);


typedef struct _Heap Heap;

typedef int (*HeapCmpFunc)(const Heap *, const void *, const void *);


typedef struct _HashNode {
    struct _HashNode *next;
    HashKey_t key;
    HashVal_t value;
} HashNode;

struct _HashTable {
    HashNode **table;
    uint32_t length;
    uint32_t count;
    HashFunc hashFunc;
    HashCmpFunc cmpFunc;
    void *userData;
};

#define MIN_LENGTH 11
#define RESIZE_FACTOR 3

static int
_hashtable_insert_node(HashTable *, HashNode *, int, int, CollisionFunc);

HashTable *
hashtable_new(HashFunc hf, HashCmpFunc cf) {
    HashTable *h;
    h = malloc(sizeof(HashTable));
    if (!h) {
        return NULL;
    }
    h->hashFunc = hf;
    h->cmpFunc = cf;
    h->length = MIN_LENGTH;
    h->count = 0;
    h->userData = NULL;
    h->table = malloc(sizeof(HashNode *) * h->length);
    if (!h->table) {
        free(h);
        return NULL;
    }
    memset(h->table, 0, sizeof(HashNode *) * h->length);
    return h;
}

static uint32_t
_findPrime(uint32_t start, int dir) {
    static int unit[] = {0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0};
    uint32_t t;
    while (start > 1) {
        if (!unit[start & 0x0f]) {
            start += dir;
            continue;
        }
        for (t = 2; t < sqrt((double)start); t++) {
            if (!start % t) {
                break;
            }
        }
        if (t >= sqrt((double)start)) {
            break;
        }
        start += dir;
    }
    return start;
}

static void
_hashtable_rehash(HashTable *h, CollisionFunc cf, uint32_t newSize) {
    HashNode **oldTable = h->table;
    uint32_t i;
    HashNode *n, *nn;
    uint32_t oldSize;
    oldSize = h->length;
    h->table = malloc(sizeof(HashNode *) * newSize);
    if (!h->table) {
        h->table = oldTable;
        return;
    }
    h->length = newSize;
    h->count = 0;
    memset(h->table, 0, sizeof(HashNode *) * h->length);
    for (i = 0; i < oldSize; i++) {
        for (n = oldTable[i]; n; n = nn) {
            nn = n->next;
            _hashtable_insert_node(h, n, 0, 0, cf);
        }
    }
    free(oldTable);
}

static void
_hashtable_resize(HashTable *h) {
    uint32_t newSize;
    uint32_t oldSize;
    oldSize = h->length;
    newSize = oldSize;
    if (h->count * RESIZE_FACTOR < h->length) {
        newSize = _findPrime(h->length / 2 - 1, -1);
    } else if (h->length * RESIZE_FACTOR < h->count) {
        newSize = _findPrime(h->length * 2 + 1, +1);
    }
    if (newSize < MIN_LENGTH) {
        newSize = oldSize;
    }
    if (newSize != oldSize) {
        _hashtable_rehash(h, NULL, newSize);
    }
}

static int
_hashtable_insert_node(
    HashTable *h, HashNode *node, int resize, int update, CollisionFunc cf
) {
    uint32_t hash = h->hashFunc(h, node->key) % h->length;
    HashNode **n, *nv;
    int i;

    for (n = &(h->table[hash]); *n; n = &((*n)->next)) {
        nv = *n;
        i = h->cmpFunc(h, nv->key, node->key);
        if (!i) {
            if (cf) {
                nv->key = node->key;
                cf(h, &(nv->key), &(nv->value), node->key, node->value);
                free(node);
                return 1;
            } else {
                nv->key = node->key;
                nv->value = node->value;
                free(node);
                return 1;
            }
        } else if (i > 0) {
            break;
        }
    }
    if (!update) {
        node->next = *n;
        *n = node;
        h->count++;
        if (resize) {
            _hashtable_resize(h);
        }
        return 1;
    } else {
        return 0;
    }
}

static int
_hashtable_insert(HashTable *h, HashKey_t key, HashVal_t val, int resize, int update) {
    HashNode **n, *nv;
    HashNode *t;
    int i;
    uint32_t hash = h->hashFunc(h, key) % h->length;

    for (n = &(h->table[hash]); *n; n = &((*n)->next)) {
        nv = *n;
        i = h->cmpFunc(h, nv->key, key);
        if (!i) {
            nv->value = val;
            return 1;
        } else if (i > 0) {
            break;
        }
    }
    if (!update) {
        t = malloc(sizeof(HashNode));
        if (!t) {
            return 0;
        }
        t->next = *n;
        *n = t;
        t->key = key;
        t->value = val;
        h->count++;
        if (resize) {
            _hashtable_resize(h);
        }
        return 1;
    } else {
        return 0;
    }
}

int
hashtable_insert_or_update_computed(
    HashTable *h, HashKey_t key, ComputeFunc newFunc, ComputeFunc existsFunc
) {
    HashNode **n, *nv;
    HashNode *t;
    int i;
    uint32_t hash = h->hashFunc(h, key) % h->length;

    for (n = &(h->table[hash]); *n; n = &((*n)->next)) {
        nv = *n;
        i = h->cmpFunc(h, nv->key, key);
        if (!i) {
            if (existsFunc) {
                existsFunc(h, nv->key, &(nv->value));
            } else {
                return 0;
            }
            return 1;
        } else if (i > 0) {
            break;
        }
    }
    t = malloc(sizeof(HashNode));
    if (!t) {
        return 0;
    }
    t->key = key;
    t->next = *n;
    *n = t;
    if (newFunc) {
        newFunc(h, t->key, &(t->value));
    } else {
        free(t);
        return 0;
    }
    h->count++;
    _hashtable_resize(h);
    return 1;
}

int
hashtable_insert(HashTable *h, HashKey_t key, HashVal_t val) {
    return _hashtable_insert(h, key, val, 1, 0);
}

void
hashtable_foreach_update(HashTable *h, IteratorUpdateFunc i, void *u) {
    HashNode *n;
    uint32_t x;

    if (h->table) {
        for (x = 0; x < h->length; x++) {
            for (n = h->table[x]; n; n = n->next) {
                i(h, n->key, &(n->value), u);
            }
        }
    }
}

void
hashtable_foreach(HashTable *h, IteratorFunc i, void *u) {
    HashNode *n;
    uint32_t x;

    if (h->table) {
        for (x = 0; x < h->length; x++) {
            for (n = h->table[x]; n; n = n->next) {
                i(h, n->key, n->value, u);
            }
        }
    }
}

void
hashtable_free(HashTable *h) {
    HashNode *n, *nn;
    uint32_t i;

    if (h->table) {
        for (i = 0; i < h->length; i++) {
            for (n = h->table[i]; n; n = nn) {
                nn = n->next;
                free(n);
            }
        }
        free(h->table);
    }
    free(h);
}

void
hashtable_rehash_compute(HashTable *h, CollisionFunc cf) {
    _hashtable_rehash(h, cf, h->length);
}

int
hashtable_lookup(const HashTable *h, const HashKey_t key, HashVal_t *valp) {
    uint32_t hash = h->hashFunc(h, key) % h->length;
    HashNode *n;
    int i;

    for (n = h->table[hash]; n; n = n->next) {
        i = h->cmpFunc(h, n->key, key);
        if (!i) {
            *valp = n->value;
            return 1;
        } else if (i > 0) {
            break;
        }
    }
    return 0;
}

uint32_t
hashtable_get_count(const HashTable *h) {
    return h->count;
}

void *
hashtable_get_user_data(const HashTable *h) {
    return h->userData;
}

void *
hashtable_set_user_data(HashTable *h, void *data) {
    void *r = h->userData;
    h->userData = data;
    return r;
}


struct _Heap {
    void **heap;
    unsigned int heapsize;
    unsigned int heapcount;
    HeapCmpFunc cf;
};

#define INITIAL_SIZE 256

// #define DEBUG

#ifdef DEBUG
static int
_heap_test(Heap *);
#endif

void
ImagingQuantHeapFree(Heap *h) {
    free(h->heap);
    free(h);
}

static int
_heap_grow(Heap *h, unsigned int newsize) {
    void *newheap;
    if (!newsize) {
        newsize = h->heapsize << 1;
    }
    if (newsize < h->heapsize) {
        return 0;
    }
    if (newsize > INT_MAX / sizeof(void *)) {
        return 0;
    }
    /* malloc check ok, using calloc for overflow, also checking
       above due to memcpy below*/
    newheap = calloc(newsize, sizeof(void *));
    if (!newheap) {
        return 0;
    }
    memcpy(newheap, h->heap, sizeof(void *) * h->heapsize);
    free(h->heap);
    h->heap = static_cast<void **>(newheap);
    h->heapsize = newsize;
    return 1;
}

#ifdef DEBUG
static int
_heap_test(Heap *h) {
    unsigned int k;
    for (k = 1; k * 2 <= h->heapcount; k++) {
        if (h->cf(h, h->heap[k], h->heap[k * 2]) < 0) {
            printf("heap is bad\n");
            return 0;
        }
        if (k * 2 + 1 <= h->heapcount && h->cf(h, h->heap[k], h->heap[k * 2 + 1]) < 0) {
            printf("heap is bad\n");
            return 0;
        }
    }
    return 1;
}
#endif

int
ImagingQuantHeapRemove(Heap *h, void **r) {
    unsigned int k, l;
    void *v;

    if (!h->heapcount) {
        return 0;
    }
    *r = h->heap[1];
    v = h->heap[h->heapcount--];
    for (k = 1; k * 2 <= h->heapcount; k = l) {
        l = k * 2;
        if (l < h->heapcount) {
            if (h->cf(h, h->heap[l], h->heap[l + 1]) < 0) {
                l++;
            }
        }
        if (h->cf(h, v, h->heap[l]) > 0) {
            break;
        }
        h->heap[k] = h->heap[l];
    }
    h->heap[k] = v;
#ifdef DEBUG
    if (!_heap_test(h)) {
        printf("oops - heap_remove messed up the heap\n");
        exit(1);
    }
#endif
    return 1;
}

int
ImagingQuantHeapAdd(Heap *h, void *val) {
    int k;
    if (h->heapcount == h->heapsize - 1) {
        _heap_grow(h, 0);
    }
    k = ++h->heapcount;
    while (k != 1) {
        if (h->cf(h, val, h->heap[k / 2]) <= 0) {
            break;
        }
        h->heap[k] = h->heap[k / 2];
        k >>= 1;
    }
    h->heap[k] = val;
#ifdef DEBUG
    if (!_heap_test(h)) {
        printf("oops - heap_add messed up the heap\n");
        exit(1);
    }
#endif
    return 1;
}

int
ImagingQuantHeapTop(Heap *h, void **r) {
    if (!h->heapcount) {
        return 0;
    }
    *r = h->heap[1];
    return 1;
}

Heap *
ImagingQuantHeapNew(HeapCmpFunc cf) {
    Heap *h;

    /* malloc check ok, small constant allocation */
    h = malloc(sizeof(Heap));
    if (!h) {
        return NULL;
    }
    h->heapsize = INITIAL_SIZE;
    /* malloc check ok, using calloc for overflow */
    h->heap = calloc(h->heapsize, sizeof(void *));
    if (!h->heap) {
        free(h);
        return NULL;
    }
    h->heapcount = 0;
    h->cf = cf;
    return h;
}

/* MSVC9.0 */
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffff
#endif

// #define DEBUG
// #define TEST_NEAREST_NEIGHBOUR

typedef struct {
    uint32_t scale;
} PixelHashData;

typedef struct _PixelList {
    struct _PixelList *next[3], *prev[3];
    Pixel p;
    unsigned int flag : 1;
    int count;
} PixelList;

typedef struct _BoxNode {
    struct _BoxNode *l, *r;
    PixelList *head[3], *tail[3];
    int axis;
    int volume;
    uint32_t pixelCount;
} BoxNode;

#define _SQR(x) ((x) * (x))
#define _DISTSQR(p1, p2)                            \
    _SQR((int)((p1)->c.r) - (int)((p2)->c.r)) +     \
        _SQR((int)((p1)->c.g) - (int)((p2)->c.g)) + \
        _SQR((int)((p1)->c.b) - (int)((p2)->c.b))

#define MAX_HASH_ENTRIES 65536

#define PIXEL_HASH(r, g, b)                                         \
    (((unsigned int)(r)) * 463 ^ ((unsigned int)(g) << 8) * 10069 ^ \
     ((unsigned int)(b) << 16) * 64997)

#define PIXEL_UNSCALE(p, q, s)                                  \
    ((q)->c.r = (p)->c.r << (s)), ((q)->c.g = (p)->c.g << (s)), \
        ((q)->c.b = (p)->c.b << (s))

#define PIXEL_SCALE(p, q, s)                                    \
    ((q)->c.r = (p)->c.r >> (s)), ((q)->c.g = (p)->c.g >> (s)), \
        ((q)->c.b = (p)->c.b >> (s))

static uint32_t
unshifted_pixel_hash(const HashTable *h, const Pixel pixel) {
    return PIXEL_HASH(pixel.c.r, pixel.c.g, pixel.c.b);
}

static int
unshifted_pixel_cmp(const HashTable *h, const Pixel pixel1, const Pixel pixel2) {
    if (pixel1.c.r == pixel2.c.r) {
        if (pixel1.c.g == pixel2.c.g) {
            if (pixel1.c.b == pixel2.c.b) {
                return 0;
            } else {
                return (int)(pixel1.c.b) - (int)(pixel2.c.b);
            }
        } else {
            return (int)(pixel1.c.g) - (int)(pixel2.c.g);
        }
    } else {
        return (int)(pixel1.c.r) - (int)(pixel2.c.r);
    }
}

static uint32_t
pixel_hash(const HashTable *h, const Pixel pixel) {
    PixelHashData *d = (PixelHashData *)hashtable_get_user_data(h);
    return PIXEL_HASH(
        pixel.c.r >> d->scale, pixel.c.g >> d->scale, pixel.c.b >> d->scale
    );
}

static int
pixel_cmp(const HashTable *h, const Pixel pixel1, const Pixel pixel2) {
    PixelHashData *d = (PixelHashData *)hashtable_get_user_data(h);
    uint32_t A, B;
    A = PIXEL_HASH(
        pixel1.c.r >> d->scale, pixel1.c.g >> d->scale, pixel1.c.b >> d->scale
    );
    B = PIXEL_HASH(
        pixel2.c.r >> d->scale, pixel2.c.g >> d->scale, pixel2.c.b >> d->scale
    );
    return (A == B) ? 0 : ((A < B) ? -1 : 1);
}

static void
exists_count_func(const HashTable *h, const Pixel key, uint32_t *val) {
    *val += 1;
}

static void
new_count_func(const HashTable *h, const Pixel key, uint32_t *val) {
    *val = 1;
}

static void
rehash_collide(
    const HashTable *h, Pixel *keyp, uint32_t *valp, Pixel newkey, uint32_t newval
) {
    *valp += newval;
}

/* %% */

static HashTable *
create_pixel_hash(Pixel *pixelData, uint32_t nPixels) {
    PixelHashData *d;
    HashTable *hash;
    uint32_t i;

    /* malloc check ok, small constant allocation */
    d = malloc(sizeof(PixelHashData));
    if (!d) {
        return NULL;
    }
    hash = hashtable_new(pixel_hash, pixel_cmp);
    hashtable_set_user_data(hash, d);
    d->scale = 0;
    for (i = 0; i < nPixels; i++) {
        if (!hashtable_insert_or_update_computed(
                hash, pixelData[i], new_count_func, exists_count_func
            )) {
            ;
        }
        while (hashtable_get_count(hash) > MAX_HASH_ENTRIES) {
            d->scale++;
            hashtable_rehash_compute(hash, rehash_collide);
        }
    }
    return hash;
}

static void
destroy_pixel_hash(HashTable *hash) {
    PixelHashData *d = (PixelHashData *)hashtable_get_user_data(hash);
    if (d) {
        free(d);
    }
    hashtable_free(hash);
}

/* 1. hash quantized pixels.                                                  */
/* 2. create R,G,B lists of sorted quantized pixels.                          */
/* 3. median cut.                                                             */
/* 4. build hash table from median cut boxes.                                 */
/* 5. for each pixel, compute entry in hash table, and hence median cut box.  */
/* 6. compute median cut box pixel averages.                                  */
/* 7. map each pixel to nearest average.                                      */

static int
compute_box_volume(BoxNode *b) {
    unsigned char rl, rh, gl, gh, bl, bh;
    if (b->volume >= 0) {
        return b->volume;
    }
    if (!b->head[0]) {
        b->volume = 0;
    } else {
        rh = b->head[0]->p.c.r;
        rl = b->tail[0]->p.c.r;
        gh = b->head[1]->p.c.g;
        gl = b->tail[1]->p.c.g;
        bh = b->head[2]->p.c.b;
        bl = b->tail[2]->p.c.b;
        b->volume = (rh - rl + 1) * (gh - gl + 1) * (bh - bl + 1);
    }
    return b->volume;
}

static void
hash_to_list(const HashTable *h, const Pixel pixel, const uint32_t count, void *u) {
    PixelHashData *d = (PixelHashData *)hashtable_get_user_data(h);
    PixelList **pl = (PixelList **)u;
    PixelList *p;
    int i;
    Pixel q;

    PIXEL_SCALE(&pixel, &q, d->scale);

    /* malloc check ok, small constant allocation */
    p = malloc(sizeof(PixelList));
    if (!p) {
        return;
    }

    p->flag = 0;
    p->p = q;
    p->count = count;
    for (i = 0; i < 3; i++) {
        p->next[i] = pl[i];
        p->prev[i] = NULL;
        if (pl[i]) {
            pl[i]->prev[i] = p;
        }
        pl[i] = p;
    }
}

static PixelList *
mergesort_pixels(PixelList *head, int i) {
    PixelList *c, *t, *a, *b, *p;
    if (!head || !head->next[i]) {
        if (head) {
            head->next[i] = NULL;
            head->prev[i] = NULL;
        }
        return head;
    }
    for (c = t = head; c && t;
         c = c->next[i], t = (t->next[i]) ? t->next[i]->next[i] : NULL);
    if (c) {
        if (c->prev[i]) {
            c->prev[i]->next[i] = NULL;
        }
        c->prev[i] = NULL;
    }
    a = mergesort_pixels(head, i);
    b = mergesort_pixels(c, i);
    head = NULL;
    p = NULL;
    while (a && b) {
        if (a->p.a.v[i] > b->p.a.v[i]) {
            c = a;
            a = a->next[i];
        } else {
            c = b;
            b = b->next[i];
        }
        c->prev[i] = p;
        c->next[i] = NULL;
        if (p) {
            p->next[i] = c;
        }
        p = c;
        if (!head) {
            head = c;
        }
    }
    if (a) {
        c->next[i] = a;
        a->prev[i] = c;
    } else if (b) {
        c->next[i] = b;
        b->prev[i] = c;
    }
    return head;
}


static int
box_heap_cmp(const Heap *h, const void *A, const void *B) {
    BoxNode *a = (BoxNode *)A;
    BoxNode *b = (BoxNode *)B;
    return (int)a->pixelCount - (int)b->pixelCount;
}

#define LUMINANCE(p) (77 * (p)->c.r + 150 * (p)->c.g + 29 * (p)->c.b)

static int
splitlists(
    PixelList *h[3],
    PixelList *t[3],
    PixelList *nh[2][3],
    PixelList *nt[2][3],
    uint32_t nCount[2],
    int axis,
    uint32_t pixelCount
) {
    uint32_t left;

    PixelList *l, *r, *c, *n;
    int i;
    int nRight;
    int splitColourVal;

    nCount[0] = nCount[1] = 0;
    nRight = 0;
    for (left = 0, c = h[axis]; c;) {
        left = left + c->count;
        nCount[0] += c->count;
        c->flag = 0;
        c = c->next[axis];
        if (left * 2 > pixelCount) {
            break;
        }
    }
    if (c) {
        splitColourVal = c->prev[axis]->p.a.v[axis];
        for (; c; c = c->next[axis]) {
            if (splitColourVal != c->p.a.v[axis]) {
                break;
            }
            c->flag = 0;
            nCount[0] += c->count;
        }
    }
    for (; c; c = c->next[axis]) {
        c->flag = 1;
        nRight++;
        nCount[1] += c->count;
    }
    if (!nRight) {
        for (c = t[axis], splitColourVal = t[axis]->p.a.v[axis]; c; c = c->prev[axis]) {
            if (splitColourVal != c->p.a.v[axis]) {
                break;
            }
            c->flag = 1;
            nRight++;
            nCount[0] -= c->count;
            nCount[1] += c->count;
        }
    }

    for (i = 0; i < 3; i++) {
        l = r = NULL;
        nh[0][i] = nt[0][i] = NULL;
        nh[1][i] = nt[1][i] = NULL;
        for (c = h[i]; c; c = n) {
            n = c->next[i];
            if (c->flag) { /* move pixel to right  list*/
                if (r) {
                    r->next[i] = c;
                } else {
                    nh[1][i] = c;
                }
                c->prev[i] = r;
                r = c;
            } else { /* move pixel to left list */
                if (l) {
                    l->next[i] = c;
                } else {
                    nh[0][i] = c;
                }
                c->prev[i] = l;
                l = c;
            }
        }
        if (l) {
            l->next[i] = NULL;
        }
        if (r) {
            r->next[i] = NULL;
        }
        nt[0][i] = l;
        nt[1][i] = r;
    }
    return 1;
}

static int
split(BoxNode *node) {
    unsigned char rl, rh, gl, gh, bl, bh;
    int f[3];
    int best, axis;
    int i;
    PixelList *heads[2][3];
    PixelList *tails[2][3];
    uint32_t newCounts[2];
    BoxNode *left, *right;

    rh = node->head[0]->p.c.r;
    rl = node->tail[0]->p.c.r;
    gh = node->head[1]->p.c.g;
    gl = node->tail[1]->p.c.g;
    bh = node->head[2]->p.c.b;
    bl = node->tail[2]->p.c.b;
    f[0] = (rh - rl) * 77;
    f[1] = (gh - gl) * 150;
    f[2] = (bh - bl) * 29;

    best = f[0];
    axis = 0;
    for (i = 1; i < 3; i++) {
        if (best < f[i]) {
            best = f[i];
            axis = i;
        }
    }
    node->axis = axis;
    if (!splitlists(
            node->head, node->tail, heads, tails, newCounts, axis, node->pixelCount
        )) {
        return 0;
    }
    /* malloc check ok, small constant allocation */
    left = malloc(sizeof(BoxNode));
    right = malloc(sizeof(BoxNode));
    if (!left || !right) {
        free(left);
        free(right);
        return 0;
    }
    for (i = 0; i < 3; i++) {
        left->head[i] = heads[0][i];
        left->tail[i] = tails[0][i];
        right->head[i] = heads[1][i];
        right->tail[i] = tails[1][i];
        node->head[i] = NULL;
        node->tail[i] = NULL;
    }
    left->l = left->r = NULL;
    right->l = right->r = NULL;
    left->axis = right->axis = -1;
    left->volume = right->volume = -1;
    left->pixelCount = newCounts[0];
    right->pixelCount = newCounts[1];
    node->l = left;
    node->r = right;
    return 1;
}

static BoxNode *
median_cut(PixelList *hl[3], uint32_t imPixelCount, int nPixels) {
    PixelList *tl[3];
    int i;
    BoxNode *root;
    Heap *h;
    BoxNode *thisNode;

    h = ImagingQuantHeapNew(box_heap_cmp);
    /* malloc check ok, small constant allocation */
    root = malloc(sizeof(BoxNode));
    if (!root) {
        ImagingQuantHeapFree(h);
        return NULL;
    }
    for (i = 0; i < 3; i++) {
        for (tl[i] = hl[i]; tl[i] && tl[i]->next[i]; tl[i] = tl[i]->next[i]);
        root->head[i] = hl[i];
        root->tail[i] = tl[i];
    }
    root->l = root->r = NULL;
    root->axis = -1;
    root->volume = -1;
    root->pixelCount = imPixelCount;

    ImagingQuantHeapAdd(h, (void *)root);
    while (--nPixels) {
        do {
            if (!ImagingQuantHeapRemove(h, (void **)&thisNode)) {
                goto done;
            }
        } while (compute_box_volume(thisNode) == 1);
        if (!split(thisNode)) {
            exit(1);
        }
        ImagingQuantHeapAdd(h, (void *)(thisNode->l));
        ImagingQuantHeapAdd(h, (void *)(thisNode->r));
    }
done:
    ImagingQuantHeapFree(h);
    return root;
}

static void
free_box_tree(BoxNode *n) {
    PixelList *p, *pp;
    if (n->l) {
        free_box_tree(n->l);
    }
    if (n->r) {
        free_box_tree(n->r);
    }
    for (p = n->head[0]; p; p = pp) {
        pp = p->next[0];
        free(p);
    }
    free(n);
}


static int
annotate_hash_table(BoxNode *n, HashTable *h, uint32_t *box) {
    PixelList *p;
    PixelHashData *d = (PixelHashData *)hashtable_get_user_data(h);
    Pixel q;
    if (n->l && n->r) {
        return annotate_hash_table(n->l, h, box) && annotate_hash_table(n->r, h, box);
    }
    if (n->l || n->r) {
        return 0;
    }
    for (p = n->head[0]; p; p = p->next[0]) {
        PIXEL_UNSCALE(&(p->p), &q, d->scale);
        if (!hashtable_insert(h, q, *box)) {
            return 0;
        }
    }
    if (n->head[0]) {
        (*box)++;
    }
    return 1;
}

typedef struct {
    uint32_t *distance;
    uint32_t index;
} DistanceWithIndex;

static int
_distance_index_cmp(const void *a, const void *b) {
    DistanceWithIndex *A = (DistanceWithIndex *)a;
    DistanceWithIndex *B = (DistanceWithIndex *)b;
    if (*A->distance == *B->distance) {
        return A->index < B->index ? -1 : +1;
    }
    return *A->distance < *B->distance ? -1 : +1;
}

static int
resort_distance_tables(
    uint32_t *avgDist, uint32_t **avgDistSortKey, Pixel *p, uint32_t nEntries
) {
    uint32_t i, j, k;
    uint32_t **skRow;
    uint32_t *skElt;

    for (i = 0; i < nEntries; i++) {
        avgDist[i * nEntries + i] = 0;
        for (j = 0; j < i; j++) {
            avgDist[j * nEntries + i] = avgDist[i * nEntries + j] =
                _DISTSQR(p + i, p + j);
        }
    }
    for (i = 0; i < nEntries; i++) {
        skRow = avgDistSortKey + i * nEntries;
        for (j = 1; j < nEntries; j++) {
            skElt = skRow[j];
            for (k = j; k && (*(skRow[k - 1]) > *(skRow[k])); k--) {
                skRow[k] = skRow[k - 1];
            }
            if (k != j) {
                skRow[k] = skElt;
            }
        }
    }
    return 1;
}

static int
build_distance_tables(
    uint32_t *avgDist, uint32_t **avgDistSortKey, Pixel *p, uint32_t nEntries
) {
    uint32_t i, j;
    DistanceWithIndex *dwi;

    for (i = 0; i < nEntries; i++) {
        avgDist[i * nEntries + i] = 0;
        avgDistSortKey[i * nEntries + i] = &(avgDist[i * nEntries + i]);
        for (j = 0; j < i; j++) {
            avgDist[j * nEntries + i] = avgDist[i * nEntries + j] =
                _DISTSQR(p + i, p + j);
            avgDistSortKey[j * nEntries + i] = &(avgDist[j * nEntries + i]);
            avgDistSortKey[i * nEntries + j] = &(avgDist[i * nEntries + j]);
        }
    }

    dwi = calloc(nEntries, sizeof(DistanceWithIndex));
    if (!dwi) {
        return 0;
    }
    for (i = 0; i < nEntries; i++) {
        for (j = 0; j < nEntries; j++) {
            dwi[j] = (DistanceWithIndex){&(avgDist[i * nEntries + j]), j};
        }
        qsort(dwi, nEntries, sizeof(DistanceWithIndex), _distance_index_cmp);
        for (j = 0; j < nEntries; j++) {
            avgDistSortKey[i * nEntries + j] = dwi[j].distance;
        }
    }
    free(dwi);
    return 1;
}

static int
map_image_pixels(
    Pixel *pixelData,
    uint32_t nPixels,
    Pixel *paletteData,
    uint32_t nPaletteEntries,
    uint32_t *avgDist,
    uint32_t **avgDistSortKey,
    uint32_t *pixelArray
) {
    uint32_t *aD, **aDSK;
    uint32_t idx;
    uint32_t i, j;
    uint32_t bestdist, bestmatch, dist;
    uint32_t initialdist;
    HashTable *h2;

    h2 = hashtable_new(unshifted_pixel_hash, unshifted_pixel_cmp);
    for (i = 0; i < nPixels; i++) {
        if (!hashtable_lookup(h2, pixelData[i], &bestmatch)) {
            bestmatch = 0;
            initialdist = _DISTSQR(paletteData + bestmatch, pixelData + i);
            bestdist = initialdist;
            initialdist <<= 2;
            aDSK = avgDistSortKey + bestmatch * nPaletteEntries;
            aD = avgDist + bestmatch * nPaletteEntries;
            for (j = 0; j < nPaletteEntries; j++) {
                idx = aDSK[j] - aD;
                if (*(aDSK[j]) <= initialdist) {
                    dist = _DISTSQR(paletteData + idx, pixelData + i);
                    if (dist < bestdist) {
                        bestdist = dist;
                        bestmatch = idx;
                    }
                } else {
                    break;
                }
            }
            hashtable_insert(h2, pixelData[i], bestmatch);
        }
        pixelArray[i] = bestmatch;
    }
    hashtable_free(h2);
    return 1;
}

static int
map_image_pixels_from_quantized_pixels(
    Pixel *pixelData,
    uint32_t nPixels,
    Pixel *paletteData,
    uint32_t nPaletteEntries,
    uint32_t *avgDist,
    uint32_t **avgDistSortKey,
    uint32_t *pixelArray,
    uint32_t *avg[3],
    uint32_t *count
) {
    uint32_t *aD, **aDSK;
    uint32_t idx;
    uint32_t i, j;
    uint32_t bestdist, bestmatch, dist;
    uint32_t initialdist;
    HashTable *h2;
    int changes = 0;

    h2 = hashtable_new(unshifted_pixel_hash, unshifted_pixel_cmp);
    for (i = 0; i < nPixels; i++) {
        if (!hashtable_lookup(h2, pixelData[i], &bestmatch)) {
            bestmatch = pixelArray[i];
            initialdist = _DISTSQR(paletteData + bestmatch, pixelData + i);
            bestdist = initialdist;
            initialdist <<= 2;
            aDSK = avgDistSortKey + bestmatch * nPaletteEntries;
            aD = avgDist + bestmatch * nPaletteEntries;
            for (j = 0; j < nPaletteEntries; j++) {
                idx = aDSK[j] - aD;
                if (*(aDSK[j]) <= initialdist) {
                    dist = _DISTSQR(paletteData + idx, pixelData + i);
                    if (dist < bestdist) {
                        bestdist = dist;
                        bestmatch = idx;
                    }
                } else {
                    break;
                }
            }
            hashtable_insert(h2, pixelData[i], bestmatch);
        }
        if (pixelArray[i] != bestmatch) {
            changes++;
            avg[0][bestmatch] += pixelData[i].c.r;
            avg[1][bestmatch] += pixelData[i].c.g;
            avg[2][bestmatch] += pixelData[i].c.b;
            avg[0][pixelArray[i]] -= pixelData[i].c.r;
            avg[1][pixelArray[i]] -= pixelData[i].c.g;
            avg[2][pixelArray[i]] -= pixelData[i].c.b;
            count[bestmatch]++;
            count[pixelArray[i]]--;
            pixelArray[i] = bestmatch;
        }
    }
    hashtable_free(h2);
    return changes;
}

static int
map_image_pixels_from_median_box(
    Pixel *pixelData,
    uint32_t nPixels,
    Pixel *paletteData,
    uint32_t nPaletteEntries,
    HashTable *medianBoxHash,
    uint32_t *avgDist,
    uint32_t **avgDistSortKey,
    uint32_t *pixelArray
) {
    uint32_t *aD, **aDSK;
    uint32_t idx;
    uint32_t i, j;
    uint32_t bestdist, bestmatch, dist;
    uint32_t initialdist;
    HashTable *h2;
    uint32_t pixelVal;

    h2 = hashtable_new(unshifted_pixel_hash, unshifted_pixel_cmp);
    for (i = 0; i < nPixels; i++) {
        if (hashtable_lookup(h2, pixelData[i], &pixelVal)) {
            pixelArray[i] = pixelVal;
            continue;
        }
        if (!hashtable_lookup(medianBoxHash, pixelData[i], &pixelVal)) {
            return 0;
        }
        initialdist = _DISTSQR(paletteData + pixelVal, pixelData + i);
        bestdist = initialdist;
        bestmatch = pixelVal;
        initialdist <<= 2;
        aDSK = avgDistSortKey + pixelVal * nPaletteEntries;
        aD = avgDist + pixelVal * nPaletteEntries;
        for (j = 0; j < nPaletteEntries; j++) {
            idx = aDSK[j] - aD;
            if (*(aDSK[j]) <= initialdist) {
                dist = _DISTSQR(paletteData + idx, pixelData + i);
                if (dist < bestdist) {
                    bestdist = dist;
                    bestmatch = idx;
                }
            } else {
                break;
            }
        }
        pixelArray[i] = bestmatch;
        hashtable_insert(h2, pixelData[i], bestmatch);
    }
    hashtable_free(h2);
    return 1;
}

static int
compute_palette_from_median_cut(
    Pixel *pixelData,
    uint32_t nPixels,
    HashTable *medianBoxHash,
    Pixel **palette,
    uint32_t nPaletteEntries,
    BoxNode *root
) {
    uint32_t i;
    uint32_t paletteEntry;
    Pixel *p;
    uint32_t *avg[3];
    uint32_t *count;

    *palette = NULL;
    /* malloc check ok, using calloc */
    if (!(count = calloc(nPaletteEntries, sizeof(uint32_t)))) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        avg[i] = NULL;
    }
    for (i = 0; i < 3; i++) {
        /* malloc check ok, using calloc */
        if (!(avg[i] = calloc(nPaletteEntries, sizeof(uint32_t)))) {
            for (i = 0; i < 3; i++) {
                if (avg[i]) {
                    free(avg[i]);
                }
            }
            free(count);
            return 0;
        }
    }
    for (i = 0; i < nPixels; i++) {
        if (!hashtable_lookup(medianBoxHash, pixelData[i], &paletteEntry)) {
            for (i = 0; i < 3; i++) {
                free(avg[i]);
            }
            free(count);
            return 0;
        }
        if (paletteEntry >= nPaletteEntries) {
            for (i = 0; i < 3; i++) {
                free(avg[i]);
            }
            free(count);
            return 0;
        }
        avg[0][paletteEntry] += pixelData[i].c.r;
        avg[1][paletteEntry] += pixelData[i].c.g;
        avg[2][paletteEntry] += pixelData[i].c.b;
        count[paletteEntry]++;
    }
    /* malloc check ok, using calloc */
    p = calloc(nPaletteEntries, sizeof(Pixel));
    if (!p) {
        for (i = 0; i < 3; i++) {
            free(avg[i]);
        }
        free(count);
        return 0;
    }
    for (i = 0; i < nPaletteEntries; i++) {
        p[i].c.r = (int)(.5 + (double)avg[0][i] / (double)count[i]);
        p[i].c.g = (int)(.5 + (double)avg[1][i] / (double)count[i]);
        p[i].c.b = (int)(.5 + (double)avg[2][i] / (double)count[i]);
    }
    *palette = p;
    for (i = 0; i < 3; i++) {
        free(avg[i]);
    }
    free(count);
    return 1;
}

static int
recompute_palette_from_averages(
    Pixel *palette, uint32_t nPaletteEntries, uint32_t *avg[3], uint32_t *count
) {
    uint32_t i;

    for (i = 0; i < nPaletteEntries; i++) {
        palette[i].c.r = (int)(.5 + (double)avg[0][i] / (double)count[i]);
        palette[i].c.g = (int)(.5 + (double)avg[1][i] / (double)count[i]);
        palette[i].c.b = (int)(.5 + (double)avg[2][i] / (double)count[i]);
    }
    return 1;
}

static int
compute_palette_from_quantized_pixels(
    Pixel *pixelData,
    uint32_t nPixels,
    Pixel *palette,
    uint32_t nPaletteEntries,
    uint32_t *avg[3],
    uint32_t *count,
    uint32_t *qp
) {
    uint32_t i;

    memset(count, 0, sizeof(uint32_t) * nPaletteEntries);
    for (i = 0; i < 3; i++) {
        memset(avg[i], 0, sizeof(uint32_t) * nPaletteEntries);
    }
    for (i = 0; i < nPixels; i++) {
        if (qp[i] >= nPaletteEntries) {
            return 0;
        }
        avg[0][qp[i]] += pixelData[i].c.r;
        avg[1][qp[i]] += pixelData[i].c.g;
        avg[2][qp[i]] += pixelData[i].c.b;
        count[qp[i]]++;
    }
    for (i = 0; i < nPaletteEntries; i++) {
        palette[i].c.r = (int)(.5 + (double)avg[0][i] / (double)count[i]);
        palette[i].c.g = (int)(.5 + (double)avg[1][i] / (double)count[i]);
        palette[i].c.b = (int)(.5 + (double)avg[2][i] / (double)count[i]);
    }
    return 1;
}

static int
k_means(
    Pixel *pixelData,
    uint32_t nPixels,
    Pixel *paletteData,
    uint32_t nPaletteEntries,
    uint32_t *qp,
    int threshold
) {
    uint32_t *avg[3];
    uint32_t *count;
    uint32_t i;
    uint32_t *avgDist;
    uint32_t **avgDistSortKey;
    int changes;
    int built = 0;

    if (nPaletteEntries > UINT32_MAX / (sizeof(uint32_t))) {
        return 0;
    }
    /* malloc check ok, using calloc */
    if (!(count = calloc(nPaletteEntries, sizeof(uint32_t)))) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        avg[i] = NULL;
    }
    for (i = 0; i < 3; i++) {
        /* malloc check ok, using calloc */
        if (!(avg[i] = calloc(nPaletteEntries, sizeof(uint32_t)))) {
            goto error_1;
        }
    }

    /* this is enough of a check, since the multiplication n*size is done above */
    if (nPaletteEntries > UINT32_MAX / nPaletteEntries) {
        goto error_1;
    }
    /* malloc check ok, using calloc, checking n*n above */
    avgDist = calloc(nPaletteEntries * nPaletteEntries, sizeof(uint32_t));
    if (!avgDist) {
        goto error_1;
    }

    /* malloc check ok, using calloc, checking n*n above */
    avgDistSortKey = calloc(nPaletteEntries * nPaletteEntries, sizeof(uint32_t *));
    if (!avgDistSortKey) {
        goto error_2;
    }

    while (1) {
        if (!built) {
            compute_palette_from_quantized_pixels(
                pixelData, nPixels, paletteData, nPaletteEntries, avg, count, qp
            );
            if (!build_distance_tables(
                    avgDist, avgDistSortKey, paletteData, nPaletteEntries
                )) {
                goto error_3;
            }
            built = 1;
        } else {
            recompute_palette_from_averages(paletteData, nPaletteEntries, avg, count);
            resort_distance_tables(
                avgDist, avgDistSortKey, paletteData, nPaletteEntries
            );
        }
        changes = map_image_pixels_from_quantized_pixels(
            pixelData,
            nPixels,
            paletteData,
            nPaletteEntries,
            avgDist,
            avgDistSortKey,
            qp,
            avg,
            count
        );
        if (changes < 0) {
            goto error_3;
        }
        if (changes <= threshold) {
            break;
        }
    }
    if (avgDistSortKey) {
        free(avgDistSortKey);
    }
    if (avgDist) {
        free(avgDist);
    }
    for (i = 0; i < 3; i++) {
        if (avg[i]) {
            free(avg[i]);
        }
    }
    if (count) {
        free(count);
    }
    return 1;

error_3:
    if (avgDistSortKey) {
        free(avgDistSortKey);
    }
error_2:
    if (avgDist) {
        free(avgDist);
    }
error_1:
    for (i = 0; i < 3; i++) {
        if (avg[i]) {
            free(avg[i]);
        }
    }
    if (count) {
        free(count);
    }
    return 0;
}

static int
quantize(
    Pixel *pixelData,
    uint32_t nPixels,
    uint32_t nQuantPixels,
    Pixel **palette,
    uint32_t *paletteLength,
    uint32_t **quantizedPixels,
    int kmeans
) {
    PixelList *hl[3];
    HashTable *h;
    BoxNode *root;
    uint32_t i;
    uint32_t *qp;
    uint32_t nPaletteEntries;

    uint32_t *avgDist;
    uint32_t **avgDistSortKey;
    Pixel *p;


    h = create_pixel_hash(pixelData, nPixels);
    if (!h) {
        goto error_0;
    }

    hl[0] = hl[1] = hl[2] = NULL;
    hashtable_foreach(h, hash_to_list, hl);

    if (!hl[0]) {
        goto error_1;
    }

    for (i = 0; i < 3; i++) {
        hl[i] = mergesort_pixels(hl[i], i);
    }

    root = median_cut(hl, nPixels, nQuantPixels);
    if (!root) {
        goto error_1;
    }
    nPaletteEntries = 0;
    if (!annotate_hash_table(root, h, &nPaletteEntries)) {
        goto error_3;
    }
    if (!compute_palette_from_median_cut(
            pixelData, nPixels, h, &p, nPaletteEntries, root
        )) {
        goto error_3;
    }

    free_box_tree(root);
    root = NULL;

    /* malloc check ok, using calloc for overflow */
    qp = calloc(nPixels, sizeof(uint32_t));
    if (!qp) {
        goto error_4;
    }

    if (nPaletteEntries > UINT32_MAX / nPaletteEntries) {
        goto error_5;
    }
    /* malloc check ok, using calloc for overflow, check of n*n above */
    avgDist = calloc(nPaletteEntries * nPaletteEntries, sizeof(uint32_t));
    if (!avgDist) {
        goto error_5;
    }

    /* malloc check ok, using calloc for overflow, check of n*n above */
    avgDistSortKey = calloc(nPaletteEntries * nPaletteEntries, sizeof(uint32_t *));
    if (!avgDistSortKey) {
        goto error_6;
    }

    if (!build_distance_tables(avgDist, avgDistSortKey, p, nPaletteEntries)) {
        goto error_7;
    }

    if (!map_image_pixels_from_median_box(
            pixelData, nPixels, p, nPaletteEntries, h, avgDist, avgDistSortKey, qp
        )) {
        goto error_7;
    }

    if (kmeans > 0) {
        k_means(pixelData, nPixels, p, nPaletteEntries, qp, kmeans - 1);
    }

    *quantizedPixels = qp;
    *palette = p;
    *paletteLength = nPaletteEntries;

    if (avgDist) {
        free(avgDist);
    }
    if (avgDistSortKey) {
        free(avgDistSortKey);
    }
    destroy_pixel_hash(h);
    return 1;

error_7:
    if (avgDistSortKey) {
        free(avgDistSortKey);
    }
error_6:
    if (avgDist) {
        free(avgDist);
    }
error_5:
    if (qp) {
        free(qp);
    }
error_4:
    if (p) {
        free(p);
    }
error_3:
    if (root) {
        free_box_tree(root);
    }
error_1:
    destroy_pixel_hash(h);
error_0:
    *quantizedPixels = NULL;
    *paletteLength = 0;
    *palette = NULL;
    return 0;
}

// clang-format on
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef malloc
#undef calloc
}  // namespace

bool MedianCutQuantize(const uint8_t* rgb, size_t count, int colors, std::vector<uint8_t>* indices,
                       std::vector<uint8_t>* palette)
{
  if (colors < 1 || colors > 256 || count == 0 || count > UINT32_MAX)
    return false;
  Pixel* pixels = static_cast<Pixel*>(calloc(count, sizeof(Pixel)));
  if (!pixels)
    return false;
  for (size_t i = 0; i < count; i++)
  {
    pixels[i].c.r = rgb[i * 3];
    pixels[i].c.g = rgb[i * 3 + 1];
    pixels[i].c.b = rgb[i * 3 + 2];
    pixels[i].c.a = 255;
  }
  Pixel* pal = nullptr;
  uint32_t pal_length = 0;
  uint32_t* quantized = nullptr;
  const int result = quantize(pixels, static_cast<uint32_t>(count), static_cast<uint32_t>(colors),
                              &pal, &pal_length, &quantized, 0);
  free(pixels);
  if (result <= 0)
    return false;
  indices->resize(count);
  for (size_t i = 0; i < count; i++)
    (*indices)[i] = static_cast<uint8_t>(quantized[i]);
  palette->clear();
  for (uint32_t i = 0; i < pal_length; i++)
  {
    palette->push_back(pal[i].c.r);
    palette->push_back(pal[i].c.g);
    palette->push_back(pal[i].c.b);
  }
  free(quantized);
  free(pal);
  return true;
}
}  // namespace GxBake
