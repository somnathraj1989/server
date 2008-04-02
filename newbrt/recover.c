/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."

/* Recover an env.  The logs are in argv[1].  The new database is created in the cwd. */

// Test:
//    cd ../src/tests/tmpdir
//    ../../../newbrt/recover ../dir.test_log2.c.tdb

#include "cachetable.h"
#include "key.h"
#include "log-internal.h"
#include "log_header.h"
#include "toku_assert.h"
#include "kv-pair.h"
#include "gpma-internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

//#define DO_VERIFY_COUNTS
#ifdef DO_VERIFY_COUNTS
#define VERIFY_COUNTS(n) toku_verify_counts(n)
#else
#define VERIFY_COUNTS(n) ((void)0)
#endif

static DB * const null_db=0;

// These data structures really should be part of a recovery data structure.  Recovery could be multithreaded (on different environments...)  But this is OK since recovery can only happen in one
static CACHETABLE ct;
static struct cf_pair {
    FILENUM filenum;
    CACHEFILE cf;
    BRT       brt; // set to zero on an fopen, but filled in when an fheader is seen.
} *cf_pairs;
static int n_cf_pairs=0, max_cf_pairs=0;;

int toku_recover_init (void) {
    int r = toku_create_cachetable(&ct, 1<<25, (LSN){0}, 0);
    return r;
}

void toku_recover_cleanup (void) {
    int i;
    for (i=0; i<n_cf_pairs; i++) {
	if (cf_pairs[i].brt) {
	    int r = toku_close_brt(cf_pairs[i].brt);
	    //r = toku_cachefile_close(&cf_pairs[i].cf);
	    assert(r==0);
	}
    }
    toku_free(cf_pairs);
    {
	int r = toku_cachetable_close(&ct);
	assert(r==0);
    }
}

void toku_recover_commit (LSN UU(lsn), TXNID UU(txnid)) {
}

void toku_recover_fcreate (LSN UU(lsn), TXNID UU(txnid),BYTESTRING fname,u_int32_t mode) {
    char *fixed_fname = fixup_fname(&fname);
    int fd = creat(fixed_fname, mode);
    assert(fd>=0);
    toku_free(fixed_fname);
    toku_free_BYTESTRING(fname);
}

int toku_recover_note_cachefile (FILENUM fnum, CACHEFILE cf, BRT brt) {
    if (max_cf_pairs==0) {
	n_cf_pairs=1;
	max_cf_pairs=2;
	MALLOC_N(max_cf_pairs, cf_pairs);
	if (cf_pairs==0) return errno;
    } else {
	if (n_cf_pairs>=max_cf_pairs) {
	    max_cf_pairs*=2;
	    cf_pairs = toku_realloc(cf_pairs, max_cf_pairs*sizeof(*cf_pairs));
	}
	n_cf_pairs++;
    }
    cf_pairs[n_cf_pairs-1].filenum = fnum;
    cf_pairs[n_cf_pairs-1].cf      = cf;
    cf_pairs[n_cf_pairs-1].brt     = brt;
    return 0;
}

static int find_cachefile (FILENUM fnum, struct cf_pair **cf_pair) {
    int i;
    for (i=0; i<n_cf_pairs; i++) {
	if (fnum.fileid==cf_pairs[i].filenum.fileid) {
	    *cf_pair = cf_pairs+i;
	    return 0;
	}
    }
    return 1;
}

static void toku_recover_fheader (LSN UU(lsn), TXNID UU(txnid),FILENUM filenum,LOGGEDBRTHEADER header) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    struct brt_header *MALLOC(h);
    assert(h);
    h->dirty=0;
    h->flags = header.flags;
    h->nodesize = header.nodesize;
    h->freelist = header.freelist;
    h->unused_memory = header.unused_memory;
    h->n_named_roots = header.n_named_roots;
    if ((signed)header.n_named_roots==-1) {
	h->unnamed_root = header.u.one.root;
    } else {
	assert(0);
    }
    toku_cachetable_put(pair->cf, 0, h, 0, toku_brtheader_flush_callback, toku_brtheader_fetch_callback, 0);
    if (pair->brt) {
	free(pair->brt->h);
    }  else {
	MALLOC(pair->brt);
	pair->brt->cf = pair->cf;
	pair->brt->database_name = 0; // Special case, we don't know or care what the database name is for recovery.
	list_init(&pair->brt->cursors);
	pair->brt->compare_fun = 0;
	pair->brt->dup_compare = 0;
	pair->brt->db = 0;
	pair->brt->skey = pair->brt->sval = 0;
    }
    pair->brt->h = h;
    pair->brt->nodesize = h->nodesize;
    pair->brt->flags    = h->nodesize;
    r = toku_unpin_brt_header(pair->brt);
    assert(r==0);
}

