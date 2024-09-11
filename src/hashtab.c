/* Copyright (c) 2024-present, Valkey contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Hashtab
 * =======
 *
 * This is an implementation of an open addressing hash table with cache-line
 * sized buckets. It's designed for speed and low memory overhead. It provides
 * lookups using a single memory access in most cases and it provides the
 * following features:
 *
 * - Incremental rehashing using two tables.
 *
 * - Stateless iteration using 'scan'.
 *
 * - A hash table contains pointer-sized elements rather than key-value entries.
 *   Using it as a set is strait-forward. Using it as a key-value store requires
 *   combining key and value in an object and inserting this object into the
 *   hash table. A callback for fetching the key from within the element is
 *   provided by the caller when creating the hash table.
 *
 * - The element type, key type, hash function and other properties are
 *   configurable as callbacks in a 'type' structure provided when creating a
 *   hash table.
 *
 * Conventions
 * -----------
 *
 * Functions and types are prefixed by "hashtab", macros by "HASHTAB". Internal
 * names don't use the prefix. Internal functions are 'static'.
 *
 * Credits
 * -------
 *
 * - The design of the cache-line aware open addressing scheme is inspired by
 *   tricks used in 'Swiss tables' (Sam Benzaquen, Alkis Evlogimenos, Matt
 *   Kulukundis, and Roman Perepelitsa et. al.).
 *
 * - The incremental rehashing using two tables, though for a chaining hash
 *   table, was designed by Salvatore Sanfilippo.
 *
 * - The original scan algorithm (for a chained hash table) was designed by
 *   Pieter Noordhuis.
 *
 * - The incremental rehashing and the scan algorithm were adapted for the open
 *   addressing scheme, including the use of linear probing by scan cursor
 *   increment, by Viktor Söderqvist. */
#include "hashtab.h"
#include "serverassert.h"
#include "zmalloc.h"
#include "mt19937-64.h"
#include "monotonic.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The default hashing function uses the SipHash implementation in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

/* --- Global variables --- */

static uint8_t hash_function_seed[16];
static hashtabResizePolicy resize_policy = HASHTAB_RESIZE_ALLOW;

/* --- Fill factor --- */

/* We use a soft and a hard limit for the minimum and maximum fill factor. The
 * hard limits are used when resizing should be avoided, according to the resize
 * policy. Resizing is typically to be avoided when we have forked child process
 * running. Then, we don't want to move too much memory around, since the fork
 * is using copy-on-write.
 *
 * With open addressing, the physical fill factor limit is 100% (probes the
 * whole table) so we may need to expand even if when it's preferred to avoid
 * it. Even if we resize and start inserting new elements in the new table, we
 * can avoid actively moving elements from the old table to the new table. When
 * the resize policy is AVOID, we perform a step of incremental rehashing only
 * on insertions and not on lookups. */

#define MAX_FILL_PERCENT_SOFT 77
#define MAX_FILL_PERCENT_HARD 90

#define MIN_FILL_PERCENT_SOFT 13
#define MIN_FILL_PERCENT_HARD 3

/* --- Hash function API --- */

/* The seed needs to be 16 bytes. */
void hashtabSetHashFunctionSeed(const uint8_t *seed) {
    memcpy(hash_function_seed, seed, sizeof(hash_function_seed));
}

uint8_t *hashtabGetHashFunctionSeed(void) {
    return hash_function_seed;
}

uint64_t hashtabGenHashFunction(const char *buf, size_t len) {
    return siphash((const uint8_t *)buf, len, hash_function_seed);
}

uint64_t hashtabGenCaseHashFunction(const char *buf, size_t len) {
    return siphash_nocase((const uint8_t *)buf, len, hash_function_seed);
}

/* --- Global resize policy API --- */

/* The global resize policy is one of
 *
 *   - HASHTAB_RESIZE_ALLOW: Rehash as required for optimal performance.
 *
 *   - HASHTAB_RESIZE_AVOID: Don't rehash and move memory if it can be avoided;
 *     used when there is a fork running and we want to avoid affecting
 *     copy-on-write memory.
 *
 *   - HASHTAB_RESIZE_FORBID: Don't rehash at all. Used in a child process which
 *     doesn't add any keys.
 *
 * Incremental rehashing works in the following way: A new table is allocated
 * and elements are incrementally moved from the old to the new table.
 *
 * To avoid affecting copy-on-write , we avoids rehashing when there is a forked
 * child process.
 *
 * With an open addressing scheme, we can't completely forbid resizing the table
 * if we want to be able to insert elements. It's impossible to insert more
 * elements than the number of slots, so we need to allow resizing even if the
 * resize policy is set to HASHTAB_RESIZE_AVOID, but we resize with incremental
 * rehashing paused, so new elements are added to the new table and the old
 * elements are rehashed only when the child process is done.
 *
 * This also means that we may need to resize even if rehashing is already
 * started and paused. In the worst case, we need to resize multiple times while
 * a child process is running. We fast-forward the rehashing in this case. */
void hashtabSetResizePolicy(hashtabResizePolicy policy) {
    resize_policy = policy;
}

/* --- Hash table layout --- */

#if SIZE_MAX == UINT64_MAX /* 64-bit version */

#define ELEMENTS_PER_BUCKET 7

/* Selecting the number of buckets.
 *
 * When resizing the table, we want to select an appropriate number of buckets
 * without an expensive division. Division by a power of two is cheap, but any
 * other division is expensive. We pick a fill factor to make division cheap for
 * our choice of ELEMENTS_PER_BUCKET.
 *
 * The number of buckets we want is NUM_ELEMENTS / (ELEMENTS_PER_BUCKET * FILL_FACTOR),
 * rounded up. The fill is the number of elements we have, or want to put, in
 * the table.
 *
 * Instead of the above fraction, we multiply by an integer BUCKET_FACTOR and
 * divide by a power-of-two BUCKET_DIVISOR. This gives us a fill factor of at
 * most MAX_FILL_PERCENT_SOFT, the soft limit for expanding.
 *
 *     NUM_BUCKETS = ceil(NUM_ELEMENTS * BUCKET_FACTOR / BUCKET_DIVISOR)
 *
 * This gives us
 *
 *     FILL_FACTOR = NUM_ELEMENTS / (NUM_BUCKETS * ELEMENTS_PER_BUCKET)
 *                 = 1 / (BUCKET_FACTOR / BUCKET_DIVISOR) / ELEMENTS_PER_BUCKET
 *                 = BUCKET_DIVISOR / BUCKET_FACTOR / ELEMENTS_PER_BUCKET
 */

#define BUCKET_FACTOR 3
#define BUCKET_DIVISOR 16
/* When resizing, we get a fill of at most 76.19% (16 / 3 / 7). */

#elif SIZE_MAX == UINT32_MAX /* 32-bit version */

#define ELEMENTS_PER_BUCKET 12
#define BUCKET_FACTOR 7
#define BUCKET_DIVISOR 64
/* When resizing, we get a fill of at most 76.19% (64 / 7 / 12). */

#else
#error "Only 64-bit or 32-bit architectures are supported"
#endif /* 64-bit vs 32-bit version */

#ifndef static_assert
#define static_assert _Static_assert
#endif

static_assert(100 * BUCKET_DIVISOR / BUCKET_FACTOR / ELEMENTS_PER_BUCKET <= MAX_FILL_PERCENT_SOFT,
              "Expand must result in a fill below the soft max fill factor");
