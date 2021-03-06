/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * arcus-memcached - Arcus memory cache server
 * Copyright 2010-2014 NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */
#include "config.h"

#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdarg.h>

#include "default_engine.h"

#define RESERVED_SLABS      4
#define RESERVED_SLAB_RATIO 4
#define MAX_SPACE_SHORTAGE_LEVEL 100

/* variable length slot management */
#define VARIABLE_LENGTH_SMMGR 1

#ifdef VARIABLE_LENGTH_SMMGR
/* should be computed manually */
#define SMMGR_NUM_CLASSES  1025
//#define SMMGR_NUM_CLASSES  81
//#define SMMGR_NUM_CLASSES  9
//#define SMMGR_FREE_MINSIZE  32
//#define SMMGR_NUM_CLASSES  8
//#define SMMGR_FREE_MINSIZE  64
//#define SMMGR_NUM_CLASSES  5
//#define SMMGR_FREE_MINSIZE  512

/* sm slot head */
typedef struct _sm_slot {
    uint32_t    status; /* 0: free slot */
    uint16_t    offset;
    uint16_t    length;
    struct _sm_slot *prev;
    struct _sm_slot *next;
} sm_slot_t;

/* sm slot tail */
typedef struct _sm_tail {
    uint16_t    offset;
    uint16_t    length; /* 0: free slot */
} sm_tail_t;

/* sm block */
typedef struct _sm_blck {
    struct _sm_blck *prev;
    struct _sm_blck *next;
    uint32_t    frspc; /* (NOT USED) free slot space in each block */
    uint32_t    frcnt; /* (NOT USED) free slot count in each block */
} sm_blck_t;

/* sm slot list */
typedef struct _sm_slist {
    sm_slot_t   *head;
    sm_slot_t   *tail;
    uint64_t    space;
    uint64_t    count;
} sm_slist_t;

/* sm block list */
typedef struct _sm_blist {
    sm_blck_t   *head;
    sm_blck_t   *tail;
    uint64_t    count;
} sm_blist_t;

typedef struct _sm_anchor {
    int         blck_clsid;         /* slab class id of block */
    int         blck_tsize;         /* block total size */
    int         blck_bsize;         /* block body size */
    int         used_num_classes;   /* # of used slot classes */
    int         free_num_classes;   /* # of free slot classes */
    int         used_minid;         /* min sm classid of used slots */
    int         used_maxid;         /* max sm classid of used slots */
    int         free_minid;         /* max sm classid of free slots */
    int         free_maxid;         /* max sm classid of free slots (excluding the largest free slot) */
    sm_blist_t  used_blist;         /* used block list */
    sm_slist_t  free_slist[SMMGR_NUM_CLASSES]; /* free slot list */
    sm_slist_t  used_slist[SMMGR_NUM_CLASSES]; /* used slot info */
    uint64_t    free_small_space;   /* the amount of free space that can't be used */
    uint64_t    free_avail_space;   /* the amount of free space that can be used */
    uint64_t    used_total_space;   /* the amount of used space */
} sm_anchor_t;

#define SMMGR_BLOCK_SIZE        (64*1024)
#define SMMGR_SLOT_SIZE(size)   (((((size)+sizeof(sm_tail_t)-1) / 8) + 1) * 8)
#define SMMGR_MIN_SLOT_SIZE     32
#define SMMGR_MAX_SLOT_SIZE     SMMGR_SLOT_SIZE(MAX_SM_VALUE_SIZE)

static sm_anchor_t sm_anchor;
#else
typedef struct _mem_chunk {
    uint32_t           total_item_count;
    uint32_t           free_item_count;
    struct _mem_chunk *prev;
    struct _mem_chunk *next;
    struct _mem_chunk *free_prev;
    struct _mem_chunk *free_next;
    void              *free_item;
} mem_chunk_t;

typedef struct _mem_anchor {
  uint32_t     total_chunk_count;
  uint32_t     free_chunk_count;
  uint32_t     chunk_item_size;
  uint32_t     items_per_chunk;
  uint32_t     items_free_count;
  uint32_t     short_of_space;
  mem_chunk_t *chunk_list;
  mem_chunk_t *chunk_free_head;
  mem_chunk_t *chunk_free_tail;
} mem_anchor_t;

#define SMMGR_ITEM_SIZE(size)   (((((size)+sizeof(uint16_t)-1) / 8) + 1) * 8)
#define SMMGR_MAX_ITEM_SIZE     SMMGR_ITEM_SIZE(MAX_SM_VALUE_SIZE)
#define SMMGR_MAX_ITEM_PHYIDX   65535
#define SMMGR_MAX_CLASS_COUNT   64 /* should be computed manually */

unsigned int smmgr_chunk_size;
unsigned int smmgr_chunk_clsid;

static unsigned int mem_class_count;
static unsigned int mem_lack_of_space_count;
static mem_anchor_t mem_anchor[SMMGR_MAX_CLASS_COUNT];
static unsigned int total_mem_chunk_count;
static unsigned int mem_nchunk_for_wideuse;
static unsigned int smslab_min_free_nchunk; /* minimum # of free chunks */
static int          smslab_cur_free_nchunk; /* current # of free chunks */
#endif

static EXTENSION_LOGGER_DESCRIPTOR *logger;

/*
 * Forward Declarations
 */
static void *do_slabs_alloc(struct default_engine *engine, const size_t size, unsigned int id);
static void  do_slabs_free(struct default_engine *engine, void *ptr, const size_t size, unsigned int id);
static int   do_slabs_newslab(struct default_engine *engine, const unsigned int id);
static void *memory_allocate(struct default_engine *engine, size_t size);

#ifndef DONT_PREALLOC_SLABS
/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate (const unsigned int maxslabs);
#endif

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(struct default_engine *engine, const size_t size)
{
    int res = POWER_SMALLEST;

    if (size == 0)
        return 0;
    while (size > engine->slabs.slabclass[res].size)
        if (res++ == engine->slabs.power_largest)     /* won't fit in the biggest slab */
            return 0;
    return res;
}

int slabs_short_of_free_space(struct default_engine *engine)
{
    if ((engine->slabs.mem_limit <= engine->slabs.mem_malloced) ||
        ((engine->slabs.mem_limit - engine->slabs.mem_malloced) < engine->slabs.mem_reserved))
    {
#ifdef VARIABLE_LENGTH_SMMGR
        if (engine->slabs.slabclass[sm_anchor.blck_clsid].slabs > 0) {
            slabclass_t *p = &engine->slabs.slabclass[sm_anchor.blck_clsid];
            size_t limit_nchunk = (p->rsvd_slabs * p->perslab * RESERVED_SLAB_RATIO) / 100;
            size_t avail_nchunk = (sm_anchor.free_avail_space/sm_anchor.blck_tsize) + (p->sl_curr+p->end_page_free)
                                + (p->slabs < p->rsvd_slabs ? ((p->rsvd_slabs-p->slabs) * p->perslab) : 0);
            if (avail_nchunk <= limit_nchunk) {
                int space_shortage_level;
                if (avail_nchunk <= 0) {
                    space_shortage_level = MAX_SPACE_SHORTAGE_LEVEL;
                } else {
                    space_shortage_level = limit_nchunk / avail_nchunk;
                    if (space_shortage_level == 1) {
                        space_shortage_level += (limit_nchunk-avail_nchunk) / (limit_nchunk/6);
                        /* space_shortage_level: 1 ~ 3 */
                    } else { /* space_shortage_level >= 2 */
                        space_shortage_level += 2;
                        if (space_shortage_level > MAX_SPACE_SHORTAGE_LEVEL)
                            space_shortage_level = MAX_SPACE_SHORTAGE_LEVEL;
                        /* space_shortage_level: 4 ~ MAX_SPACE_SHORTAGE_LEVEL */
                    }
                    /******
                    if (space_shortage_level > MAX_SPACE_SHORTAGE_LEVEL)
                        space_shortage_level = MAX_SPACE_SHORTAGE_LEVEL;
                    ******/
                }
                return space_shortage_level;
            }
        }
#else
        if (engine->slabs.slabclass[smmgr_chunk_clsid].slabs > 0) {
            slabclass_t *p = &engine->slabs.slabclass[smmgr_chunk_clsid];
            size_t cur_free_nchunk = p->sl_curr+p->end_page_free;
            if (p->slabs < p->rsvd_slabs)
                cur_free_nchunk += ((p->rsvd_slabs-p->slabs)*p->perslab);
            if (cur_free_nchunk == 0) return MAX_SPACE_SHORTAGE_LEVEL;
            if ((cur_free_nchunk < smslab_min_free_nchunk) &&
                (mem_lack_of_space_count > 0 || cur_free_nchunk < (smslab_min_free_nchunk/2))) {
                /* return the emergency level on short of space */
                int space_shortage_level = (int)(smslab_min_free_nchunk / cur_free_nchunk);
                return space_shortage_level < MAX_SPACE_SHORTAGE_LEVEL ? space_shortage_level : MAX_SPACE_SHORTAGE_LEVEL;
            }
        }
#endif
    }
    return 0;
}