void toku_recover_newbrtnode (LSN lsn, FILENUM filenum,DISKOFF diskoff,u_int32_t height,u_int32_t nodesize,u_int8_t is_dup_sort,u_int32_t rand4fingerprint) {
    int r;
    struct cf_pair *pair = NULL;
    r = find_cachefile(filenum, &pair);
    assert(r==0);
    TAGMALLOC(BRTNODE, n);
    n->nodesize     = nodesize;
    n->thisnodename = diskoff;
    n->log_lsn = n->disk_lsn  = lsn;
    //printf("%s:%d %p->disk_lsn=%"PRId64"\n", __FILE__, __LINE__, n, n->disk_lsn.lsn);
    n->layout_version = 3;
    n->height         = height;
    n->rand4fingerprint = rand4fingerprint;
    n->flags = is_dup_sort ? TOKU_DB_DUPSORT : 0; // Don't have TOKU_DB_DUP ???
    n->local_fingerprint = 0; // nothing there yet
    n->dirty = 1;
    if (height==0) {
	r=toku_gpma_create(&n->u.l.buffer, 0);
	assert(r==0);
	n->u.l.n_bytes_in_buffer=0;
	{
	    void *mp = toku_malloc(n->nodesize);
	    assert(mp);
	    toku_mempool_init(&n->u.l.buffer_mempool, mp, n->nodesize);
	}

    } else {
	n->u.n.n_children = 0;
	n->u.n.totalchildkeylens = 0;
	n->u.n.n_bytes_in_buffers = 0;
	MALLOC_N(3,n->u.n.childinfos);
	MALLOC_N(2,n->u.n.childkeys);
    }
    // Now put it in the cachetable
    toku_cachetable_put(pair->cf, diskoff, n, toku_serialize_brtnode_size(n),  toku_brtnode_flush_callback, toku_brtnode_fetch_callback, 0);

    VERIFY_COUNTS(n);

    n->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(n));
    assert(r==0);
}

static void recover_setup_node (FILENUM filenum, DISKOFF diskoff, CACHEFILE *cf, BRTNODE *resultnode) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    assert(pair->brt);
    void *node_v;
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    *resultnode = node;
    *cf = pair->cf;
}