static_assert(MAX_FILL_PERCENT_SOFT <= MAX_FILL_PERCENT_HARD, "Soft vs hard fill factor");
static_assert(MAX_FILL_PERCENT_HARD < 100, "Hard fill factor must be below 100%");

/* --- Random element --- */

#define FAIR_RANDOM_SAMPLE_SIZE (ELEMENTS_PER_BUCKET * 40)
#define WEAK_RANDOM_SAMPLE_SIZE ELEMENTS_PER_BUCKET

/* If size_t is 64 bits, use a 64 bit PRNG. */
#if SIZE_MAX >= 0xffffffffffffffff
#define randomSizeT() ((size_t)genrand64_int64())
#else
#define randomSizeT() ((size_t)random())
#endif

/* --- Types --- */

/* Open addressing scheme
 * ----------------------
 *
 * We use an open addressing scheme, with buckets of 64 bytes (one cache line).
 * Each bucket contains metadata and element slots for a fixed number of
 * elements. In a 64-bit system, there are up to 7 elements per bucket. These
 * are unordered and an element can be inserted in any of the free slots.
 * Additionally, the bucket contains metadata for the elements. This includes a
 * few bits of the hash of the key of each element, which are used to rule out
 * false negatives when looking up elements.
 *
 * The bucket metadata contains a bit that is set if the bucket has ever been
 * full. This bit acts as a tombstone for the bucket and it's what we need to
 * know if probing the next bucket is necessary.
 *
 * Bucket layout, 64-bit version, 7 elements per bucket:
 *
 *     1 bit     7 bits    [1 byte] x 7  [8 bytes] x 7 = 64 bytes
 *     everfull  presence  hashes        elements
 *
 *     everfull: a shared tombstone; set if the bucket has ever been full
 *     presence: an bit per element slot indicating if an element present or not
 *     hashes: some bits of hash of each element to rule out false positives
 *     elements: the actual elements, typically pointers (pointer-sized)
 *
 * The 32-bit version has 12 elements and 19 unused bits per bucket:
 *
 *     1 bit     12 bits   3 bits  [1 byte] x 12  2 bytes  [4 bytes] x 12
 *     everfull  presence  unused  hashes         unused   elements
 */

#if ELEMENTS_PER_BUCKET < 8
#define BUCKET_BITS_TYPE uint8_t
#define BITS_NEEDED_TO_STORE_POS_WITHIN_BUCKET 3
#elif ELEMENTS_PER_BUCKET < 16
#define BUCKET_BITS_TYPE uint16_t
#define BITS_NEEDED_TO_STORE_POS_WITHIN_BUCKET 4
#else
#error "Unexpected value of ELEMENTS_PER_BUCKET"
#endif

typedef struct {
    BUCKET_BITS_TYPE everfull : 1;
    BUCKET_BITS_TYPE presence : ELEMENTS_PER_BUCKET;
    uint8_t hashes[ELEMENTS_PER_BUCKET];
    void *elements[ELEMENTS_PER_BUCKET];
} bucket;

/* A key property is that the bucket size is one cache line. */
static_assert(sizeof(bucket) == 64, "Buckets need to be 64 bytes");

struct hashtab {
    hashtabType *type;
    ssize_t rehashIdx;       /* -1 = rehashing not in progress. */
    bucket *tables[2];       /* 0 = main table, 1 = rehashing target.  */
    size_t used[2];          /* Number of elements in each table. */
    int8_t bucketExp[2];     /* Exponent for num buckets (num = 1 << exp). */
    int16_t pauseRehash;     /* Non-zero = rehashing is paused */
    int16_t pauseAutoShrink; /* Non-zero = automatic resizing disallowed. */
    void *metadata[];
};

/* Struct for sampling elements using scan, used by random key functions. */

typedef struct {
    unsigned size;   /* Size of the elements array. */
    unsigned count;  /* Number of elements already sampled. */
    void **elements; /* Array of sampled elements. */
} scan_samples;

/* --- Internal functions --- */

static bucket *findBucketForInsert(hashtab *t, uint64_t hash, int *pos_in_bucket, int *table_index);

static inline void freeElement(hashtab *t, void *elem) {
    if (t->type->elementDestructor) t->type->elementDestructor(t, elem);
}

static inline int compareKeys(hashtab *t, const void *key1, const void *key2) {
    if (t->type->keyCompare != NULL) {
        return t->type->keyCompare(t, key1, key2);
    } else {
        return key1 != key2;
    }
}

static inline const void *elementGetKey(hashtab *t, const void *elem) {
    if (t->type->elementGetKey != NULL) {
        return t->type->elementGetKey(elem);
    } else {
        return elem;
    }
}

static inline uint64_t hashKey(hashtab *t, const void *key) {
    if (t->type->hashFunction != NULL) {
        return t->type->hashFunction(key);
    } else {
        return hashtabGenHashFunction((const char *)&key, sizeof(key));
    }
}

static inline uint64_t hashElement(hashtab *t, const void *elem) {
    return hashKey(t, elementGetKey(t, elem));
}


/* For the hash bits stored in the bucket, we use the highest bits of the hash
 * value, since these are not used for selecting the bucket. */
static inline uint8_t highBits(uint64_t hash) {
    return hash >> (CHAR_BIT * 7);
}

static inline int bucketIsFull(bucket *b) {
    return b->presence == (1 << ELEMENTS_PER_BUCKET) - 1;
}

static void resetTable(hashtab *t, int table_idx) {
    t->tables[table_idx] = NULL;
    t->used[table_idx] = 0;
    t->bucketExp[table_idx] = -1;
}

static inline size_t numBuckets(int exp) {
    return exp == -1 ? 0 : (size_t)1 << exp;
}

/* Bitmask for masking the hash value to get bucket index. */
static inline size_t expToMask(int exp) {
    return exp == -1 ? 0 : numBuckets(exp) - 1;
}

/* Returns the 'exp', where num_buckets = 1 << exp. The number of
 * buckets is a power of two. */
static signed char nextBucketExp(size_t min_capacity) {
    if (min_capacity == 0) return -1;
    /* ceil(x / y) = floor((x - 1) / y) + 1 */
    size_t min_buckets = (min_capacity * BUCKET_FACTOR - 1) / BUCKET_DIVISOR + 1;
    if (min_buckets >= SIZE_MAX / 2) return CHAR_BIT * sizeof(size_t) - 1;
    return CHAR_BIT * sizeof(size_t) - __builtin_clzl(min_buckets - 1);
}

/* Swaps the tables and frees the old table. */
static void rehashingCompleted(hashtab *t) {
    if (t->type->rehashingCompleted) t->type->rehashingCompleted(t);
    if (t->tables[0]) zfree(t->tables[0]);
    t->bucketExp[0] = t->bucketExp[1];
    t->tables[0] = t->tables[1];
    t->used[0] = t->used[1];
    resetTable(t, 1);
    t->rehashIdx = -1;
}

/* Reverse bits, adapted to use bswap, from
 * https://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static size_t rev(size_t v) {
#if SIZE_MAX == UINT64_MAX
    /* Swap odd and even bits. */
    v = ((v >> 1) & 0x5555555555555555) | ((v & 0x5555555555555555) << 1);
    /* Swap consecutive pairs. */
    v = ((v >> 2) & 0x3333333333333333) | ((v & 0x3333333333333333) << 2);
    /* Swap nibbles. */
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0F) | ((v & 0x0F0F0F0F0F0F0F0F) << 4);
    /* Reverse bytes. */
    v = __builtin_bswap64(v);
