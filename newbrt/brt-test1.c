/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."

#include "brt.h"
#include "key.h"
#include "test.h"
#include "toku_assert.h"
#include <unistd.h>

static TOKUTXN const null_txn = 0;
static DB * const null_db = 0;

static void test1 (void) {
    BRT t;
    int r;
    CACHETABLE ct;
    char fname[]="brt-test1.brt";
    DBT k,v;
    toku_memory_check=1;
    toku_memory_check_all_free();
    r = toku_brt_create_cachetable(&ct, 0, ZERO_LSN, NULL_LOGGER);
    assert(r==0);
    unlink(fname);
    r = toku_open_brt(fname, 0, 1, &t, 1024, ct, null_txn, toku_default_compare_fun, null_db);
    assert(r==0);
    toku_brt_insert(t, toku_fill_dbt(&k, "hello", 6), toku_fill_dbt(&v, "there", 6), null_txn);
    {
	r = toku_brt_lookup(t, toku_fill_dbt(&k, "hello", 6), toku_init_dbt(&v));
	assert(r==0);
	assert(strcmp(v.data, "there")==0);
	assert(v.size==6);
    }
    r = toku_close_brt(t);              assert(r==0);
    r = toku_cachetable_close(&ct);     assert(r==0);
    toku_memory_check_all_free();
    if (verbose) printf("test1 ok\n");
}
int main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
     if (verbose) printf("test1\n");
    test1();
    toku_malloc_cleanup();
    if (verbose) printf("test1 ok\n");
    return 0;
}