static void do_smmgr_init(struct default_engine *engine)
{
#ifdef VARIABLE_LENGTH_SMMGR
    /* small memory allocator */
    sm_anchor.blck_clsid = 0;
    sm_anchor.blck_tsize = SMMGR_BLOCK_SIZE;
    sm_anchor.blck_bsize = sm_anchor.blck_tsize - sizeof(sm_blck_t);
    sm_anchor.used_num_classes = 0;
    sm_anchor.free_num_classes = 0;
    sm_anchor.used_minid = SMMGR_NUM_CLASSES;
    sm_anchor.used_maxid = -1;
    sm_anchor.free_minid = SMMGR_NUM_CLASSES;
    sm_anchor.free_maxid = -1;
    sm_anchor.used_blist.head = NULL;
    sm_anchor.used_blist.tail = NULL;
    sm_anchor.used_blist.count = 0;
    for (int i = 0; i < SMMGR_NUM_CLASSES; i++) {
        sm_anchor.free_slist[i].head = NULL;
        sm_anchor.free_slist[i].tail = NULL;
        sm_anchor.free_slist[i].space = 0;
        sm_anchor.free_slist[i].count = 0;
        sm_anchor.used_slist[i].head = NULL;
        sm_anchor.used_slist[i].tail = NULL;
        sm_anchor.used_slist[i].space = 0;
        sm_anchor.used_slist[i].count = 0;
    }
    sm_anchor.free_small_space = 0;
    sm_anchor.free_avail_space = 0;
    sm_anchor.used_total_space = 0;

    /* slab allocator */
    /* slab class 0 is used for collection items ans small-sized kv items */
    engine->slabs.slabclass[0].size = sm_anchor.blck_tsize;
    engine->slabs.slabclass[0].perslab = engine->config.item_size_max / engine->slabs.slabclass[0].size;
    engine->slabs.slabclass[0].rsvd_slabs = 0; // undefined
#else
    int item_size = 8;
    int diff_size = 8;

    /* slab class 0 is used for collection items ans small-sized kv items */
    smmgr_chunk_clsid = 0;
    smmgr_chunk_size  = (sizeof(mem_chunk_t)+(2*SMMGR_MAX_ITEM_SIZE));
    engine->slabs.slabclass[0].size = smmgr_chunk_size;
    engine->slabs.slabclass[0].perslab = engine->config.item_size_max / engine->slabs.slabclass[0].size;
    engine->slabs.slabclass[0].rsvd_slabs = 0; // undefined
    mem_nchunk_for_wideuse = (engine->slabs.slabclass[0].perslab * 10); /* 10MB */
    smslab_min_free_nchunk = 0;
    smslab_cur_free_nchunk = -1;

    memset(mem_anchor, 0, sizeof(mem_anchor));
    mem_class_count = 0;
    while (item_size < SMMGR_MAX_ITEM_SIZE) {
        mem_anchor[mem_class_count].chunk_item_size = item_size;
        mem_anchor[mem_class_count].items_per_chunk = (smmgr_chunk_size-sizeof(mem_chunk_t)) / item_size;
        mem_class_count++;
        if ((item_size / 16) == diff_size)
            diff_size *= 2;
        item_size += diff_size;
    }
    mem_anchor[mem_class_count].chunk_item_size = SMMGR_MAX_ITEM_SIZE;
    mem_anchor[mem_class_count].items_per_chunk = (smmgr_chunk_size-sizeof(mem_chunk_t)) / SMMGR_MAX_ITEM_SIZE;
    mem_class_count++;
    assert(mem_class_count == SMMGR_MAX_CLASS_COUNT);
    mem_lack_of_space_count = 0;

    total_mem_chunk_count = 0;
#endif
}

#ifdef VARIABLE_LENGTH_SMMGR
static inline int do_smmgr_memid(size_t size)
{
    if (size < 8192) return (int)(size/8);
    else             return SMMGR_NUM_CLASSES-1;
#if 0
    int memid = -1;
    int limit = SMMGR_FREE_MINSIZE;
    while (size >= limit && memid < (SMMGR_NUM_CLASSES-1)) {
        limit *= 2;
        memid += 1;
    }
    assert(memid < SMMGR_NUM_CLASSES);
    return memid;
#endif
#if 0
    int memid;
    if (size < 2048) {
        if (size < 1024) memid = ( 0 + ((size     ) /  32));
        else             memid = (32 + ((size-1024) /  64));
    } else if (size < 8192) {
        if (size < 4096) memid = (48 + ((size-2048) / 128));
        else             memid = (64 + ((size-4096) / 256));
    } else {
        memid = SMMGR_NUM_CLASSES-1;
    }
    assert(memid < SMMGR_NUM_CLASSES);
    return memid;
#endif
#if 0
    int memid;
    if (size < 2048) {
        if (size < 1024) memid = (  0 + ((size     ) /  16));
        else             memid = ( 64 + ((size-1024) /  32));
    } else if (size < 8192) {
        if (size < 4096) memid = ( 96 + ((size-2048) /  64));
        else             memid = (128 + ((size-4096) / 128));
    } else {
        memid = SMMGR_NUM_CLASSES-1;
    }
    assert(memid < SMMGR_NUM_CLASSES);
    return memid;
#endif
#if 0
    int memid;
    if (size <= 2048) {
        if (size <= 512)       memid = (0  + ((size     -1) /  16));
        else if (size <= 1024) memid = (32 + ((size- 512-1) /  32));
        else                   memid = (48 + ((size-1024-1) /  64));
    } else {
        if (size <= 4096)      memid = (64 + ((size-2048-1) / 128));
        else if (size <= 8192) memid = (80 + ((size-4096-1) / 256));
        else                   memid = 96;
    }
    assert(memid < SMMGR_NUM_CLASSES);
    return memid;
#endif
}

static void do_smmgr_used_slot_list_add(int targ)
{
    if (targ < sm_anchor.used_minid) {
        sm_anchor.used_minid = targ;
    }
    if (targ > sm_anchor.used_maxid) {
        /* adjust free_space small & avail */
        int smid = (sm_anchor.used_maxid < 0 ? 0 : sm_anchor.used_maxid);
        for ( ; smid < targ; smid++) {
            if (sm_anchor.free_slist[smid].space > 0) {
                sm_anchor.free_small_space += sm_anchor.free_slist[smid].space;
                sm_anchor.free_avail_space -= sm_anchor.free_slist[smid].space;
            }
        }
        sm_anchor.used_maxid = targ;
    }
    sm_anchor.used_num_classes += 1;
}