#else
    /* 32-bit version. */
    v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
    v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
    v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* Advances a scan cursor to the next value. It increments the reverse bit
 * representation of the masked bits of v. This algorithm was invented by Pieter
 * Noordhuis. */
size_t nextCursor(size_t v, size_t mask) {
    v |= ~mask; /* Set the unmasked (high) bits. */
    v = rev(v); /* Reverse. The unmasked bits are now the low bits. */
    v++;        /* Increment the reversed cursor, flipping the unmasked bits to
                 * 0 and increments the masked bits. */
    v = rev(v); /* Reverse the bits back to normal. */
    return v;
}

/* The reverse of nextCursor. */
static size_t prevCursor(size_t v, size_t mask) {
    v = rev(v);
    v--;
    v = rev(v);
    v = v & mask;
    return v;
}

/* Returns 1 if cursor A is less then cursor B, compared in cursor next/prev
 * order, 0 otherwise. This function can be used to compare bucket indexes in
 * probing order (since probing order is cursor order) and to check if a bucket
 * has already been rehashed, since incremental rehashing is also performed in
 * cursor order. */
static inline int cursorIsLessThan(size_t a, size_t b) {
    /* Since cursors are advanced in reversed-bits order, we can just reverse
     * both numbers to compare them. If a cursor with more bits than the other,
     * it is not significant, since the more significatnt bits become less
     * significant when reversing. */
    return rev(a) < rev(b);
}

/* Rehashes one bucket. */
static void rehashStep(hashtab *t) {
    assert(hashtabIsRehashing(t));
    size_t idx = t->rehashIdx;
    bucket *b = &t->tables[0][idx];
    int pos;
    for (pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
        if (!(b->presence & (1 << pos))) continue; /* empty */
        void *elem = b->elements[pos];
        uint8_t h2 = b->hashes[pos];
        /* Insert into table 1. */
        uint64_t hash;
        /* When shrinking, it's possible to avoid computing the hash. We can
         * just use idx has the hash, but only if we know that probing didn't
         * push this element away from its primary bucket, so only if the
         * bucket before the current one hasn't ever been full. */
        if (t->bucketExp[1] < t->bucketExp[0] && !t->tables[0][prevCursor(idx, expToMask(t->bucketExp[0]))].everfull) {
            hash = idx;
        } else {
            hash = hashElement(t, elem);
        }
        int pos_in_dst_bucket;
        bucket *dst = findBucketForInsert(t, hash, &pos_in_dst_bucket, NULL);
        dst->elements[pos_in_dst_bucket] = elem;
        dst->hashes[pos_in_dst_bucket] = h2;
        dst->presence |= (1 << pos_in_dst_bucket);
        dst->everfull |= bucketIsFull(dst);
        t->used[0]--;
        t->used[1]++;
    }
    /* Mark the source bucket as empty. */
    b->presence = 0;
    /* Bucket done. Advance to the next bucket in probing order, to cover
     * complete probing chains. Other alternatives are (1) just rehashIdx++ or
     * (2) in reverse scan order and clear the tombstones while doing so.
     * (Alternative is to do rehashIdx++.) */
    t->rehashIdx = nextCursor(t->rehashIdx, expToMask(t->bucketExp[0]));
    if (t->rehashIdx == 0) {
        rehashingCompleted(t);
    }
}

/* Called internally on lookup and other reads to the table. */
static inline void rehashStepOnReadIfNeeded(hashtab *t) {
    if (!hashtabIsRehashing(t) || t->pauseRehash) return;
    if (resize_policy != HASHTAB_RESIZE_ALLOW) return;
    rehashStep(t);
}

/* When inserting or deleting, we first do a find (read) and rehash one step if
 * resize policy is set to ALLOW, so here we only do it if resize policy is
 * AVOID. The reason for doing it on insert and delete is to ensure that we
 * finish rehashing before we need to resize the table again. */
static inline void rehashStepOnWriteIfNeeded(hashtab *t) {
    if (!hashtabIsRehashing(t) || t->pauseRehash) return;
    if (resize_policy != HASHTAB_RESIZE_AVOID) return;
    rehashStep(t);
}

/* Allocates a new table and initiates incremental rehashing if necessary.
 * Returns 1 on resize (success), 0 on no resize (failure). If 0 is returned and
 * 'malloc_failed' is provided, it is set to 1 if allocation failed. If
 * 'malloc_failed' is not provided, an allocation failure triggers a panic. */
static int resize(hashtab *t, size_t min_capacity, int *malloc_failed) {
    if (malloc_failed) *malloc_failed = 0;

    /* Adjust minimum size. We don't resize to zero currently. */
    if (min_capacity == 0) min_capacity = 1;

    /* Size of new table. */
    signed char exp = nextBucketExp(min_capacity);
    size_t num_buckets = numBuckets(exp);
    size_t new_capacity = num_buckets * ELEMENTS_PER_BUCKET;
    if (new_capacity < min_capacity || num_buckets * sizeof(bucket) < num_buckets) {
        /* Overflow */
        return 0;
    }
    signed char old_exp = t->bucketExp[hashtabIsRehashing(t) ? 1 : 0];
    if (exp == old_exp) {
        /* Can't resize to the same size. */
        return 0;
    }

    /* We can't resize if rehashing is already ongoing. Fast-forward ongoing
     * rehashing before we continue. */
    while (hashtabIsRehashing(t)) {
        rehashStep(t);
    }

    /* Allocate the new hash table. */
    bucket *new_table;
    if (malloc_failed) {
        new_table = ztrycalloc(num_buckets * sizeof(bucket));
        if (new_table == NULL) {
            *malloc_failed = 1;
            return 0;
        }
    } else {
        new_table = zcalloc(num_buckets * sizeof(bucket));
    }
    t->bucketExp[1] = exp;
    t->tables[1] = new_table;
    t->used[1] = 0;
    t->rehashIdx = 0;
    if (t->type->rehashingStarted) t->type->rehashingStarted(t);

    /* If the old table was empty, the rehashing is completed immediately. */
    if (t->tables[0] == NULL || t->used[0] == 0) {
        rehashingCompleted(t);
    } else if (t->type->instant_rehashing) {
        while (hashtabIsRehashing(t)) {
            rehashStep(t);
        }
    }
    return 1;
}

/* Returns 1 if the table is expanded, 0 if not expanded. If 0 is returned and
 * 'malloc_failed' is proveded, it is set to 1 if malloc failed and 0
 * otherwise. */
static int expand(hashtab *t, size_t size, int *malloc_failed) {
    if (size < hashtabSize(t)) {
        return 0;
    }
    return resize(t, size, malloc_failed);
}

/* Finds an element matching the key. If a match is found, returns a pointer to
 * the bucket containing the matching element and points 'pos_in_bucket' to the
 * index within the bucket. Returns NULL if no matching element was found.
 *
 * If 'table_index' is provided, it is set to the index of the table (0 or 1)
 * the returned bucket belongs to. */
