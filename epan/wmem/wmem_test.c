/* wmem_test.c
 * Wireshark Memory Manager Tests
 * Copyright 2012, Evan Huus <eapache@gmail.com>
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <glib.h>
#include "wmem.h"
#include "wmem_allocator.h"
#include "wmem_allocator_block.h"
#include "wmem_allocator_simple.h"
#include "wmem_allocator_strict.h"
#include "config.h"

#define MAX_ALLOC_SIZE          (1024*64)
#define MAX_SIMULTANEOUS_ALLOCS  1024
#define CONTAINER_ITERS          10000

typedef void (*wmem_verify_func)(wmem_allocator_t *allocator);

/* A local copy of wmem_allocator_new that ignores the
 * WIRESHARK_DEBUG_WMEM_OVERRIDE variable so that test functions are
 * guaranteed to actually get the allocator type they asked for */
static wmem_allocator_t *
wmem_allocator_force_new(const wmem_allocator_type_t type)
{
    wmem_allocator_t      *allocator;

    allocator = g_slice_new(wmem_allocator_t);
    allocator->type = type;
    allocator->callbacks = NULL;

    switch (type) {
        case WMEM_ALLOCATOR_SIMPLE:
            wmem_simple_allocator_init(allocator);
            break;
        case WMEM_ALLOCATOR_BLOCK:
            wmem_block_allocator_init(allocator);
            break;
        case WMEM_ALLOCATOR_STRICT:
            wmem_strict_allocator_init(allocator);
            break;
        default:
            g_assert_not_reached();
            /* This is necessary to squelch MSVC errors; is there
	       any way to tell it that g_assert_not_reached()
	       never returns? */
            return NULL;
    };

    return allocator;
}

/* A helper for generating pseudo-random strings. Just uses glib's random number
 * functions to generate 'numbers' in the printable character range. */
static gchar *
wmem_test_rand_string(wmem_allocator_t *allocator, gint minlen, gint maxlen)
{
    gchar *str;
    gint len, i;

    len = g_random_int_range(minlen, maxlen);

    /* +1 for null-terminator */
    str = (gchar*)wmem_alloc(allocator, len + 1);
    str[len] = '\0';

    for (i=0; i<len; i++) {
        /* ASCII normal printable range is 32 (space) to 126 (tilde) */
        str[i] = (gchar) g_random_int_range(32, 126);
    }

    return str;
}

/* Some helpers for properly testing callback functionality */
wmem_allocator_t *expected_allocator;
void             *expected_user_data;
wmem_cb_event_t   expected_event;
int               cb_called_count;
int               cb_continue_count;
gboolean          value_seen[CONTAINER_ITERS];

static gboolean
wmem_test_cb(wmem_allocator_t *allocator, wmem_cb_event_t event,
        void *user_data)
{
    g_assert(allocator == expected_allocator);
    g_assert(event     == expected_event);

    cb_called_count++;

    return *(gboolean*)user_data;
}

static gboolean
wmem_test_foreach_cb(void *value, void *user_data)
{
    g_assert(user_data == expected_user_data);

    g_assert(! value_seen[GPOINTER_TO_INT(value)]);
    value_seen[GPOINTER_TO_INT(value)] = TRUE;

    cb_called_count++;
    cb_continue_count--;

    return (cb_continue_count == 0);
}

/* ALLOCATOR TESTING FUNCTIONS (/wmem/allocator/) */