void toku_recover_brtdeq (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, TXNID xid, u_int32_t typ, BYTESTRING key, BYTESTRING data, u_int32_t oldfingerprint, u_int32_t newfingerprint) {
    CACHEFILE cf;
    BRTNODE node;
    int r;
    recover_setup_node(filenum, diskoff, &cf, &node);
    assert(node->height>0);
    //printf("deq: %lld expected_old_fingerprint=%08x actual=%08x new=%08x\n", diskoff, oldfingerprint, node->local_fingerprint, newfingerprint);
    assert(node->local_fingerprint==oldfingerprint);
    bytevec actual_key, actual_data;
    ITEMLEN actual_keylen, actual_datalen;
    u_int32_t actual_type;
    TXNID   actual_xid;
    assert(childnum<(u_int32_t)node->u.n.n_children);
    r = toku_fifo_peek(BNC_BUFFER(node, childnum), &actual_key, &actual_keylen, &actual_data, &actual_datalen, &actual_type, &actual_xid);
    assert(r==0);
    assert(actual_keylen==(ITEMLEN)key.len);
    assert(memcmp(actual_key, key.data, actual_keylen)==0);
    assert(actual_datalen=data.len);
    assert(memcmp(actual_data, data.data, actual_datalen)==0);
    assert(actual_type==typ);
    assert(actual_xid==xid);
    u_int32_t sizediff = key.len + data.len + KEY_VALUE_OVERHEAD + BRT_CMD_OVERHEAD;
    node->local_fingerprint = newfingerprint;
    node->log_lsn = lsn;
    node->u.n.n_bytes_in_buffers -= sizediff;
    BNC_NBYTESINBUF(node, childnum) -= sizediff;
    r = toku_fifo_deq(BNC_BUFFER(node, childnum)); // don't deq till were' done looking at the data.
    r = toku_cachetable_unpin(cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
    toku_free(key.data);
    toku_free(data.data);
}

void toku_recover_brtenq (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, TXNID xid, u_int32_t typ, BYTESTRING key, BYTESTRING data, u_int32_t oldfingerprint, u_int32_t newfingerprint) {
    CACHEFILE cf;
    BRTNODE node;
    int r;
    recover_setup_node(filenum, diskoff, &cf, &node);
    assert(node->height>0);
    //printf("enq: %lld expected_old_fingerprint=%08x actual=%08x new=%08x\n", diskoff, oldfingerprint, node->local_fingerprint, newfingerprint);
    assert(node->local_fingerprint==oldfingerprint);
    r = toku_fifo_enq(BNC_BUFFER(node, childnum), key.data, key.len, data.data, data.len, typ, xid);
    assert(r==0);
    node->local_fingerprint = newfingerprint;
    node->log_lsn = lsn;
    u_int32_t sizediff = key.len + data.len + KEY_VALUE_OVERHEAD + BRT_CMD_OVERHEAD;
    r = toku_cachetable_unpin(cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
    node->u.n.n_bytes_in_buffers += sizediff;
    BNC_NBYTESINBUF(node, childnum) += sizediff;
    toku_free(key.data);
    toku_free(data.data);
}

void toku_recover_addchild (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, DISKOFF child, u_int32_t childfingerprint) {
    CACHEFILE cf;
    BRTNODE node;
    recover_setup_node(filenum, diskoff, &cf, &node);
    assert(node->height>0);
    assert(childnum <= (unsigned)node->u.n.n_children);
    unsigned int i;
    REALLOC_N(node->u.n.n_children+1, node->u.n.childinfos);
    REALLOC_N(node->u.n.n_children, node->u.n.childkeys);
    for (i=node->u.n.n_children; i>childnum; i--) {
	node->u.n.childinfos[i]=node->u.n.childinfos[i-1];
	BNC_NBYTESINBUF(node,i) = BNC_NBYTESINBUF(node,i-1);
	assert(i>=2);
	node->u.n.childkeys [i-1]      = node->u.n.childkeys [i-2];
    }
    if (childnum>0) {
	node->u.n.childkeys [childnum-1] = 0;
    }
    BNC_DISKOFF(node, childnum) = child;
    BNC_SUBTREE_FINGERPRINT(node, childnum) = childfingerprint;
    int r= toku_fifo_create(&BNC_BUFFER(node, childnum)); assert(r==0);
    BNC_NBYTESINBUF(node, childnum) = 0;
    node->u.n.n_children++;
    node->log_lsn = lsn;
    r = toku_cachetable_unpin(cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
}

void toku_recover_delchild (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, DISKOFF child, u_int32_t childfingerprint, BYTESTRING pivotkey) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height>0);

    assert(childnum < (unsigned)node->u.n.n_children);
    assert(node->u.n.childinfos[childnum].subtree_fingerprint == childfingerprint);
    assert(BNC_DISKOFF(node, childnum)==child);
    assert(toku_fifo_n_entries(BNC_BUFFER(node,childnum))==0);
    assert(BNC_NBYTESINBUF(node,childnum)==0);
    assert(node->u.n.n_children>2); // Must be at least two children.
    u_int32_t i;
    assert(childnum>0);
    node->u.n.totalchildkeylens -= toku_brt_pivot_key_len(pair->brt, node->u.n.childkeys[childnum-1]);
    toku_free((void*)node->u.n.childkeys[childnum-1]);
    toku_fifo_free(&BNC_BUFFER(node,childnum));
    for (i=childnum+1; i<(unsigned)node->u.n.n_children; i++) {
	node->u.n.childinfos[i-1] = node->u.n.childinfos[i];
	BNC_NBYTESINBUF(node,i-1) = BNC_NBYTESINBUF(node,i);
	node->u.n.childkeys[i-2] = node->u.n.childkeys[i-1];
    }
    node->u.n.n_children--;

    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
    toku_free(pivotkey.data);
}

void toku_recover_setchild (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, DISKOFF UU(oldchild), DISKOFF newchild) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height>0);
    assert(childnum < (unsigned)node->u.n.n_children);
    BNC_DISKOFF(node, childnum) = newchild;
    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
}
void toku_recover_setpivot (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, BYTESTRING pivotkey) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height>0);
    
    struct kv_pair *new_pivot = kv_pair_malloc(pivotkey.data, pivotkey.len, 0, 0);

    node->u.n.childkeys[childnum] = new_pivot;
    node->u.n.totalchildkeylens += toku_brt_pivot_key_len(pair->brt, node->u.n.childkeys[childnum]);

    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);

    toku_free(pivotkey.data);
}