static void do_smmgr_used_slot_list_del(int targ)
{
    int smid;
    if (targ == sm_anchor.used_minid) {
        if (sm_anchor.used_total_space > 0) {
            for (smid = sm_anchor.used_minid+1; smid <= sm_anchor.used_maxid; smid++) {
                if (sm_anchor.used_slist[smid].count > 0) break;
            }
            sm_anchor.used_minid = smid;
        } else {
            sm_anchor.used_minid = SMMGR_NUM_CLASSES;
        }
    }
    if (targ == sm_anchor.used_maxid) {
        if (sm_anchor.used_total_space > 0) {
            for (smid = sm_anchor.used_maxid-1; smid >= sm_anchor.used_minid; smid--) {
                if (sm_anchor.used_slist[smid].count > 0) break;
            }
            sm_anchor.used_maxid = smid;
        } else {
            sm_anchor.used_maxid = -1;
        }
        /* adjust free_space small & avail */
        smid = (sm_anchor.used_maxid < 0 ? 0 : sm_anchor.used_maxid);
        for ( ; smid < targ; smid++) {
            if (sm_anchor.free_slist[smid].space > 0) {
                sm_anchor.free_small_space -= sm_anchor.free_slist[smid].space;
                sm_anchor.free_avail_space += sm_anchor.free_slist[smid].space;
            }
        }
    }
    sm_anchor.used_num_classes -= 1;
}

static void do_smmgr_free_slot_list_add(int targ)
{
    if (targ < (SMMGR_NUM_CLASSES-1)) {
        if (sm_anchor.free_minid > targ) {
            sm_anchor.free_minid = targ;
        }
        if (sm_anchor.free_maxid < targ) {
            sm_anchor.free_maxid = targ;
        }
        sm_anchor.free_num_classes += 1;
    }
    /*****
    if (sm_anchor.free_maxid < targ) {
        sm_anchor.free_maxid = targ;
    }
    *****/
}

static void do_smmgr_free_slot_list_del(int targ)
{
    if (targ < (SMMGR_NUM_CLASSES-1)) {
        int smid;
        if (targ == sm_anchor.free_minid) {
            if (sm_anchor.free_minid < sm_anchor.free_maxid) {
                for (smid = sm_anchor.free_minid+1; smid <= sm_anchor.free_maxid; smid++) {
                    if (sm_anchor.free_slist[smid].head != NULL) break;
                }
                sm_anchor.free_minid = smid;
            } else {
                sm_anchor.free_minid = SMMGR_NUM_CLASSES;
            }
        }
        if (targ == sm_anchor.free_maxid) {
            if (sm_anchor.free_maxid > sm_anchor.free_minid) {
                for (smid = sm_anchor.free_maxid-1; smid >= sm_anchor.free_minid; smid--) {
                    if (sm_anchor.free_slist[smid].head != NULL) break;
                }
                sm_anchor.free_maxid = smid;
            } else {
                sm_anchor.free_maxid = -1;
            }
        }
        sm_anchor.free_num_classes -= 1;
    }
}

static void do_smmgr_used_slot_init(sm_slot_t *slot, uint16_t offset, uint16_t length)
{
    sm_tail_t *tail = (sm_tail_t*)((char*)slot + length - sizeof(sm_tail_t));
    tail->offset = offset;
    tail->length = length; /* used slot */
    /* Set it as used slot.
     * In eager invalidation, incomplete slot can be inspected.
     * At that time, the slot must be regared as an used slot.
     */
    slot->status = (uint32_t)-1; /* used slot */
}

static void do_smmgr_free_slot_init(sm_slot_t *slot, uint16_t offset, uint16_t length)
{
    sm_tail_t *tail = (sm_tail_t*)((char*)slot + length - sizeof(sm_tail_t));
    tail->offset = offset;
    tail->length = 0; /* free slot */
    slot->status = 0; /* free slot */
    slot->offset = offset;
    slot->length = length;
}

static void do_smmgr_free_slot_link(sm_slot_t *slot)
{
    sm_slist_t *list;
    int         smid;

    if (slot->length < SMMGR_MIN_SLOT_SIZE) {
        sm_anchor.free_small_space += slot->length;
        return;
    }

    smid = do_smmgr_memid(slot->length);
    list = &sm_anchor.free_slist[smid];

    if (list->head == NULL) {
        assert(list->tail == NULL && list->count == 0 && list->space == 0);
        slot->prev = NULL;
        slot->next = NULL;
        list->head = slot;
        list->tail = slot;
        list->space = slot->length;
        list->count = 1;
        do_smmgr_free_slot_list_add(smid);
    } else {
        assert(list->tail != NULL && list->count > 0 && list->space > 0);
        slot->prev = list->tail;
        slot->next = NULL;
        slot->prev->next = slot;
        list->tail = slot;
        list->space += slot->length;
        list->count += 1;
    }
    if (smid < sm_anchor.used_maxid) {
        sm_anchor.free_small_space += slot->length;
    } else {
        sm_anchor.free_avail_space += slot->length;
    }
}

static void do_smmgr_free_slot_unlink(sm_slot_t *slot)
{
    sm_slist_t *list;
    int         smid;

    if (slot->length < SMMGR_MIN_SLOT_SIZE) {
        sm_anchor.free_small_space -= slot->length;
        return;
    }

    smid = do_smmgr_memid(slot->length);
    if (smid < sm_anchor.used_maxid) {
        sm_anchor.free_small_space -= slot->length;
    } else {
        sm_anchor.free_avail_space -= slot->length;
    }

    list = &sm_anchor.free_slist[smid];
    if (list->count == 1) {
        assert(list->space == slot->length && list->head == slot && list->tail == slot);
        list->head = NULL;
        list->tail = NULL;
        list->space = 0;
        list->count = 0;
        do_smmgr_free_slot_list_del(smid);
    } else {
        assert(list->count > 1 && list->space > slot->length &&
               list->head != NULL && list->tail != NULL);
        if (slot->prev != NULL) slot->prev->next = slot->next;
        if (slot->next != NULL) slot->next->prev = slot->prev;
        if (list->head == slot) list->head = slot->next;
        if (list->tail == slot) list->tail = slot->prev;
        list->space -= slot->length;
        list->count -= 1;
    }
}

static void do_smmgr_free_slot_replace(sm_slot_t *old_slot, sm_slot_t *new_slot, int smid)
{
    sm_slist_t *list = &sm_anchor.free_slist[smid];
    assert(list->head == old_slot);
    uint16_t diff_leng = old_slot->length - new_slot->length;

    new_slot->prev = NULL;
    if (old_slot->next == NULL) {
        new_slot->next = NULL;
        list->tail = new_slot;
    } else {
        new_slot->next = old_slot->next;
        new_slot->next->prev = new_slot;
    }
    list->head = new_slot;
    list->space -= diff_leng;

    if (smid < sm_anchor.used_maxid) {
        sm_anchor.free_small_space -= diff_leng;
    } else {
        sm_anchor.free_avail_space -= diff_leng;
    }
}

#if 0 // can be used later
static void do_smmgr_used_blck_check(void)
{
    sm_blck_t *blck;
    sm_slot_t *slot;
    sm_tail_t *tail;
    uint64_t blck_count = 0;
    uint64_t used_count = 0;
    uint64_t free_count = 0;
    uint32_t comp_length;

    blck = sm_anchor.used_blist.head;
    while (blck != NULL) {
        blck_count += 1;
        tail = (sm_tail_t*)((char*)blck + sm_anchor.blck_tsize - sizeof(sm_tail_t));
        while (((char*)tail - (char*)blck) > sizeof(sm_blck_t)) {
            slot = (sm_slot_t*)((char*)blck + tail->offset);
            if (tail->length > 8) { /* used slot */
                used_count += 1;
                assert(slot->status != 0);
            } else { /* free slot */
                free_count += 1;
                comp_length = (char*)tail - (char*)slot + sizeof(sm_tail_t);
                assert(slot->status == 0);
                assert(slot->offset == tail->offset);
                assert(slot->length == comp_length);
            }
            tail = (sm_tail_t*)((char*)slot - sizeof(sm_tail_t));
        }
        blck = blck->next;
    }
    assert(blck_count == sm_anchor.used_blist.count);
}
#endif

static void do_smmgr_used_blck_link(sm_blck_t *blck)
{
    blck->frspc = 0;
    blck->frcnt = 0;

    blck->prev = sm_anchor.used_blist.tail;
    blck->next = NULL;
    if (sm_anchor.used_blist.head == NULL) {
        sm_anchor.used_blist.head = blck;
        sm_anchor.used_blist.tail = blck;
    } else {
        blck->prev->next = blck;
        sm_anchor.used_blist.tail = blck;
    }
    sm_anchor.used_blist.count += 1;
}