static void
wmem_test_allocator_callbacks(void)
{
    wmem_allocator_t *allocator;
    gboolean t = TRUE;
    gboolean f = FALSE;
    guint    cb_id;

    allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    expected_allocator = allocator;

    wmem_register_callback(expected_allocator, &wmem_test_cb, &f);
    wmem_register_callback(expected_allocator, &wmem_test_cb, &f);
    cb_id = wmem_register_callback(expected_allocator, &wmem_test_cb, &t);
    wmem_register_callback(expected_allocator, &wmem_test_cb, &t);
    wmem_register_callback(expected_allocator, &wmem_test_cb, &f);

    expected_event = WMEM_CB_FREE_EVENT;

    cb_called_count = 0;
    wmem_free_all(allocator);
    g_assert(cb_called_count == 5);

    cb_called_count = 0;
    wmem_free_all(allocator);
    g_assert(cb_called_count == 2);

    cb_called_count = 0;
    wmem_free_all(allocator);
    g_assert(cb_called_count == 2);

    wmem_unregister_callback(allocator, cb_id);
    cb_called_count = 0;
    wmem_free_all(allocator);
    g_assert(cb_called_count == 1);

    cb_id = wmem_register_callback(expected_allocator, &wmem_test_cb, &f);
    wmem_register_callback(expected_allocator, &wmem_test_cb, &t);

    cb_called_count = 0;
    wmem_free_all(allocator);
    g_assert(cb_called_count == 3);

    wmem_unregister_callback(allocator, cb_id);
    cb_called_count = 0;
    wmem_free_all(allocator);
    g_assert(cb_called_count == 2);

    wmem_register_callback(expected_allocator, &wmem_test_cb, &t);

    expected_event = WMEM_CB_DESTROY_EVENT;
    cb_called_count = 0;
    wmem_destroy_allocator(allocator);
    g_assert(cb_called_count == 3);
}

static void
wmem_test_allocator(wmem_allocator_type_t type, wmem_verify_func verify)
{
    int i;
    char *ptrs[MAX_SIMULTANEOUS_ALLOCS];
    wmem_allocator_t *allocator;

    allocator = wmem_allocator_force_new(type);

    if (verify) (*verify)(allocator);

    /* start with some fairly simple deterministic tests */

    /* we use wmem_alloc0 in part because it tests slightly more code, but
     * primarily so that if the allocator doesn't give us enough memory or
     * gives us memory that includes its own metadata, we write to it and
     * things go wrong, causing the tests to fail */
    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        ptrs[i] = (char *)wmem_alloc0(allocator, 8);
    }
    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        wmem_free(allocator, ptrs[i]);
    }

    if (verify) (*verify)(allocator);
    wmem_free_all(allocator);
    wmem_gc(allocator);
    if (verify) (*verify)(allocator);

    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        ptrs[i] = (char *)wmem_alloc0(allocator, 64);
    }
    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        wmem_free(allocator, ptrs[i]);
    }

    if (verify) (*verify)(allocator);
    wmem_free_all(allocator);
    wmem_gc(allocator);
    if (verify) (*verify)(allocator);

    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        ptrs[i] = (char *)wmem_alloc0(allocator, 512);
    }
    for (i=MAX_SIMULTANEOUS_ALLOCS-1; i>=0; i--) {
        /* no wmem_realloc0 so just use memset manually */
        ptrs[i] = (char *)wmem_realloc(allocator, ptrs[i], MAX_ALLOC_SIZE);
        memset(ptrs[i], 0, MAX_ALLOC_SIZE);
    }
    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        wmem_free(allocator, ptrs[i]);
    }

    if (verify) (*verify)(allocator);
    wmem_free_all(allocator);
    wmem_gc(allocator);
    if (verify) (*verify)(allocator);

    /* now do some random fuzz-like tests */

    /* reset our ptr array */
    for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
        ptrs[i] = NULL;
    }

    /* Run ~64,000 iterations */
    for (i=0; i<1024*64; i++) {
        gint ptrs_index;
        gint new_size;

        /* returns value 0 <= x < MAX_SIMULTANEOUS_ALLOCS which is a valid
         * index into ptrs */
        ptrs_index = g_test_rand_int_range(0, MAX_SIMULTANEOUS_ALLOCS);

        if (ptrs[ptrs_index] == NULL) {
            /* if that index is unused, allocate some random amount of memory
             * between 0 and MAX_ALLOC_SIZE */
            new_size = g_test_rand_int_range(0, MAX_ALLOC_SIZE);

            ptrs[ptrs_index] = (char *) wmem_alloc0(allocator, new_size);
        }
        else if (g_test_rand_bit()) {
            /* the index is used, and our random bit has determined we will be
             * reallocating instead of freeing. Do so to some random size
             * between 0 and MAX_ALLOC_SIZE, then manually zero the
             * new memory */
            new_size = g_test_rand_int_range(0, MAX_ALLOC_SIZE);

            ptrs[ptrs_index] = (char *) wmem_realloc(allocator,
                    ptrs[ptrs_index], new_size);

            memset(ptrs[ptrs_index], 0, new_size);
        }
        else {
            /* the index is used, and our random bit has determined we will be
             * freeing instead of reallocating. Do so and NULL the pointer for
             * the next iteration. */
            wmem_free(allocator, ptrs[ptrs_index]);
            ptrs[ptrs_index] = NULL;
        }
        if (verify) (*verify)(allocator);
    }

    wmem_destroy_allocator(allocator);
}