void toku_recover_changechildfingerprint (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t childnum, u_int32_t UU(oldfingerprint), u_int32_t newfingerprint) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height>0);
    assert((signed)childnum <= node->u.n.n_children); // we allow the childnum to be one too large.
    BNC_SUBTREE_FINGERPRINT(node, childnum) = newfingerprint;
    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
    
}

void toku_recover_fopen (LSN UU(lsn), TXNID UU(txnid), BYTESTRING fname, FILENUM filenum) {
    char *fixedfname = fixup_fname(&fname);
    CACHEFILE cf;
    int fd = open(fixedfname, O_RDWR, 0);
    assert(fd>=0);
    BRT MALLOC(brt);
    assert(errno==0 && brt!=0);
    brt->database_name = fixedfname;
    brt->h=0;
    list_init(&brt->cursors);
    brt->compare_fun = 0;
    brt->dup_compare = 0;
    brt->db = 0;
    int r = toku_cachetable_openfd(&cf, ct, fd, brt);
    assert(r==0);
    brt->skey = brt->sval = 0;
    brt->cf=cf;
    toku_recover_note_cachefile(filenum, cf, brt);
    toku_free_BYTESTRING(fname);
}

void toku_recover_insertinleaf (LSN lsn, TXNID UU(txnid), FILENUM filenum, DISKOFF diskoff, u_int32_t pmaidx, BYTESTRING keybs, BYTESTRING databs) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height==0);
    VERIFY_COUNTS(node);
    struct kv_pair *kvp = brtnode_malloc_kv_pair(node->u.l.buffer, &node->u.l.buffer_mempool, keybs.data, keybs.len, databs.data, databs.len);
    assert(pair);
    toku_gpma_set_at_index(node->u.l.buffer, pmaidx, kv_pair_size(kvp), kvp);
    node->local_fingerprint += node->rand4fingerprint*toku_calccrc32_kvpair(keybs.data, keybs.len, databs.data, databs.len);
//    printf("%s:%d local_fingerprint=%08x (this=%08x)\n", __FILE__, __LINE__, node->local_fingerprint, toku_calccrc32_kvpair(keybs.data, keybs.len, databs.data, databs.len));
    node->u.l.n_bytes_in_buffer += PMA_ITEM_OVERHEAD + KEY_VALUE_OVERHEAD + keybs.len + databs.len; 

//    PMA_ITERATE_IDX(node->u.l.buffer, idx, skey, keylen __attribute__((__unused__)), sdata, datalen __attribute__((__unused__)),
//		    printf("%d: %s %s\n", idx, (char*)skey, (char*)sdata));

    VERIFY_COUNTS(node);

    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
    toku_free_BYTESTRING(keybs);
    toku_free_BYTESTRING(databs);
}

void toku_recover_deleteinleaf (LSN lsn, TXNID UU(txnid), FILENUM filenum, DISKOFF diskoff, u_int32_t pmaidx, BYTESTRING keybs, BYTESTRING databs) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height==0);
    VERIFY_COUNTS(node);
    {
	u_int32_t len;
	void *data;
	r = toku_gpma_get_from_index(node->u.l.buffer, pmaidx, &len, &data);
	if (r==0) {
	    toku_mempool_mfree(&node->u.l.buffer_mempool, data, len);
	}
    }
    toku_gpma_clear_at_index(node->u.l.buffer, pmaidx);
    node->local_fingerprint -= node->rand4fingerprint*toku_calccrc32_kvpair(keybs.data, keybs.len, databs.data, databs.len);
    node->u.l.n_bytes_in_buffer -= PMA_ITEM_OVERHEAD + KEY_VALUE_OVERHEAD + keybs.len + databs.len; 
    VERIFY_COUNTS(node);
    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
    toku_free_BYTESTRING(keybs);
    toku_free_BYTESTRING(databs);
}