static void do_smmgr_used_blck_unlink(sm_blck_t *blck)
{
    if (blck->prev != NULL) blck->prev->next = blck->next;
    if (blck->next != NULL) blck->next->prev = blck->prev;
    if (sm_anchor.used_blist.head == blck) sm_anchor.used_blist.head = blck->next;
    if (sm_anchor.used_blist.tail == blck) sm_anchor.used_blist.tail = blck->prev;
    sm_anchor.used_blist.count -= 1;
}

static sm_blck_t *do_smmgr_blck_alloc(struct default_engine *engine)
{
    sm_blck_t *blck = (sm_blck_t *)do_slabs_alloc(engine, sm_anchor.blck_tsize, sm_anchor.blck_clsid);
    if (blck != NULL) {
        do_smmgr_used_blck_link(blck);
    } else {
        logger->log(EXTENSION_LOG_INFO, NULL, "no more small memory chunk\n");
    }
    if (slabs_short_of_free_space(engine) > 0) {
        coll_del_thread_wakeup(engine);
    }
    return blck;
}

static void do_smmgr_blck_free(struct default_engine *engine, sm_blck_t *blck)
{
    do_smmgr_used_blck_unlink(blck);
    do_slabs_free(engine, blck, sm_anchor.blck_tsize, sm_anchor.blck_clsid);
}

#else // VARIABLE_LENGTH_SMMGR
static mem_chunk_t *do_mem_chunk_alloc(struct default_engine *engine, int memid)
{
    mem_chunk_t *chunk;

    chunk = (mem_chunk_t *)do_slabs_alloc(engine, smmgr_chunk_size, smmgr_chunk_clsid);
    if (chunk != NULL) {
        int item_size = mem_anchor[memid].chunk_item_size;
        void    **item;
        uint16_t *pidx; /* physical item index */

        chunk->total_item_count = mem_anchor[memid].items_per_chunk;
        chunk->free_item_count  = chunk->total_item_count;
        for (int i = 0; i < chunk->total_item_count; i++) {
             item  = (void**)((char *)chunk + sizeof(mem_chunk_t) + (i*item_size));
             *item = ((i == (chunk->total_item_count-1)) ? (void*)NULL
                                                         : (void*)((char*)item + item_size));
             pidx  = (uint16_t *)((char*)item + item_size - sizeof(uint16_t));
             *pidx = i;
        }
        chunk->free_item = (void *)((char *)chunk + sizeof(mem_chunk_t));
        total_mem_chunk_count++;
        if (smslab_cur_free_nchunk != -1)
            smslab_cur_free_nchunk--;
    } else {
        logger->log(EXTENSION_LOG_INFO, NULL, "no more small memory chunk\n");
    }
    if (slabs_short_of_free_space(engine) > 0) {
        coll_del_thread_wakeup(engine);
    }
    return chunk;
}

static void do_mem_chunk_free(struct default_engine *engine, mem_chunk_t *chunk)
{
    do_slabs_free(engine, chunk, smmgr_chunk_size, smmgr_chunk_clsid);
    total_mem_chunk_count--;
    if (smslab_cur_free_nchunk != -1)
        smslab_cur_free_nchunk++;
}

static void do_mem_chunk_link(int memid, mem_chunk_t *chunk, bool chunk_list_flag)
{
    mem_anchor_t *anchor = &mem_anchor[memid];

    if (chunk_list_flag == true) {
        chunk->prev = NULL;
        chunk->next = anchor->chunk_list;
        if (chunk->next != NULL) chunk->next->prev = chunk;
        anchor->chunk_list = chunk;
        anchor->total_chunk_count++;

        anchor->items_free_count += anchor->items_per_chunk;
        if (anchor->short_of_space != 0) {
            if (anchor->items_free_count >= (anchor->items_per_chunk*anchor->total_chunk_count/30)) { /* 3.33% */
                anchor->short_of_space = 0;
                mem_lack_of_space_count--;
            }
        } else { /* anchor->short_of_space == 0 */
            if (anchor->total_chunk_count >= mem_nchunk_for_wideuse &&
                anchor->items_free_count < (anchor->items_per_chunk*anchor->total_chunk_count/30)) { /* 3.33% */
                anchor->short_of_space = 1;
                mem_lack_of_space_count++;
            }
        }
    }

    chunk->free_prev = anchor->chunk_free_tail;
    chunk->free_next = NULL;
    if (chunk->free_prev != NULL) chunk->free_prev->free_next = chunk;
    anchor->chunk_free_tail = chunk;
    if (anchor->chunk_free_head == NULL)
        anchor->chunk_free_head = chunk;
    anchor->free_chunk_count++;
}

static void do_mem_chunk_unlink(int memid, mem_chunk_t *chunk, bool chunk_list_flag)
{
    mem_anchor_t *anchor = &mem_anchor[memid];

    if (chunk->free_next != NULL)
        chunk->free_next->free_prev = chunk->free_prev;
    if (chunk->free_prev != NULL)
        chunk->free_prev->free_next = chunk->free_next;
    if (anchor->chunk_free_head == chunk)
        anchor->chunk_free_head = chunk->free_next;
    if (anchor->chunk_free_tail == chunk)
        anchor->chunk_free_tail = chunk->free_prev;
    chunk->free_prev = chunk->free_next = NULL;
    anchor->free_chunk_count--;

    if (chunk_list_flag == true) {
        if (chunk->next != NULL)
            chunk->next->prev = chunk->prev;
        if (chunk->prev != NULL)
            chunk->prev->next = chunk->next;
        if (anchor->chunk_list == chunk)
            anchor->chunk_list = chunk->next;
        chunk->prev = chunk->next = NULL;
        anchor->total_chunk_count--;
        anchor->items_free_count -= anchor->items_per_chunk;

        if (anchor->short_of_space != 0) {
            if (anchor->total_chunk_count < mem_nchunk_for_wideuse) {
                anchor->short_of_space = 0;
                mem_lack_of_space_count--;
            }
        } else { /* anchor->short_of_space == 0 */
            if (anchor->total_chunk_count >= mem_nchunk_for_wideuse &&
                anchor->items_free_count < (anchor->items_per_chunk*anchor->total_chunk_count/30)) { /* 3.33% */
                anchor->short_of_space = 1;
                mem_lack_of_space_count++;
            }
        }
    }
}

static bool do_mem_near_short_of_space(mem_anchor_t *anchor)
{
    /* anchor->short_of_space == 0 */
    if ((anchor->total_chunk_count-1) >= mem_nchunk_for_wideuse) {
        uint32_t items_free_count = anchor->items_free_count - anchor->items_per_chunk;
        if (items_free_count < (anchor->items_per_chunk*(anchor->total_chunk_count-1)/30)) { /* 3.33% */
            return true;
        }
    }
    return false;
}

static inline int do_smmgr_memid(size_t size)
{
    int item_size = SMMGR_ITEM_SIZE(size);
    int memid;
    assert(item_size <= SMMGR_MAX_ITEM_SIZE);
    if (item_size <= 512) {
        if (item_size <= 128)       memid = ( 0 + ((item_size     -1) /   8));
        else if (item_size <= 256)  memid = (16 + ((item_size- 128-1) /  16));
        else                        memid = (24 + ((item_size- 256-1) /  32));
    } else {
        if (item_size <= 1024)      memid = (32 + ((item_size- 512-1) /  64));
        else if (item_size <= 2048) memid = (40 + ((item_size-1024-1) / 128));
        else if (item_size <= 4096) memid = (48 + ((item_size-2048-1) / 256));
        else                        memid = (56 + ((item_size-4096-1) / 512));
    }
    assert(memid < SMMGR_MAX_CLASS_COUNT);
    return memid;
}
#endif // VARIABLE_LENGTH_SMMGR

