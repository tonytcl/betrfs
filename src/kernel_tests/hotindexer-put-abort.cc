/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*
COPYING CONDITIONS NOTICE:

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation, and provided that the
  following conditions are met:

      * Redistributions of source code must retain this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below).

      * Redistributions in binary form must reproduce this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below) in the documentation and/or other materials
        provided with the distribution.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.

COPYRIGHT NOTICE:

  TokuDB, Tokutek Fractal Tree Indexing Library.
  Copyright (C) 2007-2013 Tokutek, Inc.

DISCLAIMER:

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

UNIVERSITY PATENT NOTICE:

  The technology is licensed by the Massachusetts Institute of
  Technology, Rutgers State University of New Jersey, and the Research
  Foundation of State University of New York at Stony Brook under
  United States of America Serial No. 11/760379 and to the patents
  and/or patent applications resulting from it.

PATENT MARKING NOTICE:

  This software is covered by US Patent No. 8,185,551.
  This software is covered by US Patent No. 8,489,638.

PATENT RIGHTS GRANT:

  "THIS IMPLEMENTATION" means the copyrightable works distributed by
  Tokutek as part of the Fractal Tree project.

  "PATENT CLAIMS" means the claims of patents that are owned or
  licensable by Tokutek, both currently or in the future; and that in
  the absence of this license would be infringed by THIS
  IMPLEMENTATION or by using or running THIS IMPLEMENTATION.

  "PATENT CHALLENGE" shall mean a challenge to the validity,
  patentability, enforceability and/or non-infringement of any of the
  PATENT CLAIMS or otherwise opposing any of the PATENT CLAIMS.

  Tokutek hereby grants to you, for the term and geographical scope of
  the PATENT CLAIMS, a non-exclusive, no-charge, royalty-free,
  irrevocable (except as stated in this section) patent license to
  make, have made, use, offer to sell, sell, import, transfer, and
  otherwise run, modify, and propagate the contents of THIS
  IMPLEMENTATION, where such license applies only to the PATENT
  CLAIMS.  This grant does not include claims that would be infringed
  only as a consequence of further modifications of THIS
  IMPLEMENTATION.  If you or your agent or licensee institute or order
  or agree to the institution of patent litigation against any entity
  (including a cross-claim or counterclaim in a lawsuit) alleging that
  THIS IMPLEMENTATION constitutes direct or contributory patent
  infringement, or inducement of patent infringement, then any rights
  granted to you under this License shall terminate as of the date
  such litigation is filed.  If you or your agent or exclusive
  licensee institute or order or agree to the institution of a PATENT
  CHALLENGE, then Tokutek may terminate any rights granted to you
  under this License.
*/

#ident "Copyright (c) 2007-2013 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."
#include "test.h"
#include "ydb.h"
#include "toku_pthread.h"

// this test reproduces the rollback log corruption that occurs when hot indexing runs concurrent with a long abort
// the concurrent operation occurs when the abort periodically releases the ydb lock which allows the hot indexer
// to run.  the hot indexer erroneously append to the rollback log that is in the process of being aborted.

static int
put_callback(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, DBT_ARRAY *dest_vals, const DBT *src_key, const DBT *src_val) {
    toku_dbt_array_resize(dest_keys, 1);
    toku_dbt_array_resize(dest_vals, 1);
    DBT *dest_key = &dest_keys->dbts[0];
    DBT *dest_val = &dest_vals->dbts[0];

    (void) dest_db; (void) src_db; (void) dest_keys; (void) dest_vals; (void) src_key; (void) src_val;
    lazy_assert(src_db != NULL && dest_db != NULL);

    if (dest_key->flags == DB_DBT_REALLOC) {
        dest_key->data = toku_realloc(dest_key->data, src_val->size);
        memcpy(dest_key->data, src_val->data, src_val->size);
        dest_key->size = src_val->size;
    }
    dest_val->size = 0;
    
    return 0;
}

struct indexer_arg {
    DB_ENV *env;
    DB *src_db;
    int n_dest_db;
    DB **dest_db;
};

static void *
indexer_thread(void *arg) {
    struct indexer_arg *indexer_arg = (struct indexer_arg *) arg;
    DB_ENV *env = indexer_arg->env;
    int r;
    
    DB_TXN *indexer_txn = NULL;
    r = env->txn_begin(env, NULL, &indexer_txn, 0); assert_zero(r);
        
    DB_INDEXER *indexer = NULL;
    r = env->create_indexer(env, indexer_txn, &indexer, indexer_arg->src_db, indexer_arg->n_dest_db, indexer_arg->dest_db, NULL, 0); assert_zero(r);
    
    r = indexer->build(indexer); assert_zero(r);
        
    r = indexer->close(indexer); assert_zero(r);
        
    r = indexer_txn->commit(indexer_txn, 0); assert_zero(r);

    return arg;
}

static void
verify_empty(DB_ENV *env, DB *db) {
    int r;
    
    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0); assert_zero(r);

    DBC *cursor = NULL;
    r = db->cursor(db, txn, &cursor, 0); assert_zero(r);

    DBT key, val;
    r = cursor->c_get(cursor, dbt_init(&key, 0, 0), dbt_init(&val, 0, 0), DB_NEXT);
    assert(r == DB_NOTFOUND);
    
    r = cursor->c_close(cursor); assert_zero(r);

    r = txn->commit(txn, 0); assert_zero(r);
}

static void
run_test(void) {
    int r;
    DB_ENV *env = NULL;
    r = db_env_create(&env, 0); assert_zero(r);

    r = env->set_generate_row_callback_for_put(env, put_callback); assert_zero(r);

    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB *src_db = NULL;
    r = db_create(&src_db, env, 0); assert_zero(r);
    r = src_db->open(src_db, NULL, "0.tdb", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB *dest_db = NULL;
    r = db_create(&dest_db, env, 0); assert_zero(r);
    r = dest_db->open(dest_db, NULL, "1.tdb", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0); assert_zero(r);

    // insert some
    for (int i = 0; i < 246723; i++) {
        int k = htonl(i);
        int v = i;
        DBT key; dbt_init(&key, &k, sizeof k);
        DBT val; dbt_init(&val, &v, sizeof v);
        r = src_db->put(src_db, txn, &key, &val, 0); assert_zero(r);
    }

    // run the indexer
    struct indexer_arg indexer_arg = { env, src_db, 1, &dest_db };
    toku_pthread_t pid;
    r = toku_pthread_create(&pid, NULL, indexer_thread, &indexer_arg); assert_zero(r);

    r = txn->abort(txn); assert_zero(r);

    void *ret;
    r = toku_pthread_join(pid, &ret); assert_zero(r);

    verify_empty(env, src_db);
    verify_empty(env, dest_db);

    r = src_db->close(src_db, 0); assert_zero(r);

    r = dest_db->close(dest_db, 0); assert_zero(r);

    r = env->close(env, 0); assert_zero(r);
}

extern "C" int test_hotindexer_put_abort(void);
int test_hotindexer_put_abort(void) {
    int r;

    for (int i = 1; i < argc; i++) {
        char * const arg = argv[i];
        if (strcmp(arg, "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(arg, "-q") == 0) {
            verbose = 0;
            continue;
        }
    }

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    run_test();

    return 0;
}