static void
wmem_time_allocator(wmem_allocator_type_t type)
{
    int i, j;
    wmem_allocator_t *allocator;

    allocator = wmem_allocator_force_new(type);

    for (j=0; j<128; j++) {
        for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
            wmem_alloc(allocator, 8);
        }
        wmem_free_all(allocator);

        for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
            wmem_alloc(allocator, 256);
        }
        wmem_free_all(allocator);

        for (i=0; i<MAX_SIMULTANEOUS_ALLOCS; i++) {
            wmem_alloc(allocator, 1024);
        }
    }

    wmem_destroy_allocator(allocator);
}

static void
wmem_time_allocators(void)
{
    double simple_time, block_time;

    g_test_timer_start();
    wmem_time_allocator(WMEM_ALLOCATOR_SIMPLE);
    simple_time = g_test_timer_elapsed();

    g_test_timer_start();
    wmem_time_allocator(WMEM_ALLOCATOR_BLOCK);
    block_time = g_test_timer_elapsed();

    printf("(simple: %lf; block: %lf) ", simple_time, block_time);
    g_assert(simple_time > block_time);
}

static void
wmem_test_allocator_block(void)
{
    wmem_test_allocator(WMEM_ALLOCATOR_BLOCK, &wmem_block_verify);
}

static void
wmem_test_allocator_simple(void)
{
    wmem_test_allocator(WMEM_ALLOCATOR_SIMPLE, NULL);
}

static void
wmem_test_allocator_strict(void)
{
    wmem_test_allocator(WMEM_ALLOCATOR_STRICT, &wmem_strict_check_canaries);
}

/* UTILITY TESTING FUNCTIONS (/wmem/utils/) */

static void
wmem_test_miscutls(void)
{
    wmem_allocator_t   *allocator;
    const char         *source = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char               *ret;

    allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    ret = (char*) wmem_memdup(allocator, source, 5);
    ret[4] = '\0';
    g_assert_cmpstr(ret, ==, "ABCD");

    ret = (char*) wmem_memdup(allocator, source, 1);
    g_assert(ret[0] == 'A');
    wmem_strict_check_canaries(allocator);

    ret = (char*) wmem_memdup(allocator, source, 10);
    ret[9] = '\0';
    g_assert_cmpstr(ret, ==, "ABCDEFGHI");

    wmem_destroy_allocator(allocator);
}

static void
wmem_test_strutls(void)
{
    wmem_allocator_t   *allocator;
    const char         *orig_str;
    char               *new_str;

    allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    orig_str = "TEST1";
    new_str  = wmem_strdup(allocator, orig_str);
    g_assert_cmpstr(new_str, ==, orig_str);
    new_str[0] = 'X';
    g_assert_cmpstr(new_str, >, orig_str);
    wmem_strict_check_canaries(allocator);

    orig_str = "TEST123456789";
    new_str  = wmem_strndup(allocator, orig_str, 6);
    g_assert_cmpstr(new_str, ==, "TEST12");
    g_assert_cmpstr(new_str, <, orig_str);
    new_str[0] = 'X';
    g_assert_cmpstr(new_str, >, orig_str);
    wmem_strict_check_canaries(allocator);

    new_str = wmem_strdup_printf(allocator, "abc %s %% %d", "boo", 23);
    g_assert_cmpstr(new_str, ==, "abc boo % 23");
    wmem_strict_check_canaries(allocator);

    wmem_destroy_allocator(allocator);
}

/* DATA STRUCTURE TESTING FUNCTIONS (/wmem/datastruct/) */