static void *do_smmgr_alloc(struct default_engine *engine, const size_t size)
{
#ifdef VARIABLE_LENGTH_SMMGR
    sm_slot_t *cur_slot = NULL;
    int smid, targ;
    int slen = SMMGR_SLOT_SIZE(size);

    if (slen < SMMGR_MIN_SLOT_SIZE)
        slen = SMMGR_MIN_SLOT_SIZE;
    targ = do_smmgr_memid(slen);

    /* allocate slot from free slot list */
#if 0 // check
    if (targ <= sm_anchor.free_maxid) {
        for (smid = targ; smid <= sm_anchor.free_maxid; smid++) {
            if (sm_anchor.free_slist[smid].head != NULL) break;
        }
    } else {
        smid = SMMGR_NUM_CLASSES-1;
    }
    cur_slot = sm_anchor.free_slist[smid].head;
#endif
#if 0 // check
    if (targ <= sm_anchor.free_maxid) {
        if (sm_anchor.free_slist[targ].head != NULL) smid = targ;
        else                                         smid = sm_anchor.free_maxid;
    } else {
        smid = SMMGR_NUM_CLASSES-1;
    }
    cur_slot = sm_anchor.free_slist[smid].head;
#endif
#if 0
    for (smid = targ; smid < sm_anchor.free_maxid; smid += targ) {
        if (sm_anchor.free_slist[smid].head != NULL) {
            cur_slot = sm_anchor.free_slist[smid].head; break;
        }
    }
    if (cur_slot == NULL && targ < sm_anchor.free_maxid) {
        smid = sm_anchor.free_maxid;
        cur_slot = sm_anchor.free_slist[smid].head;
        assert(cur_slot != NULL);
    }
#endif
    if (targ <= sm_anchor.free_maxid) {
        if (sm_anchor.free_slist[targ].head != NULL) {
            smid = targ;
        } else if ((targ*2) <= sm_anchor.free_maxid) {
            for (smid = targ*2; smid <= sm_anchor.free_maxid; smid++) {
                if (sm_anchor.free_slist[smid].head != NULL) break;
            }
        } else {
            smid = sm_anchor.free_maxid;
        }
    } else {
        smid = SMMGR_NUM_CLASSES-1;
    }

    cur_slot = sm_anchor.free_slist[smid].head;
    if (cur_slot == NULL) {
        sm_blck_t *blck = do_smmgr_blck_alloc(engine);
        if (blck == NULL) return NULL;

        cur_slot = (sm_slot_t*)((char*)blck + sizeof(sm_blck_t) + slen);
        do_smmgr_free_slot_init(cur_slot, sizeof(sm_blck_t) + slen, sm_anchor.blck_bsize - slen);
        do_smmgr_free_slot_link(cur_slot);

        cur_slot = (sm_slot_t*)((char*)blck + sizeof(sm_blck_t));
        do_smmgr_used_slot_init(cur_slot, sizeof(sm_blck_t), slen);
    } else {
        if (cur_slot->length > slen) {
            sm_slot_t *nxt_slot = (sm_slot_t*)((char*)cur_slot + slen);
            if (smid != do_smmgr_memid(cur_slot->length - slen)) {
                do_smmgr_free_slot_unlink(cur_slot);
                do_smmgr_free_slot_init(nxt_slot, cur_slot->offset + slen, cur_slot->length - slen);
                do_smmgr_free_slot_link(nxt_slot);
            } else {
                do_smmgr_free_slot_init(nxt_slot, cur_slot->offset + slen, cur_slot->length - slen);
                do_smmgr_free_slot_replace(cur_slot, nxt_slot, smid);
            }
        } else {
            do_smmgr_free_slot_unlink(cur_slot);
        }
        do_smmgr_used_slot_init(cur_slot, cur_slot->offset, slen);
    }

    /* used slot stats */
    sm_anchor.used_total_space += slen;
    sm_anchor.used_slist[targ].space += slen;
    sm_anchor.used_slist[targ].count += 1;
    if (sm_anchor.used_slist[targ].count == 1) {
        do_smmgr_used_slot_list_add(targ);
    }

    //do_smmgr_used_blck_check();
    return (void*)cur_slot;
#else
    void *ret;
    int memid = do_smmgr_memid(size);
    int above_memid = -1;
    mem_anchor_t *anchor = &mem_anchor[memid];
    mem_chunk_t  *chunk;

    if (anchor->chunk_free_head == NULL) {
        chunk = do_mem_chunk_alloc(engine, memid);
        if (chunk != NULL) {
            do_mem_chunk_link(memid, chunk, true);
            assert(anchor->chunk_free_head != NULL);
        } else {
            for (above_memid = memid+1; above_memid < mem_class_count; above_memid++) {
                if (mem_anchor[above_memid].chunk_free_head != NULL) break;
            }
            if (above_memid == mem_class_count) {
                return NULL;
            }
            anchor = &mem_anchor[above_memid];
            chunk = anchor->chunk_free_head;
        }
    } else {
        if ((smslab_cur_free_nchunk == -1 || smslab_cur_free_nchunk >= smslab_min_free_nchunk) &&
            (anchor->short_of_space != 0)) {
            /* allocate additional chunk to avoid short of space */
            chunk = do_mem_chunk_alloc(engine, memid);
            if (chunk != NULL) {
                do_mem_chunk_link(memid, chunk, true);
            }
        }
        chunk = anchor->chunk_free_head;
    }

    ret = chunk->free_item;
    chunk->free_item = *((void**)chunk->free_item);

    chunk->free_item_count--;
    if (chunk->free_item_count == 0) {
        assert(chunk->free_item == NULL);
        do_mem_chunk_unlink((above_memid != -1 ? above_memid : memid), chunk, false);
    }

    anchor->items_free_count--;
    if (anchor->short_of_space == 0) {
        if (anchor->total_chunk_count >= mem_nchunk_for_wideuse &&
            anchor->items_free_count < (anchor->items_per_chunk*anchor->total_chunk_count/30)) { /* 3.33% */
            anchor->short_of_space = 1;
            mem_lack_of_space_count++;
        }
    }
    if (above_memid != -1) {
        uint16_t *pidx = (uint16_t *)((char*)ret + mem_anchor[memid].chunk_item_size - sizeof(uint16_t));
        *pidx = SMMGR_MAX_ITEM_PHYIDX;
        *(pidx+1) = above_memid;
    }
    return ret;
#endif
}