static bucket *findBucket(hashtab *t, uint64_t hash, const void *key, int *pos_in_bucket, int *table_index) {
    if (hashtabSize(t) == 0) return 0;
    uint8_t h2 = highBits(hash);
    int table;

    /* Do some incremental rehashing. */
    rehashStepOnReadIfNeeded(t);

    /* Check rehashing destination table first, since it is newer and typically
     * has less 'everfull' flagged buckets. Therefore it needs less probing for
     * lookup. */
    for (table = 1; table >= 0; table--) {
        if (t->used[table] == 0) continue;
        size_t mask = expToMask(t->bucketExp[table]);
        size_t bucket_idx = hash & mask;
        while (1) {
            bucket *b = &t->tables[table][bucket_idx];
            /* Find candidate elements with presence flag set and matching h2 hash. */
            for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                if ((b->presence & (1 << pos)) && b->hashes[pos] == h2) {
                    /* It's a candidate. */
                    void *elem = b->elements[pos];
                    const void *elem_key = elementGetKey(t, elem);
                    if (compareKeys(t, key, elem_key) == 0) {
                        /* It's a match. */
                        if (pos_in_bucket) *pos_in_bucket = pos;
                        if (table_index) *table_index = table;
                        return b;
                    }
                }
            }

            /* Probe the next bucket? */
            if (!b->everfull) break;
            bucket_idx = nextCursor(bucket_idx, mask);
        }
    }
    return NULL;
}

/* Find an empty position in the table for inserting an element with the given hash. */
static bucket *findBucketForInsert(hashtab *t, uint64_t hash, int *pos_in_bucket, int *table_index) {
    int table = hashtabIsRehashing(t) ? 1 : 0;
    assert(t->tables[table]);
    size_t mask = expToMask(t->bucketExp[table]);
    size_t bucket_idx = hash & mask;
    while (1) {
        bucket *b = &t->tables[table][bucket_idx];
        for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
            if (b->presence & (1 << pos)) continue; /* busy */
            if (pos_in_bucket) *pos_in_bucket = pos;
            if (table_index) *table_index = table;
            return b;
        }
        bucket_idx = nextCursor(bucket_idx, mask);
    }
}

/* Encode bucket_index, pos_in_bucket, table_index into an opaque pointer. */
static void *encodePositionInTable(size_t bucket_index, int pos_in_bucket, int table_index) {
    uintptr_t encoded = bucket_index;
    encoded <<= BITS_NEEDED_TO_STORE_POS_WITHIN_BUCKET;
    encoded |= pos_in_bucket;
    encoded <<= 1;
    encoded |= table_index;
    encoded++; /* Add one to make sure we don't return NULL. */
    return (void *)encoded;
}

/* Decodes a position in the table encoded using encodePositionInTable(). */
static void decodePositionInTable(void *encoded_position, size_t *bucket_index, int *pos_in_bucket, int *table_index) {
    uintptr_t encoded = (uintptr_t)encoded_position;
    encoded--;
    *table_index = encoded & 1;
    encoded >>= 1;
    *pos_in_bucket = encoded & ((1 << BITS_NEEDED_TO_STORE_POS_WITHIN_BUCKET) - 1);
    encoded >>= BITS_NEEDED_TO_STORE_POS_WITHIN_BUCKET;
    *bucket_index = encoded;
}

/* Helper to insert an element. Doesn't check if an element with a matching key
 * already exists. This must be ensured by the caller. */
static void insert(hashtab *t, uint64_t hash, void *elem) {
    hashtabExpandIfNeeded(t);
    rehashStepOnWriteIfNeeded(t);
    int pos_in_bucket;
    int table_index;
    bucket *b = findBucketForInsert(t, hash, &pos_in_bucket, &table_index);
    b->elements[pos_in_bucket] = elem;
    b->presence |= (1 << pos_in_bucket);
    b->hashes[pos_in_bucket] = highBits(hash);
    b->everfull |= bucketIsFull(b);
    t->used[table_index]++;
}

/* A fingerprint of some of the state of the hash table. */
static uint64_t hashtabFingerprint(hashtab *t) {
    uint64_t integers[6], hash = 0;
    integers[0] = (uintptr_t)t->tables[0];
    integers[1] = t->bucketExp[0];
    integers[2] = t->used[0];
    integers[3] = (uintptr_t)t->tables[1];
    integers[4] = t->bucketExp[1];
    integers[5] = t->used[1];

    /* Result = hash(hash(hash(int1)+int2)+int3) */
    for (int j = 0; j < 6; j++) {
        hash += integers[j];
        /* Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); /* hash = (hash << 21) - hash - 1; */
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); /* hash * 265 */
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); /* hash * 21 */
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

/* Scan callback function used by hashtabGetSomeElements() for sampling elements
 * using scan. */
static void sampleElementsScanFn(void *privdata, void *element) {
    scan_samples *samples = privdata;
    if (samples->count < samples->size) {
        samples->elements[samples->count++] = element;
    }
}

/* --- API functions --- */

/* Allocates and initializes a new hashtable specified by the given type. */
hashtab *hashtabCreate(hashtabType *type) {
    size_t metasize = type->getMetadataSize ? type->getMetadataSize() : 0;
    hashtab *t = zmalloc(sizeof(*t) + metasize);
    if (metasize > 0) {
        memset(&t->metadata, 0, metasize);
    }
    t->type = type;
    t->rehashIdx = -1;
    t->pauseRehash = 0;
    t->pauseAutoShrink = 0;
    resetTable(t, 0);
    resetTable(t, 1);
    return t;
}

/* Deletes all the elements. If a callback is provided, it is called from time
 * to time to indicate progress. */
void hashtabEmpty(hashtab *t, void(callback)(hashtab *)) {
    if (hashtabIsRehashing(t)) {
        /* Pretend rehashing completed. */
        if (t->type->rehashingCompleted) t->type->rehashingCompleted(t);
        t->rehashIdx = -1;
    }
    for (int table_index = 0; table_index <= 1; table_index++) {
        if (t->bucketExp[table_index] < 0) {
            continue;
        }
        if (t->type->elementDestructor) {
            /* Call the destructor with each element. */
            for (size_t idx = 0; idx < numBuckets(t->bucketExp[table_index]); idx++) {
                if (callback && (idx & 65535) == 0) callback(t);
                bucket *b = &t->tables[table_index][idx];
                if (b->presence == 0) {
                    continue;
                }
                for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                    if (b->presence & (1 << pos)) {
                        t->type->elementDestructor(t, b->elements[pos]);
                    }
                }
            }
        }
        zfree(t->tables[table_index]);
        resetTable(t, table_index);
    }
}

/* Deletes all the elements and frees the table. */
void hashtabRelease(hashtab *t) {
    hashtabEmpty(t, NULL);
    zfree(t);
}

/* Returns the type of the hashtable. */
hashtabType *hashtabGetType(hashtab *t) {
    return t->type;
}

/* Returns a pointer to the table's metadata (userdata) section. */
void *hashtabMetadata(hashtab *t) {
    return &t->metadata;
}

/* Returns the number of elements stored. */
size_t hashtabSize(hashtab *t) {
    return t->used[0] + t->used[1];
}

/* Returns the size of the hashtab structures, in bytes (not including the sizes
 * of the elements, if the elements are pointers to allocated objects). */
size_t hashtabMemUsage(hashtab *t) {
    size_t num_buckets = numBuckets(t->bucketExp[0]) + numBuckets(t->bucketExp[1]);
    size_t metasize = t->type->getMetadataSize ? t->type->getMetadataSize() : 0;
    return sizeof(hashtab) + metasize + sizeof(bucket) * num_buckets;
}

/* Pauses automatic shrinking. This can be called before deleting a lot of
 * elements, to prevent automatic shrinking from being triggered multiple times.
 * Call hashtableResumeAutoShrink afterwards to restore automatic shrinking. */
void hashtabPauseAutoShrink(hashtab *t) {
    t->pauseAutoShrink++;
}