static void
wmem_test_slist(void)
{
    wmem_allocator_t   *allocator;
    wmem_slist_t       *slist;
    wmem_slist_frame_t *frame;
    unsigned int        i;

    allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    slist = wmem_slist_new(allocator);
    g_assert(slist);
    g_assert(wmem_slist_count(slist) == 0);

    frame = wmem_slist_front(slist);
    g_assert(frame == NULL);

    for (i=0; i<CONTAINER_ITERS; i++) {
        wmem_slist_prepend(slist, GINT_TO_POINTER(i));
        g_assert(wmem_slist_count(slist) == i+1);

        frame = wmem_slist_front(slist);
        g_assert(frame);
        g_assert(wmem_slist_frame_data(frame) == GINT_TO_POINTER(i));
    }
    wmem_strict_check_canaries(allocator);

    i = CONTAINER_ITERS - 1;
    frame = wmem_slist_front(slist);
    while (frame) {
        g_assert(wmem_slist_frame_data(frame) == GINT_TO_POINTER(i));
        i--;
        frame = wmem_slist_frame_next(frame);
    }

    i = CONTAINER_ITERS - 2;
    while (wmem_slist_count(slist) > 1) {
        wmem_slist_remove(slist, GINT_TO_POINTER(i));
        i--;
    }
    wmem_slist_remove(slist, GINT_TO_POINTER(CONTAINER_ITERS - 1));
    g_assert(wmem_slist_count(slist) == 0);
    g_assert(wmem_slist_front(slist) == NULL);

    for (i=0; i<CONTAINER_ITERS; i++) {
        wmem_slist_append(slist, GINT_TO_POINTER(i));
        g_assert(wmem_slist_count(slist) == i+1);

        frame = wmem_slist_front(slist);
        g_assert(frame);
    }
    wmem_strict_check_canaries(allocator);

    i = 0;
    frame = wmem_slist_front(slist);
    while (frame) {
        g_assert(wmem_slist_frame_data(frame) == GINT_TO_POINTER(i));
        i++;
        frame = wmem_slist_frame_next(frame);
    }

    wmem_destroy_allocator(allocator);
}

static void
wmem_test_stack(void)
{
    wmem_allocator_t   *allocator;
    wmem_stack_t       *stack;
    unsigned int        i;

    allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    stack = wmem_stack_new(allocator);
    g_assert(stack);
    g_assert(wmem_stack_count(stack) == 0);

    for (i=0; i<CONTAINER_ITERS; i++) {
        wmem_stack_push(stack, GINT_TO_POINTER(i));

        g_assert(wmem_stack_count(stack) == i+1);
        g_assert(wmem_stack_peek(stack) == GINT_TO_POINTER(i));
    }
    wmem_strict_check_canaries(allocator);

    for (i=CONTAINER_ITERS; i>0; i--) {
        g_assert(wmem_stack_peek(stack) == GINT_TO_POINTER(i-1));
        g_assert(wmem_stack_pop(stack) == GINT_TO_POINTER(i-1));
        g_assert(wmem_stack_count(stack) == i-1);
    }
    g_assert(wmem_stack_count(stack) == 0);

    wmem_destroy_allocator(allocator);
}