static void do_smmgr_free(struct default_engine *engine, void *ptr, const size_t size)
{
#ifdef VARIABLE_LENGTH_SMMGR
    sm_blck_t *cur_blck;
    sm_slot_t *cur_slot;
    sm_tail_t *cur_tail;
    int targ;
    int slen = SMMGR_SLOT_SIZE(size);

    if (slen < SMMGR_MIN_SLOT_SIZE)
        slen = SMMGR_MIN_SLOT_SIZE;
    targ = do_smmgr_memid(slen);

    //do_smmgr_used_blck_check();

    cur_tail = (sm_tail_t*)((char*)ptr + slen - sizeof(sm_tail_t));
    assert(cur_tail->length == slen);
    cur_blck = (sm_blck_t*)((char*)ptr - cur_tail->offset);

    /* check if prev slot is in freed state. if then, merge with that */
    if (cur_tail->offset > sizeof(sm_blck_t)) {
        sm_slot_t *prv_slot;
        sm_tail_t *prv_tail = (sm_tail_t*)((char*)ptr - sizeof(sm_tail_t));
        if (prv_tail->length <= 8) { /* free slot */
            prv_slot = (sm_slot_t*)((char*)cur_blck + prv_tail->offset);
            assert(prv_slot->offset == prv_tail->offset);
            do_smmgr_free_slot_unlink(prv_slot);
            cur_tail->offset  = prv_slot->offset;
            cur_tail->length += prv_slot->length;
        }
    }
    /* check if next slot is in freed state. if then, merge with that */
    if ((cur_tail->offset + cur_tail->length) < sm_anchor.blck_tsize) {
        sm_slot_t *nxt_slot = (sm_slot_t*)((char*)cur_tail + sizeof(sm_tail_t));
        sm_tail_t *nxt_tail;
        if (nxt_slot->status == 0) { /* free slot */
            nxt_tail = (sm_tail_t*)((char*)nxt_slot + nxt_slot->length - sizeof(sm_tail_t));
            assert(nxt_tail->offset == nxt_slot->offset && nxt_tail->length <= 8);
            do_smmgr_free_slot_unlink(nxt_slot);
            nxt_tail->offset = cur_tail->offset;
            nxt_tail->length = cur_tail->length + nxt_slot->length;
            cur_tail = nxt_tail;
        }
    }

    if (cur_tail->offset > sizeof(sm_blck_t) || cur_tail->length < sm_anchor.blck_bsize) {
        cur_slot = (sm_slot_t*)((char*)cur_tail - cur_tail->length + sizeof(sm_tail_t));
        do_smmgr_free_slot_init(cur_slot, cur_tail->offset, cur_tail->length);
        do_smmgr_free_slot_link(cur_slot);
    } else {
        do_smmgr_blck_free(engine, cur_blck);
    }

    /* used slot stats */
    assert(sm_anchor.used_slist[targ].count >= 1);
    sm_anchor.used_total_space -= slen;
    sm_anchor.used_slist[targ].space -= slen;
    sm_anchor.used_slist[targ].count -= 1;
    if (sm_anchor.used_slist[targ].count == 0) {
        do_smmgr_used_slot_list_del(targ);
    }
#else
    int memid = do_smmgr_memid(size);
    mem_anchor_t *anchor = &mem_anchor[memid];
    uint16_t     *pidx   = (uint16_t*)((char*)ptr + anchor->chunk_item_size - sizeof(uint16_t));
    mem_chunk_t  *chunk;

    if (*pidx == SMMGR_MAX_ITEM_PHYIDX) {
        memid = *(pidx+1);
        anchor = &mem_anchor[memid];
        pidx = (uint16_t*)((char*)ptr + anchor->chunk_item_size - sizeof(uint16_t));
    }
    chunk = (mem_chunk_t *)((char*)ptr - sizeof(mem_chunk_t) - ((*pidx)*anchor->chunk_item_size));

    *(void**)ptr = chunk->free_item;
    chunk->free_item = ptr;

    anchor->items_free_count++;
    if (anchor->short_of_space != 0) {
        if (anchor->items_free_count >= (anchor->items_per_chunk*anchor->total_chunk_count/30)) { /* 3.33% */
            anchor->short_of_space = 0;
            mem_lack_of_space_count--;
        }
    }

    chunk->free_item_count++;
    if (chunk->free_item_count == 1) {
        do_mem_chunk_link(memid, chunk, false);
    }
    if (chunk->free_item_count >= chunk->total_item_count) {
        /* free chunk in which all items are free */
        if ((smslab_cur_free_nchunk == -1 || smslab_cur_free_nchunk >= smslab_min_free_nchunk) &&
            (anchor->short_of_space != 0 || do_mem_near_short_of_space(anchor))) {
            /* do not free the chunk */
        } else {
            do_mem_chunk_unlink(memid, chunk, true);
            do_mem_chunk_free(engine, chunk);
        }
    }
#endif
}

unsigned int slabs_space_size(struct default_engine *engine, const size_t size)
{
    if (size <= MAX_SM_VALUE_SIZE) {
#ifdef VARIABLE_LENGTH_SMMGR
        return SMMGR_SLOT_SIZE(size);
#else
        int memid = do_smmgr_memid(size);
        return mem_anchor[memid].chunk_item_size;
#endif
    }
    int clsid = slabs_clsid(engine, size);
    if (clsid == 0)
        return 0;
    else
        return engine->slabs.slabclass[clsid].size;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
ENGINE_ERROR_CODE slabs_init(struct default_engine *engine,
                             const size_t limit, const double factor, const bool prealloc)
{
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(hash_item) + engine->config.chunk_size;

    logger = engine->server.log->get_logger();

    engine->slabs.mem_limit = limit;
    engine->slabs.mem_reserved = (limit / 100) * RESERVED_SLAB_RATIO;
    if (engine->slabs.mem_reserved < (RESERVED_SLABS*engine->config.item_size_max))
        engine->slabs.mem_reserved = (RESERVED_SLABS*engine->config.item_size_max);

    if (prealloc) {
        /* Allocate everything in a big chunk with malloc */
        engine->slabs.mem_base = malloc(engine->slabs.mem_limit);
        if (engine->slabs.mem_base != NULL) {
            engine->slabs.mem_current = engine->slabs.mem_base;
            engine->slabs.mem_avail = engine->slabs.mem_limit;
        } else {
            return ENGINE_ENOMEM;
        }
    } else {
        engine->slabs.mem_base = NULL;
        engine->slabs.mem_current = NULL;
        engine->slabs.mem_avail = 0;
    }

    memset(engine->slabs.slabclass, 0, sizeof(engine->slabs.slabclass));

    while (++i < POWER_LARGEST && size <= engine->config.item_size_max / factor) {
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        engine->slabs.slabclass[i].size = size;
        engine->slabs.slabclass[i].perslab = engine->config.item_size_max / engine->slabs.slabclass[i].size;
        engine->slabs.slabclass[i].rsvd_slabs = RESERVED_SLABS;

        size *= factor;
        if (engine->config.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, engine->slabs.slabclass[i].size, engine->slabs.slabclass[i].perslab);
        }
    }

    engine->slabs.power_largest = i;
    engine->slabs.slabclass[engine->slabs.power_largest].size = engine->config.item_size_max;
    engine->slabs.slabclass[engine->slabs.power_largest].perslab = 1;
    engine->slabs.slabclass[engine->slabs.power_largest].rsvd_slabs = RESERVED_SLABS;

    if (engine->config.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, engine->slabs.slabclass[i].size, engine->slabs.slabclass[i].perslab);
    }

    /* for the test suite:  faking of how much we've already malloc'd */
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            engine->slabs.mem_malloced = (size_t)atol(t_initial_malloc);
        }

    }
#ifndef DONT_PREALLOC_SLABS
    {
        char *pre_alloc = getenv("T_MEMD_SLABS_ALLOC");

        if (pre_alloc == NULL || atoi(pre_alloc) != 0) {
            slabs_preallocate(power_largest);
        }
    }
#endif

    do_smmgr_init(engine);
    return ENGINE_SUCCESS;
}

#ifndef DONT_PREALLOC_SLABS
static void slabs_preallocate(const unsigned int maxslabs)
{
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
       confused by non-intuitive "SERVER_ERROR out of memory"
       messages.  this is the most common question on the mailing
       list.  if you really don't want this, you can rebuild without
       these three lines.  */

    for (i = POWER_SMALLEST; i <= POWER_LARGEST; i++) {
        if (++prealloc > maxslabs)
            return;
        do_slabs_newslab(i);
    }

    /* slab class 0 is used for collection items and small-size kv items */
    do_slabs_newslab(0);
}
#endif

static int grow_slab_list (struct default_engine *engine, const unsigned int id)
{
    slabclass_t *p = &engine->slabs.slabclass[id];
    if (p->slabs == p->list_size) {
        size_t new_size =  (p->list_size != 0) ? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void *));
        if (new_list == 0) return 0;
        p->list_size = new_size;
        p->slab_list = new_list;
    }
    return 1;
}