/* Re-enables automatic shrinking, after it has been paused. If you have deleted
 * many elements while automatic shrinking was paused, you may want to call
 * hashtabShrinkIfNeeded. */
void hashtabResumeAutoShrink(hashtab *t) {
    t->pauseAutoShrink--;
    if (t->pauseAutoShrink == 0) {
        hashtabShrinkIfNeeded(t);
    }
}

/* Pauses incremental rehashing. */
void hashtabPauseRehashing(hashtab *t) {
    t->pauseRehash++;
}

/* Resumes incremental rehashing, after pausing it. */
void hashtabResumeRehashing(hashtab *t) {
    t->pauseRehash--;
}

/* Returns 1 if incremental rehashing is paused, 0 if it isn't. */
int hashtabIsRehashingPaused(hashtab *t) {
    return t->pauseRehash > 0;
}

/* Returns 1 if incremental rehashing is in progress, 0 otherwise. */
int hashtabIsRehashing(hashtab *t) {
    return t->rehashIdx != -1;
}

/* Provides the old and new table size during rehashing. This function can only
 * be used when rehashing is in progress, and from the rehashingStarted and
 * rehashingCompleted callbacks. */
void hashtabRehashingInfo(hashtab *t, size_t *from_size, size_t *to_size) {
    assert(hashtabIsRehashing(t));
    *from_size = numBuckets(t->bucketExp[0]) * ELEMENTS_PER_BUCKET;
    *to_size = numBuckets(t->bucketExp[1]) * ELEMENTS_PER_BUCKET;
}

/* Return 1 if expand was performed; 0 otherwise. */
int hashtabExpand(hashtab *t, size_t size) {
    return expand(t, size, NULL);
}

/* Returns 1 if expand was performed or if expand is not needed. Returns 0 if
 * expand failed due to memory allocation failure. */
int hashtabTryExpand(hashtab *t, size_t size) {
    int malloc_failed = 0;
    return expand(t, size, &malloc_failed) || !malloc_failed;
}

/* Expanding is done automatically on insertion, but less eagerly if resize
 * policy is set to AVOID or FORBID. After restoring resize policy to ALLOW, you
 * may want to call hashtabExpandIfNeeded. Returns 1 if expanding, 0 if not
 * expanding. */
int hashtabExpandIfNeeded(hashtab *t) {
    size_t min_capacity = t->used[0] + t->used[1] + 1;
    size_t num_buckets = numBuckets(t->bucketExp[hashtabIsRehashing(t) ? 1 : 0]);
    size_t current_capacity = num_buckets * ELEMENTS_PER_BUCKET;
    unsigned max_fill_percent = resize_policy == HASHTAB_RESIZE_AVOID ? MAX_FILL_PERCENT_HARD : MAX_FILL_PERCENT_SOFT;
    if (min_capacity * 100 <= current_capacity * max_fill_percent) {
        return 0;
    }
    return resize(t, min_capacity, NULL);
}

/* Shrinking is done automatically on deletion, but less eagerly if resize
 * policy is set to AVOID and not at all if set to FORBID. After restoring
 * resize policy to ALLOW, you may want to call hashtabShrinkIfNeeded. */
int hashtabShrinkIfNeeded(hashtab *t) {
    /* Don't shrink if rehashing is already in progress. */
    if (hashtabIsRehashing(t) || resize_policy == HASHTAB_RESIZE_FORBID) {
        return 0;
    }
    size_t current_capacity = numBuckets(t->bucketExp[0]) * ELEMENTS_PER_BUCKET;
    unsigned min_fill_percent = resize_policy == HASHTAB_RESIZE_AVOID ? MIN_FILL_PERCENT_HARD : MIN_FILL_PERCENT_SOFT;
    if (t->used[0] * 100 > current_capacity * min_fill_percent) {
        return 0;
    }
    return resize(t, t->used[0], NULL);
}

/* Returns 1 if an element was found matching the key. Also points *found to it,
 * if found is provided. Returns 0 if no matching element was found. */
int hashtabFind(hashtab *t, const void *key, void **found) {
    if (hashtabSize(t) == 0) return 0;
    uint64_t hash = hashKey(t, key);
    int pos_in_bucket = 0;
    bucket *b = findBucket(t, hash, key, &pos_in_bucket, NULL);
    if (b) {
        if (found) *found = b->elements[pos_in_bucket];
        return 1;
    } else {
        return 0;
    }
}

/* Adds an element. Returns 1 on success. Returns 0 if there was already an element
 * with the same key. */
int hashtabAdd(hashtab *t, void *elem) {
    return hashtabAddOrFind(t, elem, NULL);
}

/* Adds an element and returns 1 on success. Returns 0 if there was already an
 * element with the same key and, if an 'existing' pointer is provided, it is
 * pointed to the existing element. */
int hashtabAddOrFind(hashtab *t, void *elem, void **existing) {
    const void *key = elementGetKey(t, elem);
    uint64_t hash = hashKey(t, key);
    int pos_in_bucket = 0;
    bucket *b = findBucket(t, hash, key, &pos_in_bucket, NULL);
    if (b != NULL) {
        if (existing) *existing = b->elements[pos_in_bucket];
        return 0;
    } else {
        insert(t, hash, elem);
        return 1;
    }
}

/* Finds and returns the position within the hashtab where an element with the
 * given key should be inserted using hashtabInsertAtPosition. This is the first
 * phase in a two-phase insert operation and it can be used if you want to avoid
 * creating an element before you know if it already exists in the table or not,
 * and without a separate lookup to the table.
 *
 * The returned pointer is opaque, but if it's NULL, it means that an element
 * with the given key already exists in the table.
 *
 * If a non-NULL pointer is returned, this pointer can be passed as the
 * 'position' argument to hashtabInsertAtPosition to insert an element. */
void *hashtabFindPositionForInsert(hashtab *t, void *key, void **existing) {
    uint64_t hash = hashKey(t, key);
    int pos_in_bucket, table_index;
    bucket *b = findBucket(t, hash, key, &pos_in_bucket, NULL);
    if (b != NULL) {
        if (existing) *existing = b->elements[pos_in_bucket];
        return NULL;
    } else {
        hashtabExpandIfNeeded(t);
        rehashStepOnWriteIfNeeded(t);
        b = findBucketForInsert(t, hash, &pos_in_bucket, &table_index);
        assert((b->presence & (1 << pos_in_bucket)) == 0);

        /* Store the hash bits now, so we don't need to compute the hash again
         * when hashtabInsertAtPosition() is called. */
        b->hashes[pos_in_bucket] = highBits(hash);

        /* Compute bucket index from bucket pointer. */
        void *b0 = &t->tables[table_index][0];
        size_t bucket_index = ((uintptr_t)b - (uintptr_t)b0) / sizeof(bucket);
        assert(&t->tables[table_index][bucket_index] == b);

        /* Encode position as pointer. */
        return encodePositionInTable(bucket_index, pos_in_bucket, table_index);
    }
}

/* Inserts an element at the position previously acquired using
 * hashtabFindPositionForInsert(). The element must match the key provided when
 * finding the position. You must not access the hashtab in any way between
 * hashtabFindPositionForInsert() and hashtabInsertAtPosition(), since even a
 * hashtabFind() may cause incremental rehashing to move elements in memory. */