// a newbrtnode should have been done before this
void toku_recover_resizepma (LSN lsn, FILENUM filenum, DISKOFF diskoff, u_int32_t oldsize __attribute__((__unused__)), u_int32_t newsize) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_v;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin (pair->cf, diskoff, &node_v, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE node = node_v;
    assert(node->height==0);
    r = toku_resize_gpma_exactly (node->u.l.buffer, newsize);
    assert(r==0);
    
    VERIFY_COUNTS(node);

    node->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, diskoff, 1, toku_serialize_brtnode_size(node));
    assert(r==0);
}

int move_indices (GPMA from, GPMA to, INTPAIRARRAY fromto,
		  u_int32_t a_rand, u_int32_t *a_fp,
		  u_int32_t b_rand, u_int32_t *b_fp,
		  u_int32_t *a_nbytes, u_int32_t *b_nbytes) {
    struct gitem *MALLOC_N(fromto.size, items);
    if (items==0) return errno;
    u_int32_t i;
    u_int32_t fp=0;
    u_int32_t sizediff=0;
    for (i=0; i<fromto.size; i++) {
	int idx = fromto.array[i].a;
	struct gitem item = from->items[idx];
	items[i]=item;
	from->items[idx].data = 0;
	fp += toku_crc32(toku_null_crc, item.data, item.len);
	sizediff += PMA_ITEM_OVERHEAD + item.len;
    }
    for (i=0; i<fromto.size; i++) {
	to->items[fromto.array[i].b] = items[i];
    }
    *a_fp -= a_rand * fp;
    *b_fp += b_rand * fp;
    *a_nbytes -= sizediff;
    *b_nbytes += sizediff;
    toku_free(items);
    return 0;
}

void toku_recover_pmadistribute (LSN lsn, FILENUM filenum, DISKOFF old_diskoff, DISKOFF new_diskoff, INTPAIRARRAY fromto, u_int32_t old_N __attribute__((__unused__)), u_int32_t new_N) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    void *node_va, *node_vb;
    assert(pair->brt);
    r = toku_cachetable_get_and_pin(pair->cf, old_diskoff, &node_va, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    r = toku_cachetable_get_and_pin(pair->cf, new_diskoff, &node_vb, NULL, toku_brtnode_flush_callback, toku_brtnode_fetch_callback, pair->brt);
    assert(r==0);
    BRTNODE nodea = node_va;      assert(nodea->height==0);
    BRTNODE nodeb = node_vb;      assert(nodeb->height==0);
    if (new_N!=toku_gpma_index_limit(nodeb->u.l.buffer)) {
	r = toku_resize_gpma_exactly(nodeb->u.l.buffer, new_N);
	assert(r==0);
    }
    {    
	unsigned int i;
	//printf("{");
	for (i=0; i<fromto.size; i++) {
	    //printf(" {%d %d}", fromto.array[i].a, fromto.array[i].b);
	    assert(fromto.array[i].a < toku_gpma_index_limit(nodea->u.l.buffer));
	    assert(fromto.array[i].b < toku_gpma_index_limit(nodeb->u.l.buffer));
	}
	//printf("}\n");
    }
    r = move_indices (nodea->u.l.buffer, nodeb->u.l.buffer, fromto,
		      nodea->rand4fingerprint, &nodea->local_fingerprint,
		      nodeb->rand4fingerprint, &nodeb->local_fingerprint,
		      &nodea->u.l.n_bytes_in_buffer, &nodeb->u.l.n_bytes_in_buffer
		      );
    // The bytes in buffer and fingerprint shouldn't change

//    PMA_ITERATE_IDX(nodeb->u.l.buffer, idx, key, keylen __attribute__((__unused__)), data, datalen __attribute__((__unused__)),
//		    printf("%d: %s %s\n", idx, (char*)key, (char*)data));

    VERIFY_COUNTS(nodea);
    VERIFY_COUNTS(nodeb);

    nodea->log_lsn = lsn;
    nodeb->log_lsn = lsn;
    r = toku_cachetable_unpin(pair->cf, old_diskoff, 1, toku_serialize_brtnode_size(nodea));
    assert(r==0);
    r = toku_cachetable_unpin(pair->cf, new_diskoff, 1, toku_serialize_brtnode_size(nodeb));
    assert(r==0);


    toku_free_INTPAIRARRAY(fromto);
}