static void
wmem_test_strbuf(void)
{
    wmem_allocator_t   *allocator;
    wmem_strbuf_t      *strbuf;
    int                 i;

    allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    strbuf = wmem_strbuf_new(allocator, "TEST");
    g_assert(strbuf);
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TEST");
    g_assert(wmem_strbuf_get_len(strbuf) == 4);

    wmem_strbuf_append(strbuf, "FUZZ");
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TESTFUZZ");
    g_assert(wmem_strbuf_get_len(strbuf) == 8);

    wmem_strbuf_append_printf(strbuf, "%d%s", 3, "a");
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TESTFUZZ3a");
    g_assert(wmem_strbuf_get_len(strbuf) == 10);

    wmem_strbuf_append_c(strbuf, 'q');
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TESTFUZZ3aq");
    g_assert(wmem_strbuf_get_len(strbuf) == 11);

    wmem_strbuf_append_unichar(strbuf, g_utf8_get_char("\xC2\xA9"));
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TESTFUZZ3aq\xC2\xA9");
    g_assert(wmem_strbuf_get_len(strbuf) == 13);

    wmem_strbuf_truncate(strbuf, 32);
    wmem_strbuf_truncate(strbuf, 24);
    wmem_strbuf_truncate(strbuf, 16);
    wmem_strbuf_truncate(strbuf, 13);
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TESTFUZZ3aq\xC2\xA9");
    g_assert(wmem_strbuf_get_len(strbuf) == 13);

    wmem_strbuf_truncate(strbuf, 3);
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "TES");
    g_assert(wmem_strbuf_get_len(strbuf) == 3);

    strbuf = wmem_strbuf_sized_new(allocator, 10, 10);
    g_assert(strbuf);
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "");
    g_assert(wmem_strbuf_get_len(strbuf) == 0);

    wmem_strbuf_append(strbuf, "FUZZ");
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "FUZZ");
    g_assert(wmem_strbuf_get_len(strbuf) == 4);

    wmem_strbuf_append_printf(strbuf, "%d%s", 3, "abcdefghijklmnop");
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "FUZZ3abcd");
    g_assert(wmem_strbuf_get_len(strbuf) == 9);

    wmem_strbuf_append(strbuf, "abcdefghijklmnopqrstuvwxyz");
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "FUZZ3abcd");
    g_assert(wmem_strbuf_get_len(strbuf) == 9);

    wmem_strbuf_append_c(strbuf, 'q');
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "FUZZ3abcd");
    g_assert(wmem_strbuf_get_len(strbuf) == 9);

    wmem_strbuf_append_unichar(strbuf, g_utf8_get_char("\xC2\xA9"));
    g_assert_cmpstr(wmem_strbuf_get_str(strbuf), ==, "FUZZ3abcd");
    g_assert(wmem_strbuf_get_len(strbuf) == 9);

    wmem_free_all(allocator);

    strbuf = wmem_strbuf_new(allocator, "TEST");
    for (i=0; i<1024; i++) {
        if (g_test_rand_bit()) {
            wmem_strbuf_append(strbuf, "ABC");
        }
        else {
            wmem_strbuf_append_printf(strbuf, "%d%d", 3, 777);
        }
        wmem_strict_check_canaries(allocator);
    }
    g_assert(strlen(wmem_strbuf_get_str(strbuf)) ==
             wmem_strbuf_get_len(strbuf));

    wmem_destroy_allocator(allocator);
}