static int do_slabs_newslab(struct default_engine *engine, const unsigned int id)
{
    slabclass_t *p = &engine->slabs.slabclass[id];
    int len = p->size * p->perslab;
    char *ptr;

    if ((engine->slabs.mem_limit && engine->slabs.mem_malloced + len > engine->slabs.mem_limit && p->slabs >= p->rsvd_slabs) ||
        (grow_slab_list(engine, id) == 0) ||
        ((ptr = memory_allocate(engine, (size_t)len)) == 0)) {

        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    memset(ptr, 0, (size_t)len);
    p->end_page_ptr = ptr;
    p->end_page_free = p->perslab;

    p->slab_list[p->slabs++] = ptr;
    engine->slabs.mem_malloced += len;
#ifdef VARIABLE_LENGTH_SMMGR
#else
    if (id == smmgr_chunk_clsid && p->rsvd_slabs != 0 && p->slabs > p->rsvd_slabs) {
        smslab_cur_free_nchunk += p->perslab;
    }
#endif

    if ((engine->slabs.mem_limit <= engine->slabs.mem_malloced) ||
        ((engine->slabs.mem_limit - engine->slabs.mem_malloced) < engine->slabs.mem_reserved))
    {
#ifdef VARIABLE_LENGTH_SMMGR
        if (engine->slabs.slabclass[sm_anchor.blck_clsid].rsvd_slabs == 0) { /* undefined */
            /* define the reserved slab count of slab class 0 */
            slabclass_t *z = &engine->slabs.slabclass[sm_anchor.blck_clsid];
            unsigned int additional_slabs = (z->slabs/100) * RESERVED_SLAB_RATIO;
            if (additional_slabs < RESERVED_SLABS)
                additional_slabs = RESERVED_SLABS;
            z->rsvd_slabs = z->slabs + additional_slabs;
            coll_del_thread_wakeup(engine);
        }
#else
        if (engine->slabs.slabclass[smmgr_chunk_clsid].rsvd_slabs == 0) { /* undefined */
            /* define the reserved slab count of slab class 0 */
            slabclass_t *z = &engine->slabs.slabclass[smmgr_chunk_clsid];
            unsigned int additional_slabs = (z->slabs/100) * RESERVED_SLAB_RATIO;
            if (additional_slabs < RESERVED_SLABS)
                additional_slabs = RESERVED_SLABS;
            z->rsvd_slabs = z->slabs + additional_slabs;
            smslab_min_free_nchunk = (z->rsvd_slabs - z->slabs) * z->perslab;
            smslab_cur_free_nchunk = z->sl_curr + z->end_page_free + (int)smslab_min_free_nchunk;
            coll_del_thread_wakeup(engine);
        }
#endif
    }
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

/*@null@*/
static void *do_slabs_alloc(struct default_engine *engine, const size_t size, unsigned int id)
{
    slabclass_t *p;
    void *ret = NULL;

    if (size <= MAX_SM_VALUE_SIZE) {
        return do_smmgr_alloc(engine, size);
    }

    p = &engine->slabs.slabclass[id];

#ifdef USE_SYSTEM_MALLOC
    if (engine->slabs.mem_limit && engine->slabs.mem_malloced + size > engine->slabs.mem_limit) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
        return 0;
    }
    engine->slabs.mem_malloced += size;
    ret = malloc(size);
    MEMCACHED_SLABS_ALLOCATE(size, id, 0, ret);
    return ret;
#endif

    /* fail unless we have space at the end of a recently allocated page,
       we have something on our freelist, or we could allocate a new page */
    if (! (p->end_page_ptr != 0 || p->sl_curr != 0 ||
           do_slabs_newslab(engine, id) != 0)) {
        /* We don't have more memory available */
        ret = NULL;
    } else if (p->sl_curr != 0) {
        /* return off our freelist */
        ret = p->slots[--p->sl_curr];
    } else {
        /* if we recently allocated a whole page, return from that */
        assert(p->end_page_ptr != NULL);
        ret = p->end_page_ptr;
        if (--p->end_page_free != 0) {
            p->end_page_ptr = ((caddr_t)p->end_page_ptr) + p->size;
        } else {
            p->end_page_ptr = 0;
        }
    }

    if (ret) {
        p->requested += size;
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }
    return ret;
}

static void do_slabs_free(struct default_engine *engine, void *ptr, const size_t size, unsigned int id)
{
    slabclass_t *p;

    if (size <= MAX_SM_VALUE_SIZE) {
        do_smmgr_free(engine, ptr, size);
        return;
    }

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &engine->slabs.slabclass[id];

#ifdef USE_SYSTEM_MALLOC
    engine->slabs.mem_malloced -= size;
    free(ptr);
    return;
#endif

    if (p->sl_curr == p->sl_total) { /* need more space on the free list */
        int new_size = (p->sl_total != 0) ? p->sl_total * 2 : 16;  /* 16 is arbitrary */
        void **new_slots = realloc(p->slots, new_size * sizeof(void *));
        if (new_slots == 0)
            return;
        p->slots = new_slots;
        p->sl_total = new_size;
    }
    p->slots[p->sl_curr++] = ptr;
    p->requested -= size;
    return;
}

void add_statistics(const void *cookie, ADD_STAT add_stats,
                    const char* prefix, int num, const char *key,
                    const char *fmt, ...)
{
    char name[80], val[80];
    int klen = 0, vlen;
    va_list ap;

    assert(cookie);
    assert(add_stats);
    assert(key);

    va_start(ap, fmt);
    vlen = vsnprintf(val, sizeof(val) - 1, fmt, ap);
    va_end(ap);

    if (prefix != NULL) {
        klen = snprintf(name, sizeof(name), "%s:", prefix);
    }

    if (num != -1) {
        klen += snprintf(name + klen, sizeof(name) - klen, "%d:", num);
    }

    klen += snprintf(name + klen, sizeof(name) - klen, "%s", key);

    add_stats(name, klen, val, vlen, cookie);
}

/*@null@*/
static void do_slabs_stats(struct default_engine *engine, ADD_STAT add_stats, const void *cookie)
{
    int i, total;
    /* Get the per-thread stats which contain some interesting aggregates */
#ifdef FUTURE
    struct conn *conn = (struct conn*)cookie;
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(c, &thread_stats);
#endif

    /* small memory classes */
#ifdef VARIABLE_LENGTH_SMMGR
    slabclass_t *smp = &engine->slabs.slabclass[sm_anchor.blck_clsid];
    uint64_t free_chunk_space = 0;
    if (smp->rsvd_slabs > 0) {
        free_chunk_space = smp->sl_curr + smp->end_page_free;
        if (smp->slabs < smp->rsvd_slabs)
            free_chunk_space += ((smp->rsvd_slabs - smp->slabs) * smp->perslab);
        free_chunk_space *= sm_anchor.blck_tsize;
    }

    add_statistics(cookie, add_stats, "SM", -1, "used_num_classes", "%d", sm_anchor.used_num_classes);
    add_statistics(cookie, add_stats, "SM", -1, "free_num_classes", "%d", sm_anchor.free_num_classes);
    add_statistics(cookie, add_stats, "SM", -1, "used_min_classid", "%d", sm_anchor.used_minid);
    add_statistics(cookie, add_stats, "SM", -1, "used_max_classid", "%d", sm_anchor.used_maxid);
    add_statistics(cookie, add_stats, "SM", -1, "free_min_classid", "%d", sm_anchor.free_minid);
    add_statistics(cookie, add_stats, "SM", -1, "free_max_classid", "%d", sm_anchor.free_maxid);
    add_statistics(cookie, add_stats, "SM", -1, "free_big_slot_count", "%"PRIu64, sm_anchor.free_slist[SMMGR_NUM_CLASSES-1].count);
    add_statistics(cookie, add_stats, "SM", -1, "used_total_space", "%"PRIu64, sm_anchor.used_total_space);
    add_statistics(cookie, add_stats, "SM", -1, "free_small_space", "%"PRIu64, sm_anchor.free_small_space);
    add_statistics(cookie, add_stats, "SM", -1, "free_avail_space", "%"PRIu64, sm_anchor.free_avail_space);
    add_statistics(cookie, add_stats, "SM", -1, "free_chunk_space", "%"PRIu64, free_chunk_space);
    add_statistics(cookie, add_stats, "SM", -1, "used_block_count", "%"PRIu64, sm_anchor.used_blist.count);
#else
    total = 0;
    for (i = 0; i < mem_class_count; i++) {
        mem_anchor_t *p = &mem_anchor[i];
        if (p->total_chunk_count > 0) {
            add_statistics(cookie, add_stats, "SM", i, "item_size", "%u", p->chunk_item_size);
            add_statistics(cookie, add_stats, "SM", i, "items_per_chunk", "%u", p->items_per_chunk);
            add_statistics(cookie, add_stats, "SM", i, "total_chunks", "%u", p->total_chunk_count);
            add_statistics(cookie, add_stats, "SM", i, "total_items", "%u", p->items_per_chunk * p->total_chunk_count);
            add_statistics(cookie, add_stats, "SM", i, "free_items", "%u", p->items_free_count);
            total++;
        }
    }
    add_statistics(cookie, add_stats, "SM", -1, "active_mem_classes", "%d", total);
    add_statistics(cookie, add_stats, "SM", -1, "lack_of_space_mem_classes", "%d", mem_lack_of_space_count);
    add_statistics(cookie, add_stats, "SM", -1, "total_mem_chunks", "%d", total_mem_chunk_count);
#endif

    total = 0;
    int min_slab_id = POWER_SMALLEST;
#ifdef VARIABLE_LENGTH_SMMGR
    min_slab_id = sm_anchor.blck_clsid;
#else
    min_slab_id = smmgr_chunk_clsid;
#endif
    for (i = min_slab_id; i <= engine->slabs.power_largest; i++) {
        slabclass_t *p = &engine->slabs.slabclass[i];
        if (p->slabs != 0) {
            uint32_t perslab, slabs;
            slabs = p->slabs;
            perslab = p->perslab;

            add_statistics(cookie, add_stats, NULL, i, "chunk_size", "%u", p->size);
            add_statistics(cookie, add_stats, NULL, i, "chunks_per_page", "%u", perslab);
            add_statistics(cookie, add_stats, NULL, i, "reserved_pages", "%u", p->rsvd_slabs);
            add_statistics(cookie, add_stats, NULL, i, "total_pages", "%u", slabs);
            add_statistics(cookie, add_stats, NULL, i, "total_chunks", "%u", slabs*perslab);
            add_statistics(cookie, add_stats, NULL, i, "used_chunks", "%u", (slabs*perslab)-p->sl_curr-p->end_page_free);
            add_statistics(cookie, add_stats, NULL, i, "free_chunks", "%u", p->sl_curr);
            add_statistics(cookie, add_stats, NULL, i, "free_chunks_end", "%u", p->end_page_free);
            add_statistics(cookie, add_stats, NULL, i, "mem_requested", "%zu", p->requested);
#ifdef FUTURE
            add_statistics(cookie, add_stats, NULL, i, "get_hits", "%"PRIu64, thread_stats.slab_stats[i].get_hits);
            add_statistics(cookie, add_stats, NULL, i, "cmd_set", "%"PRIu64, thread_stats.slab_stats[i].set_cmds);
            add_statistics(cookie, add_stats, NULL, i, "delete_hits", "%"PRIu64, thread_stats.slab_stats[i].delete_hits);
            add_statistics(cookie, add_stats, NULL, i, "cas_hits", "%"PRIu64, thread_stats.slab_stats[i].cas_hits);
            add_statistics(cookie, add_stats, NULL, i, "cas_badval", "%"PRIu64, thread_stats.slab_stats[i].cas_badval);
#endif
            total++;
        }
    }

    /* add overall slab stats and append terminator */
    add_statistics(cookie, add_stats, NULL, -1, "active_slabs", "%d", total);
#ifdef VARIABLE_LENGTH_SMMGR
#else
    add_statistics(cookie, add_stats, NULL, -1, "min_free_chunks_for_smslab", "%u", smslab_min_free_nchunk);
    add_statistics(cookie, add_stats, NULL, -1, "cur_free_chunks_for_smslab", "%u", smslab_cur_free_nchunk);
#endif
    add_statistics(cookie, add_stats, NULL, -1, "memory_limit", "%zu", engine->slabs.mem_limit);
    add_statistics(cookie, add_stats, NULL, -1, "total_malloced", "%zu", engine->slabs.mem_malloced);
}

static void *memory_allocate(struct default_engine *engine, size_t size)
{
    void *ret;

    if (engine->slabs.mem_base == NULL) {
        /* We are not using a preallocated large memory chunk */
        ret = malloc(size);
    } else {
        ret = engine->slabs.mem_current;

        if (size > engine->slabs.mem_avail) {
            return NULL;
        }

        /* mem_current pointer _must_ be aligned!!! */
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        engine->slabs.mem_current = ((char*)engine->slabs.mem_current) + size;
        if (size < engine->slabs.mem_avail) {
            engine->slabs.mem_avail -= size;
        } else {
            engine->slabs.mem_avail = 0;
        }
    }
    return ret;
}

static ENGINE_ERROR_CODE do_slabs_set_memlimit(struct default_engine *engine, size_t memlimit)
{
    if (engine->slabs.mem_base != NULL) {
        /* We are using a preallocated large memory chunk */
        return ENGINE_EBADVALUE;
    }
    if (memlimit < (engine->slabs.mem_malloced + (engine->slabs.mem_malloced/10))) {
        /* We cannot set mem_limit smaller than (mem_malloced * 1.1) */
        return ENGINE_EBADVALUE;
    }

    size_t new_mem_reserved = (memlimit / 100) * RESERVED_SLAB_RATIO;
    if (new_mem_reserved < (RESERVED_SLABS*engine->config.item_size_max))
        new_mem_reserved = (RESERVED_SLABS*engine->config.item_size_max);

#ifdef VARIABLE_LENGTH_SMMGR
    if (engine->slabs.slabclass[sm_anchor.blck_clsid].rsvd_slabs != 0) {
        /* memlimit > engine->slabs.mem_malloced */
        if ((memlimit - engine->slabs.mem_malloced) < new_mem_reserved) {
            return ENGINE_EBADVALUE;
        }
    }
#else
    if (engine->slabs.slabclass[smmgr_chunk_clsid].rsvd_slabs != 0) {
        /* memlimit > engine->slabs.mem_malloced */
        if ((memlimit - engine->slabs.mem_malloced) < new_mem_reserved) {
            return ENGINE_EBADVALUE;
        }
    }
#endif
    engine->slabs.mem_limit = memlimit;
    engine->slabs.mem_reserved = new_mem_reserved;
#ifdef VARIABLE_LENGTH_SMMGR
    engine->slabs.slabclass[sm_anchor.blck_clsid].rsvd_slabs = 0; /* undefined */
#else
    engine->slabs.slabclass[smmgr_chunk_clsid].rsvd_slabs = 0; /* undefined */
    smslab_min_free_nchunk = 0;
    smslab_cur_free_nchunk = -1;
#endif
    return ENGINE_SUCCESS;
}

void *slabs_alloc(struct default_engine *engine, size_t size, unsigned int id)
{
    void *ret;

    if (id < POWER_SMALLEST || id > engine->slabs.power_largest)
        return NULL;
    pthread_mutex_lock(&engine->slabs.lock);
    ret = do_slabs_alloc(engine, size, id);
    pthread_mutex_unlock(&engine->slabs.lock);
    return ret;
}

void slabs_free(struct default_engine *engine, void *ptr, size_t size, unsigned int id)
{
    if (id < POWER_SMALLEST || id > engine->slabs.power_largest)
        return;
    pthread_mutex_lock(&engine->slabs.lock);
    do_slabs_free(engine, ptr, size, id);
    pthread_mutex_unlock(&engine->slabs.lock);
}

void slabs_stats(struct default_engine *engine, ADD_STAT add_stats, const void *c)
{
    pthread_mutex_lock(&engine->slabs.lock);
    do_slabs_stats(engine, add_stats, c);
    pthread_mutex_unlock(&engine->slabs.lock);
}

void slabs_adjust_mem_requested(struct default_engine *engine, unsigned int id, size_t old, size_t ntotal)
{
    slabclass_t *p;

    if (id < POWER_SMALLEST || id > engine->slabs.power_largest)
        return;
    pthread_mutex_lock(&engine->slabs.lock);
    p = &engine->slabs.slabclass[id];
    p->requested = p->requested - old + ntotal;
    pthread_mutex_unlock(&engine->slabs.lock);
}

ENGINE_ERROR_CODE slabs_set_memlimit(struct default_engine *engine, size_t memlimit)
{
    ENGINE_ERROR_CODE ret;
    pthread_mutex_lock(&engine->slabs.lock);
    ret = do_slabs_set_memlimit(engine, memlimit);
    pthread_mutex_unlock(&engine->slabs.lock);
    return ret;
}