void hashtabInsertAtPosition(hashtab *t, void *elem, void *position) {
    /* Decode position. */
    size_t bucket_index;
    int table_index, pos_in_bucket;
    decodePositionInTable(position, &bucket_index, &pos_in_bucket, &table_index);

    /* Insert the element at this position. */
    bucket *b = &t->tables[table_index][bucket_index];
    assert((b->presence & (1 << pos_in_bucket)) == 0);
    b->presence |= (1 << pos_in_bucket);
    b->elements[pos_in_bucket] = elem;
    /* Hash bits are already set by hashtabFindPositionForInsert. */
    b->everfull |= bucketIsFull(b);
    t->used[table_index]++;
}

/* Add or overwrite. Returns 1 if an new element was inserted, 0 if an existing
 * element was overwritten. */
int hashtabReplace(hashtab *t, void *elem) {
    const void *key = elementGetKey(t, elem);
    int pos_in_bucket = 0;
    uint64_t hash = hashKey(t, key);
    bucket *b = findBucket(t, hash, key, &pos_in_bucket, NULL);
    if (b != NULL) {
        freeElement(t, b->elements[pos_in_bucket]);
        b->elements[pos_in_bucket] = elem;
        return 0;
    } else {
        insert(t, hash, elem);
        return 1;
    }
}

/* Removes the element with the matching key and returns it. The element
 * destructor is not called. Returns 1 and points 'popped' to the element if a
 * matching element was found. Returns 0 if no matching element was found. */
int hashtabPop(hashtab *t, const void *key, void **popped) {
    if (hashtabSize(t) == 0) return 0;
    uint64_t hash = hashKey(t, key);
    int pos_in_bucket = 0;
    int table_index = 0;
    bucket *b = findBucket(t, hash, key, &pos_in_bucket, &table_index);
    if (b) {
        if (popped) *popped = b->elements[pos_in_bucket];
        b->presence &= ~(1 << pos_in_bucket);
        t->used[table_index]--;
        hashtabShrinkIfNeeded(t);
        return 1;
    } else {
        return 0;
    }
}

/* Deletes the element with the matching key. Returns 1 if an element was
 * deleted, 0 if no matching element was found. */
int hashtabDelete(hashtab *t, const void *key) {
    void *elem;
    if (hashtabPop(t, key, &elem)) {
        freeElement(t, elem);
        return 1;
    } else {
        return 0;
    }
}

/* Two-phase pop: Look up an element, do something with it, then delete it
 * without searching the hash table again.
 *
 * hashtabTwoPhasePopFind finds an element in the table and also the position of
 * the element within the table, so that it can be deleted without looking it up
 * in the table again. The function returns 1 if an element with a matching key
 * is found and 0 otherwise.
 *
 * If 1 is returned, call 'hashtabTwoPhasePopDelete' with the returned
 * 'position' afterwards to actually delete the element from the table. These
 * two functions are designed be used in pair. `hashtabTwoPhasePopFind` pauses
 * rehashing and `hashtabTwoPhasePopDelete` resumes rehashing.
 *
 * While hashtabPop finds and returns an element, the purpose of two-phase pop
 * is to provide an optimized equivalent of hashtabFind followed by
 * hashtabDelete, where the first call finds the element but doesn't delete it
 * from the hash table and the latter doesn't need to look up the element in the
 * hash table again.
 *
 * Example:
 *
 *     void *element, *position;
 *     if (hashtabTwoPhasePopFind(t, key, &element, &position) {
 *         // do something with the element, then...
 *         hashtabTwoPhasePopDelete(t, position);
 *     }
 */
int hashtabTwoPhasePopFind(hashtab *t, const void *key, void **found, void **position) {
    if (hashtabSize(t) == 0) return 0;
    uint64_t hash = hashKey(t, key);
    int pos_in_bucket = 0;
    int table_index = 0;
    bucket *b = findBucket(t, hash, key, &pos_in_bucket, &table_index);
    if (b) {
        hashtabPauseRehashing(t);
        *found = b->elements[pos_in_bucket];

        /* Compute bucket index from bucket pointer. */
        void *b0 = &t->tables[table_index][0];
        size_t bucket_index = ((uintptr_t)b - (uintptr_t)b0) / sizeof(bucket);
        assert(&t->tables[table_index][bucket_index] == b);

        /* Encode position as pointer. */
        *position = encodePositionInTable(bucket_index, pos_in_bucket, table_index);
        return 1;
    } else {
        return 0;
    }
}

/* Deletes the element at the opaque representation of its position, and resumes
 * rehashing. */
void hashtabTwoPhasePopDelete(hashtab *t, void *position) {
    /* Decode position. */
    size_t bucket_index;
    int table_index, pos_in_bucket;
    decodePositionInTable(position, &bucket_index, &pos_in_bucket, &table_index);

    /* Delete the element and resume rehashing. */
    bucket *b = &t->tables[table_index][bucket_index];
    assert(b->presence & (1 << pos_in_bucket));
    freeElement(t, b->elements[pos_in_bucket]);
    b->presence &= ~(1 << pos_in_bucket);
    t->used[table_index]--;
    hashtabShrinkIfNeeded(t);
    hashtabResumeRehashing(t);
}

/* --- Scan --- */

/* Scan is a stateless iterator. It works with a cursor that is returned to the
 * caller and which should be provided to the next call to continue scanning.
 * The hash table can be modified in any way between two scan calls. The scan
 * still continues iterating where it was.
 *
 * A full scan is performed like this: Start with a cursor of 0. The scan
 * callback is invoked for each element scanned and a new cursor is returned.
 * Next time, call this function with the new cursor. Continue until the
 * function returns 0.
 *
 * We say that an element is *emitted* when it's passed to the scan callback.
 *
 * Scan guarantees:
 *
 * - An element that is present in the hash table during an entire full scan
 *   will be returned (emitted) at least once. (Most of the time exactly once,
 *   but sometimes twice.)
 *
 * - An element that is inserted or deleted during a full scan may or may not be
 *   returned during the scan.
 *
 * The hash table uses a variant of linear probing with a cursor increment
 * rather than a regular increment of the index when probing. The scan algorithm
 * needs to continue scanning as long as a bucket in either of the tables has
 * ever been full. This means that we may wrap around cursor zero and still
 * continue until we find a bucket where we can stop, so some elements can be
 * returned twice (in the first and the last scan calls) due to this.
 *
 * The 'flags' argument can be used to tweak the behaviour. It's a bitwise-or
 * (zero means no flags) of the following:
 *
 * - HASHTAB_SCAN_EMIT_REF: Emit a pointer to the element's location in the
 *   table is passed to the scan function instead of the actual element. This
 *   can be used for advanced things like reallocating the memory of an element
 *   (for the purpose of defragmentation) and updating the pointer to the
 *   element inside the hash table.
 *
 * - HASHTAB_SCAN_SINGLE_STEP: This flag can be used for selecting fewer
 *   elements when the scan guarantees don't need to be enforced. With this
 *   flag, we don't continue scanning complete probing chains, so if rehashing
 *   happens between calls, elements can be missed. The scan cursor is advanced
 *   only a single step. */