static void
wmem_test_tree(void)
{
    wmem_allocator_t   *allocator, *extra_allocator;
    wmem_tree_t        *tree;
    guint32             i;
    int                 seen_values = 0;
    int                 j;
    gchar              *str_key;
#define WMEM_TREE_MAX_KEY_COUNT 2
    int                 key_count;
    wmem_tree_key_t     keys[WMEM_TREE_MAX_KEY_COUNT+1];

    allocator       = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);
    extra_allocator = wmem_allocator_force_new(WMEM_ALLOCATOR_STRICT);

    tree = wmem_tree_new(allocator);
    g_assert(tree);

    /* test basic 32-bit key operations */
    for (i=0; i<CONTAINER_ITERS; i++) {
        g_assert(wmem_tree_lookup32(tree, i) == NULL);
        if (i > 0) {
            g_assert(wmem_tree_lookup32_le(tree, i) == GINT_TO_POINTER(i-1));
        }
        wmem_tree_insert32(tree, i, GINT_TO_POINTER(i));
        g_assert(wmem_tree_lookup32(tree, i) == GINT_TO_POINTER(i));
    }
    wmem_free_all(allocator);

    tree = wmem_tree_new(allocator);
    for (i=0; i<CONTAINER_ITERS; i++) {
        guint32 rand = g_test_rand_int();
        wmem_tree_insert32(tree, rand, GINT_TO_POINTER(i));
        g_assert(wmem_tree_lookup32(tree, rand) == GINT_TO_POINTER(i));
    }
    wmem_free_all(allocator);

    /* test auto-reset functionality */
    tree = wmem_tree_new_autoreset(allocator, extra_allocator);
    for (i=0; i<CONTAINER_ITERS; i++) {
        g_assert(wmem_tree_lookup32(tree, i) == NULL);
        wmem_tree_insert32(tree, i, GINT_TO_POINTER(i));
        g_assert(wmem_tree_lookup32(tree, i) == GINT_TO_POINTER(i));
    }
    wmem_free_all(extra_allocator);
    for (i=0; i<CONTAINER_ITERS; i++) {
        g_assert(wmem_tree_lookup32(tree, i) == NULL);
        g_assert(wmem_tree_lookup32_le(tree, i) == NULL);
    }
    wmem_free_all(allocator);

    /* test array key functionality */
    tree = wmem_tree_new(allocator);
    for (i=0; i<CONTAINER_ITERS; i++) {
        key_count = g_random_int_range(1, WMEM_TREE_MAX_KEY_COUNT);
        keys[key_count].length = 0;
        for (j=0; j<key_count; j++) {
            keys[j].key    = (guint32*)wmem_test_rand_string(allocator, 8, 9);
            keys[j].length = 2;
        }
        g_assert(wmem_tree_lookup32_array(tree, keys) == NULL);
        wmem_tree_insert32_array(tree, keys, GINT_TO_POINTER(i));
        g_assert(wmem_tree_lookup32_array(tree, keys) == GINT_TO_POINTER(i));
    }
    wmem_free_all(allocator);

    /* test string key functionality */
    tree = wmem_tree_new(allocator);
    for (i=0; i<CONTAINER_ITERS; i++) {
        str_key = wmem_test_rand_string(allocator, 1, 64);
        wmem_tree_insert_string(tree, str_key, GINT_TO_POINTER(i), 0);
        g_assert(wmem_tree_lookup_string(tree, str_key, 0) ==
                GINT_TO_POINTER(i));
    }
    wmem_free_all(allocator);

    tree = wmem_tree_new(allocator);
    for (i=0; i<CONTAINER_ITERS; i++) {
        str_key = wmem_test_rand_string(allocator, 1, 64);
        wmem_tree_insert_string(tree, str_key, GINT_TO_POINTER(i),
                WMEM_TREE_STRING_NOCASE);
        g_assert(wmem_tree_lookup_string(tree, str_key,
                    WMEM_TREE_STRING_NOCASE) == GINT_TO_POINTER(i));
    }
    wmem_free_all(allocator);

    /* test for-each functionality */
    tree = wmem_tree_new(allocator);
    expected_user_data = GINT_TO_POINTER(g_test_rand_int());
    for (i=0; i<CONTAINER_ITERS; i++) {
        value_seen[i] = FALSE;
        wmem_tree_insert32(tree, g_test_rand_int(), GINT_TO_POINTER(i));
    }

    cb_called_count    = 0;
    cb_continue_count  = CONTAINER_ITERS;
    wmem_tree_foreach(tree, wmem_test_foreach_cb, expected_user_data);
    g_assert(cb_called_count   == CONTAINER_ITERS);
    g_assert(cb_continue_count == 0);

    for (i=0; i<CONTAINER_ITERS; i++) {
        g_assert(value_seen[i]);
        value_seen[i] = FALSE;
    }

    cb_called_count    = 0;
    cb_continue_count  = 10;
    wmem_tree_foreach(tree, wmem_test_foreach_cb, expected_user_data);
    g_assert(cb_called_count   == 10);
    g_assert(cb_continue_count == 0);

    for (i=0; i<CONTAINER_ITERS; i++) {
        if (value_seen[i]) {
            seen_values++;
        }
    }
    g_assert(seen_values == 10);

    wmem_destroy_allocator(extra_allocator);
    wmem_destroy_allocator(allocator);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/wmem/allocator/block",     wmem_test_allocator_block);
    g_test_add_func("/wmem/allocator/simple",    wmem_test_allocator_simple);
    g_test_add_func("/wmem/allocator/strict",    wmem_test_allocator_strict);
    g_test_add_func("/wmem/allocator/times",     wmem_time_allocators);
    g_test_add_func("/wmem/allocator/callbacks", wmem_test_allocator_callbacks);

    g_test_add_func("/wmem/utils/misc",    wmem_test_miscutls);
    g_test_add_func("/wmem/utils/strings", wmem_test_strutls);

    g_test_add_func("/wmem/datastruct/slist",  wmem_test_slist);
    g_test_add_func("/wmem/datastruct/stack",  wmem_test_stack);
    g_test_add_func("/wmem/datastruct/strbuf", wmem_test_strbuf);
    g_test_add_func("/wmem/datastruct/tree",   wmem_test_tree);

    return g_test_run();
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