void toku_recover_changeunnamedroot (LSN UU(lsn), FILENUM filenum, DISKOFF UU(oldroot), DISKOFF newroot) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    assert(pair->brt);
    r = toku_read_and_pin_brt_header(pair->cf, &pair->brt->h);
    assert(r==0);
    pair->brt->h->unnamed_root = newroot;
    r = toku_unpin_brt_header(pair->brt);
}
void toku_recover_changenamedroot (LSN UU(lsn), FILENUM UU(filenum), BYTESTRING UU(name), DISKOFF UU(oldroot), DISKOFF UU(newroot)) { assert(0); }

void toku_recover_changeunusedmemory (LSN UU(lsn), FILENUM filenum, DISKOFF UU(oldunused), DISKOFF newunused) {
    struct cf_pair *pair = NULL;
    int r = find_cachefile(filenum, &pair);
    assert(r==0);
    assert(pair->brt);
    r = toku_read_and_pin_brt_header(pair->cf, &pair->brt->h);
    assert(r==0);
    pair->brt->h->unused_memory = newunused;
    r = toku_unpin_brt_header(pair->brt);
}

static int toku_recover_checkpoint (LSN UU(lsn)) {
    return 0;
}

static int toku_recover_xbegin (LSN UU(lsn), TXNID UU(parent)) {
    return 0;
}

int tokudb_recover(const char *data_dir, const char *log_dir) {
    int r;
    int entrycount=0;
    char **logfiles;

    int lockfd;

    {
	int namelen=strlen(data_dir);
	char lockfname[namelen+20];

	snprintf(lockfname, sizeof(lockfname), "%s/__recoverylock_dont_delete_me", data_dir);
	lockfd = open(lockfname, O_RDWR|O_CREAT, S_IRUSR | S_IWUSR);
	if (lockfd<0) {
	    printf("Couldn't open %s\n", lockfname);
	    return errno;
	}
	r=flock(lockfd, LOCK_EX | LOCK_NB);
	if (r!=0) {
	    printf("Couldn't run recovery because some other process holds the recovery lock %s\n", lockfname);
	    return errno;
	}
    }

    r = toku_logger_find_logfiles(log_dir, &logfiles);
    if (r!=0) return r;
    int i;
    toku_recover_init();
    char org_wd[1000];
    {
	char *wd=getcwd(org_wd, sizeof(org_wd));
	assert(wd!=0);
	//printf("%s:%d org_wd=\"%s\"\n", __FILE__, __LINE__, org_wd);
    }
    char data_wd[1000];
    {
	r=chdir(data_dir); assert(r==0);
	char *wd=getcwd(data_wd, sizeof(data_wd));
	assert(wd!=0);
	//printf("%s:%d data_wd=\"%s\"\n", __FILE__, __LINE__, data_wd);
    }
    for (i=0; logfiles[i]; i++) {
	//fprintf(stderr, "Opening %s\n", logfiles[i]);
	r=chdir(org_wd);
	assert(r==0);
	FILE *f = fopen(logfiles[i], "r");
	struct log_entry le;
	u_int32_t version;
	//printf("Reading file %s\n", logfiles[i]);
	r=toku_read_and_print_logmagic(f, &version);
	assert(r==0 && version==0);
	r=chdir(data_wd);
	assert(r==0);
	while ((r = toku_log_fread(f, &le))==0) {
	    //printf("%lld: Got cmd %c\n", (long long)le.u.commit.lsn.lsn, le.cmd);
	    logtype_dispatch_args(&le, toku_recover_);
	    entrycount++;
	}
	if (r!=EOF) {
	    if (r==DB_BADFORMAT) {
		fprintf(stderr, "Bad log format at record %d\n", entrycount);
		return r;
	    } else {
		fprintf(stderr, "Huh? %s\n", strerror(r));
		return r;
	    }
	}
	fclose(f);
    }
    toku_recover_cleanup();
    for (i=0; logfiles[i]; i++) {
	toku_free(logfiles[i]);
    }
    toku_free(logfiles);

    r=flock(lockfd, LOCK_UN);
    if (r!=0) return errno;

    r=chdir(org_wd);
    if (r!=0) return errno;

    //printf("%s:%d recovery successful! ls -l says\n", __FILE__, __LINE__);
    //system("ls -l");
    return 0;
}