size_t hashtabScan(hashtab *t, size_t cursor, hashtabScanFunction fn, void *privdata, int flags) {
    if (hashtabSize(t) == 0) return 0;

    /* Prevent elements from being moved around during the scan call, as a
     * side-effect of the scan callback. */
    hashtabPauseRehashing(t);

    /* Flags. */
    int emit_ref = (flags & HASHTAB_SCAN_EMIT_REF);
    int single_step = (flags & HASHTAB_SCAN_SINGLE_STEP);

    /* If any element that hashes to the current bucket may have been inserted
     * in another bucket due to probing, we need to continue to cover the whole
     * probe sequence in the same scan cycle. Otherwise we may miss those
     * elements if they are rehashed before the next scan call. */
    int in_probe_sequence = 1;

    /* When the cursor reaches zero, may need to continue scanning and advancing
     * the cursor until the probing chain ends, but when we stop, we return 0 to
     * indicate that the full scan is completed. */
    int cursor_passed_zero = 0;
    while (in_probe_sequence) {
        in_probe_sequence = 0; /* Set to 1 if an ever-full bucket is scanned. */
        if (!hashtabIsRehashing(t)) {
            /* Emit elements at the cursor index. */
            size_t mask = expToMask(t->bucketExp[0]);
            bucket *b = &t->tables[0][cursor & mask];
            int pos;
            for (pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                if (b->presence & (1 << pos)) {
                    void *emit = emit_ref ? &b->elements[pos] : b->elements[pos];
                    fn(privdata, emit);
                }
            }

            /* Do we need to continue scanning? */
            in_probe_sequence |= b->everfull;

            /* Advance cursor. */
            cursor = nextCursor(cursor, mask);
        } else {
            /* Let table0 be the the smaller table and table1 the bigger one. */
            int table0, table1;
            if (t->bucketExp[0] <= t->bucketExp[1]) {
                table0 = 0;
                table1 = 1;
            } else {
                table0 = 1;
                table1 = 0;
            }

            size_t mask0 = expToMask(t->bucketExp[table0]);
            size_t mask1 = expToMask(t->bucketExp[table1]);

            /* Emit elements in table 0, if this bucket hasn't already been rehashed. */
            if (!cursorIsLessThan(cursor, t->rehashIdx)) {
                bucket *b = &t->tables[0][cursor & mask0];
                for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                    if (b->presence & (1 << pos)) {
                        void *emit = emit_ref ? &b->elements[pos] : b->elements[pos];
                        fn(privdata, emit);
                    }
                }
                in_probe_sequence |= b->everfull;
            }

            /* Iterate over indices in larger table that are the expansion of
             * the index pointed to by the cursor in the smaller table. */
            do {
                /* Emit elements in table 1. */
                bucket *b = &t->tables[1][cursor & mask1];
                for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                    if (b->presence & (1 << pos)) {
                        void *emit = emit_ref ? &b->elements[pos] : b->elements[pos];
                        fn(privdata, emit);
                    }
                }
                in_probe_sequence |= b->everfull;

                /* Increment the reverse cursor not covered by the smaller mask.*/
                cursor = nextCursor(cursor, mask1);

                /* Continue while bits covered by mask difference is non-zero */
            } while (cursor & (mask0 ^ mask1));
        }
        if (cursor == 0) {
            cursor_passed_zero = 1;
        }
    }
    while (in_probe_sequence && !single_step);
    hashtabResumeRehashing(t);
    return cursor_passed_zero ? 0 : cursor;
}

/* --- Iterator --- */

/* Initiaize a iterator, that is not allowed to insert, delete or even lookup
 * elements in the hashtab, because such operations can trigger incremental
 * rehashing which moves elements around and confuses the iterator. Only
 * hashtabNext is allowed. Each element is returned exactly once. Call
 * hashtabResetIterator when you are done. See also hashtabInitSafeIterator. */
void hashtabInitIterator(hashtabIterator *iter, hashtab *t) {
    iter->t = t;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
}

/* Initialize a safe iterator, which is allowed to modify the hash table while
 * iterating. It pauses incremental rehashing to prevent elements from moving
 * around. Call hashtabNext to fetch each element. You must call
 * hashtabResetIterator when you are done with a safe iterator.
 *
 * Guarantees:
 *
 * - Elements that are in the hash table for the entire iteration are returned
 *   exactly once.
 *
 * - Elements that are deleted or replaced using hashtabReplace after they
 *   have been returned are not returned again.
 *
 * - Elements that are replaced using hashtabReplace before they've been
 *   returned by the iterator will be returned.
 *
 * - Elements that are inserted during the iteration may or may not be returned
 *   by the iterator.
 */
void hashtabInitSafeIterator(hashtabIterator *iter, hashtab *t) {
    hashtabInitIterator(iter, t);
    iter->safe = 1;
}

/* Resets a stack-allocated iterator. */
void hashtabResetIterator(hashtabIterator *iter) {
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe) {
            hashtabResumeRehashing(iter->t);
            assert(iter->t->pauseRehash >= 0);
        } else {
            assert(iter->fingerprint == hashtabFingerprint(iter->t));
        }
    }
}

/* Allocates and initializes an iterator. */
hashtabIterator *hashtabCreateIterator(hashtab *t) {
    hashtabIterator *iter = zmalloc(sizeof(*iter));
    hashtabInitIterator(iter, t);
    return iter;
}

/* Allocates and initializes a safe iterator. */
hashtabIterator *hashtabCreateSafeIterator(hashtab *t) {
    hashtabIterator *iter = hashtabCreateIterator(t);
    iter->safe = 1;
    return iter;
}

/* Resets and frees the memory of an allocated iterator, i.e. one created using
 * hashtabCreate(Safe)Iterator. */
void hashtabReleaseIterator(hashtabIterator *iter) {
    hashtabResetIterator(iter);
    zfree(iter);
}

/* Points elemptr to the next element and returns 1 if there is a next element.
 * Returns 0 if there are not more elements. */
int hashtabNext(hashtabIterator *iter, void **elemptr) {
    while (1) {
        if (iter->index == -1 && iter->table == 0) {
            /* It's the first call to next. */
            if (iter->safe) {
                hashtabPauseRehashing(iter->t);
            } else {
                iter->fingerprint = hashtabFingerprint(iter->t);
            }
            iter->index = 0;
            /* skip the rehashed slots in table[0] */
            if (hashtabIsRehashing(iter->t)) {
                iter->index = iter->t->rehashIdx;
            }
            iter->posInBucket = 0;
        } else {
            /* Advance position within bucket, or bucket index, or table. */
            iter->posInBucket++;
            if (iter->posInBucket >= ELEMENTS_PER_BUCKET) {
                iter->posInBucket = 0;
                iter->index++;
                if (iter->index >= (long)numBuckets(iter->t->bucketExp[iter->table])) {
                    iter->index = 0;
                    if (hashtabIsRehashing(iter->t) && iter->table == 0) {
                        iter->table++;
                    } else {
                        /* Done. */
                        break;
                    }
                }
            }
        }
        bucket *b = &iter->t->tables[iter->table][iter->index];
        if (!(b->presence & (1 << iter->posInBucket))) {
            /* No element here. Skip. */
            continue;
        }
        /* Return the element at this position. */
        if (elemptr) {
            *elemptr = b->elements[iter->posInBucket];
        }
        return 1;
    }
    return 0;
}

/* --- Random elements --- */

/* Points 'found' to a random element in the hash table and returns 1. Returns 0
 * if the table is empty. */
int hashtabRandomElement(hashtab *t, void **found) {
    void *samples[WEAK_RANDOM_SAMPLE_SIZE];
    unsigned count = hashtabSampleElements(t, (void **)&samples, WEAK_RANDOM_SAMPLE_SIZE);
    if (count == 0) return 0;
    unsigned idx = random() % count;
    *found = samples[idx];
    return 1;
}

/* Points 'found' to a random element in the hash table and returns 1. Returns 0
 * if the table is empty. This one is more fair than hashtabRandomElement(). */
int hashtabFairRandomElement(hashtab *t, void **found) {
    void *samples[FAIR_RANDOM_SAMPLE_SIZE];
    unsigned count = hashtabSampleElements(t, (void **)&samples, FAIR_RANDOM_SAMPLE_SIZE);
    if (count == 0) return 0;
    unsigned idx = random() % count;
    *found = samples[idx];
    return 1;
}

/* This function samples a sequence of elements starting at a random location in
 * the hash table.
 *
 * The sampled elements are stored in the array 'dst' which must have space for
 * at least 'count' elements.te
 *
 * The function returns the number of sampled elements, which is 'count' except
 * if 'count' is greater than the total number of elements in the hash table. */
unsigned hashtabSampleElements(hashtab *t, void **dst, unsigned count) {
    /* Adjust count. */
    if (count > hashtabSize(t)) count = hashtabSize(t);
    scan_samples samples;
    samples.size = count;
    samples.count = 0;
    samples.elements = dst;
    size_t cursor = randomSizeT();
    while (samples.count < count) {
        rehashStepOnReadIfNeeded(t);
        cursor = hashtabScan(t, cursor, sampleElementsScanFn, &samples, HASHTAB_SCAN_SINGLE_STEP);
    }
    return count;
}

/* --- Stats --- */

#define HASHTAB_STATS_VECTLEN 50
void hashtabFreeStats(hashtabStats *stats) {
    zfree(stats->clvector);
    zfree(stats);
}

void hashtabCombineStats(hashtabStats *from, hashtabStats *into) {
    into->buckets += from->buckets;
    into->maxChainLen = (from->maxChainLen > into->maxChainLen) ? from->maxChainLen : into->maxChainLen;
    into->totalChainLen += from->totalChainLen;
    into->htSize += from->htSize;
    into->htUsed += from->htUsed;
    for (int i = 0; i < HASHTAB_STATS_VECTLEN; i++) {
        into->clvector[i] += from->clvector[i];
    }
}

hashtabStats *hashtabGetStatsHt(hashtab *t, int htidx, int full) {
    unsigned long *clvector = zcalloc(sizeof(unsigned long) * HASHTAB_STATS_VECTLEN);
    hashtabStats *stats = zcalloc(sizeof(hashtabStats));
    stats->htidx = htidx;
    stats->clvector = clvector;
    stats->buckets = numBuckets(t->bucketExp[htidx]);
    stats->htSize = stats->buckets * ELEMENTS_PER_BUCKET;
    stats->htUsed = t->used[htidx];
    if (!full) return stats;
    /* Compute stats. */
    unsigned long chainlen = 0;
    for (unsigned long idx = 0; idx < stats->buckets; idx++) {
        bucket *b = &t->tables[htidx][idx];
        if (b->everfull) {
            stats->totalChainLen++;
            chainlen++;
        } else {
            /* End of a chain (even a zero-length chain). */
            /* Increment length index in chainlength vector. */
            clvector[(chainlen < HASHTAB_STATS_VECTLEN) ? chainlen : (HASHTAB_STATS_VECTLEN - 1)]++;
            if (chainlen > stats->maxChainLen) stats->maxChainLen = chainlen;
            chainlen = 0;
        }
    }
    return stats;
}

/* Generates human readable stats. */
size_t hashtabGetStatsMsg(char *buf, size_t bufsize, hashtabStats *stats, int full) {
    if (stats->htUsed == 0) {
        return snprintf(buf, bufsize,
                        "Hash table %d stats (%s):\n"
                        "No stats available for empty hash tables\n",
                        stats->htidx, (stats->htidx == 0) ? "main hash table" : "rehashing target");
    }
    size_t l = 0;
    l += snprintf(buf + l, bufsize - l,
                  "Hash table %d stats (%s):\n"
                  " table size: %lu\n"
                  " number of elements: %lu\n",
                  stats->htidx, (stats->htidx == 0) ? "main hash table" : "rehashing target", stats->htSize,
                  stats->htUsed);
    if (full) {
        l += snprintf(buf + l, bufsize - l,
                      " buckets: %lu\n"
                      " max chain length: %lu\n"
                      " avg chain length (counted): %.02f\n"
                      " avg chain length (computed): %.02f\n"
                      " Chain length distribution:\n",
                      stats->buckets, stats->maxChainLen, (float)stats->totalChainLen / stats->buckets,
                      (float)stats->htUsed / stats->buckets);

        for (unsigned long i = 0; i < HASHTAB_STATS_VECTLEN - 1; i++) {
            if (stats->clvector[i] == 0) continue;
            if (l >= bufsize) break;
            l += snprintf(buf + l, bufsize - l, "   %ld: %ld (%.02f%%)\n", i, stats->clvector[i],
                          ((float)stats->clvector[i] / stats->htSize) * 100);
        }
    }

    /* Make sure there is a NULL term at the end. */
    buf[bufsize - 1] = '\0';
    /* Unlike snprintf(), return the number of characters actually written. */
    return strlen(buf);
}

void hashtabGetStats(char *buf, size_t bufsize, hashtab *t, int full) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    hashtabStats *mainHtStats = hashtabGetStatsHt(t, 0, full);
    l = hashtabGetStatsMsg(buf, bufsize, mainHtStats, full);
    hashtabFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (hashtabIsRehashing(t) && bufsize > 0) {
        hashtabStats *rehashHtStats = hashtabGetStatsHt(t, 1, full);
        hashtabGetStatsMsg(buf, bufsize, rehashHtStats, full);
        hashtabFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    orig_buf[orig_bufsize - 1] = '\0';
}

/* --- DEBUG --- */

void hashtabDump(hashtab *t) {
    for (int table = 0; table <= 1; table++) {
        printf("Table %d, used %zu, exp %d\n", table, t->used[table], t->bucketExp[table]);
        for (size_t idx = 0; idx < numBuckets(t->bucketExp[table]); idx++) {
            bucket *b = &t->tables[table][idx];
            printf("Bucket %d:%zu everfull:%d\n", table, idx, b->everfull);
            for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                printf("  %d ", pos);
                if (b->presence & (1 << pos)) {
                    printf("h2 %02x, key \"%s\"\n", b->hashes[pos], (const char *)elementGetKey(t, b->elements[pos]));
                } else {
                    printf("(empty)\n");
                }
            }
        }
    }
}

void hashtabHistogram(hashtab *t) {
    for (int table = 0; table <= 1; table++) {
        for (size_t idx = 0; idx < numBuckets(t->bucketExp[table]); idx++) {
            bucket *b = &t->tables[table][idx];
            char c = b->presence == 0 && b->everfull ? 'X' : '0' + __builtin_popcount(b->presence);
            printf("%c", c);
        }
        if (table == 0) printf(" ");
    }
    printf("\n");
}

int hashtabLongestProbingChain(hashtab *t) {
    int maxlen = 0;
    for (int table = 0; table <= 1; table++) {
        if (t->bucketExp[table] < 0) {
            continue; /* table not used */
        }
        size_t cursor = 0;
        size_t mask = expToMask(t->bucketExp[table]);
        int chainlen = 0;
        do {
            assert(cursor <= mask);
            bucket *b = &t->tables[table][cursor];
            if (b->everfull) {
                if (++chainlen > maxlen) {
                    maxlen = chainlen;
                }
            } else {
                chainlen = 0;
            }
            cursor = nextCursor(cursor, mask);
        } while (cursor != 0);
    }
    return maxlen;
}