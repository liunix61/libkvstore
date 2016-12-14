// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include <assert.h>
#include <stdlib.h>
#include "db_base_internal.h"
#include "liblmdb/lmdb.h"

// MDB private definition but seems unlikely to change.
// We double check it at run time and return an error if it's different.
#define MDB_MAIN_DBI 1

struct DB_env {
	DB_base const *isa;
	MDB_env *env;
	DB_cmp_data cmp[1];
	DB_cmd_data cmd[1];
};
struct DB_txn {
	DB_base const *isa;
	DB_env *env;
	DB_txn *parent;
	MDB_txn *txn;
	unsigned flags;
	DB_cursor *cursor;
};
struct DB_cursor {
	DB_base const *isa;
	DB_txn *txn;
	MDB_cursor *cursor;
};

static int mdberr(int const rc) {
	return rc <= 0 ? rc : -rc;
}

DB_FN int db__env_create(DB_env **const out) {
	MDB_env *e = NULL;
	int rc = mdberr(mdb_env_create(&e));
	if(rc < 0) return rc;
	DB_env *env = calloc(1, sizeof(DB_env));
	if(!env) {
		mdb_env_close(e);
		return DB_ENOMEM;
	}
	env->isa = db_base_mdb;
	env->env = e;
	*out = env;
	return 0;
}
DB_FN int db__env_config(DB_env *const env, unsigned const type, void *data) {
	if(!env) return DB_EINVAL;
	switch(type) {
	case DB_CFG_MAPSIZE: {
		size_t *const sp = data;
		return mdberr(mdb_env_set_mapsize(env->env, *sp));
	} case DB_CFG_COMPARE: return DB_ENOTSUP; //*env->cmp = *(DB_cmp_data *)data; return 0;
	case DB_CFG_COMMAND: *env->cmd = *(DB_cmd_data *)data; return 0;
	case DB_CFG_TXNSIZE: return 0;
	default: return DB_ENOTSUP;
	}
}
DB_FN int db__env_open(DB_env *const env, char const *const name, unsigned const flags, unsigned const mode) {
	if(!env) return DB_EINVAL;
	int rc = mdberr(mdb_env_open(env->env, name, flags | MDB_NOSUBDIR, mode));
	if(rc < 0) return rc;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	rc = mdberr(mdb_txn_begin(env->env, NULL, 0, &txn));
	if(rc < 0) goto cleanup;
	rc = mdberr(mdb_dbi_open(txn, NULL, 0, &dbi));
	if(rc < 0) goto cleanup;
	if(env->cmp->fn) {
		rc = mdberr(mdb_set_compare(txn, dbi, (MDB_cmp_func *)env->cmp->fn));
		if(rc < 0) goto cleanup;
	}
	rc = mdberr(mdb_txn_commit(txn)); txn = NULL;
	if(rc < 0) goto cleanup;
	if(MDB_MAIN_DBI != dbi) return DB_PANIC;
cleanup:
	mdb_txn_abort(txn); txn = NULL;
	return rc;
}
DB_FN void db__env_close(DB_env *const env) {
	if(!env) return;
	mdb_env_close(env->env);
	env->isa = NULL;
	env->env = NULL;
	env->cmp->fn = NULL;
	env->cmp->ctx = NULL;
	env->cmd->fn = NULL;
	env->cmd->ctx = NULL;
	free(env);
}

DB_FN int db__txn_begin(DB_env *const env, DB_txn *const parent, unsigned const flags, DB_txn **const out) {
	if(!env) return DB_EINVAL;
	if(!out) return DB_EINVAL;
	MDB_txn *const psub = parent ? parent->txn : NULL;
	MDB_txn *subtxn;
	int rc = mdberr(mdb_txn_begin(env->env, psub, flags, &subtxn));
	if(rc < 0) return rc;
	DB_txn *txn = malloc(sizeof(struct DB_txn));
	if(!txn) {
		mdb_txn_abort(subtxn);
		return DB_ENOMEM;
	}
	txn->isa = db_base_mdb;
	txn->env = env;
	txn->parent = parent;
	txn->txn = subtxn;
	txn->flags = flags;
	txn->cursor = NULL;
	*out = txn;
	return 0;
}
DB_FN int db__txn_commit(DB_txn *const txn) {
	if(!txn) return DB_EINVAL;
	db_cursor_close(txn->cursor);
	int rc = mdberr(mdb_txn_commit(txn->txn));
	txn->isa = NULL;
	txn->env = NULL;
	free(txn);
	return rc;
}
DB_FN void db__txn_abort(DB_txn *const txn) {
	if(!txn) return;
	db_cursor_close(txn->cursor);
	mdb_txn_abort(txn->txn);
	txn->isa = NULL;
	txn->env = NULL;
	txn->parent = NULL;
	free(txn);
}
DB_FN void db__txn_reset(DB_txn *const txn) {
	if(!txn) return;
	mdb_txn_reset(txn->txn);
}
DB_FN int db__txn_renew(DB_txn *const txn) {
	if(!txn) return DB_EINVAL;
	int rc = mdberr(mdb_txn_renew(txn->txn));
	if(rc < 0) return rc;
	if(txn->cursor) {
		rc = db_cursor_renew(txn, &txn->cursor);
		if(rc < 0) return rc;
	}
	return 0;
}
DB_FN int db__txn_env(DB_txn *const txn, DB_env **const out) {
	if(!txn) return DB_EINVAL;
	if(out) *out = txn->env;
	return 0;
}
DB_FN int db__txn_parent(DB_txn *const txn, DB_txn **const out) {
	if(!txn) return DB_EINVAL;
	if(out) *out = txn->parent;
	return 0;
}
DB_FN int db__txn_get_flags(DB_txn *const txn, unsigned *const flags) {
	if(!txn) return DB_EINVAL;
	if(flags) *flags = txn->flags;
	return 0;
}
DB_FN int db__txn_cmp(DB_txn *const txn, DB_val const *const a, DB_val const *const b) {
	return mdb_cmp(txn->txn, MDB_MAIN_DBI, (MDB_val *)a, (MDB_val *)b);
}
DB_FN int db__txn_cursor(DB_txn *const txn, DB_cursor **const out) {
	if(!txn) return DB_EINVAL;
	if(!txn->cursor) {
		int rc = db_cursor_renew(txn, &txn->cursor);
		if(rc < 0) return rc;
	}
	if(out) *out = txn->cursor;
	return 0;
}

// Use our own cursor for these rather than mdb_get/put
// because otherwise MDB has to construct its own temporary cursor
// on the stack, which is just wasteful if we might need it again.
DB_FN int db__get(DB_txn *const txn, DB_val *const key, DB_val *const data) {
	return db_helper_get(txn, key, data);
}
DB_FN int db__put(DB_txn *const txn, DB_val *const key, DB_val *const data, unsigned const flags) {
	return db_helper_put(txn, key, data, flags);
}
DB_FN int db__del(DB_txn *const txn, DB_val *const key, unsigned const flags) {
	return db_helper_del(txn, key, flags);
}
DB_FN int db__cmd(DB_txn *const txn, unsigned char const *const buf, size_t const len) {
	if(!txn) return DB_EINVAL;
	if(!txn->env->cmd->fn) return DB_EINVAL;
	return txn->env->cmd->fn(txn->env->cmd->ctx, txn, buf, len);
}

DB_FN int db__cursor_open(DB_txn *const txn, DB_cursor **const out) {
	if(!txn) return DB_EINVAL;
	MDB_cursor *c = NULL;
	int rc = mdberr(mdb_cursor_open(txn->txn, MDB_MAIN_DBI, &c));
	if(rc < 0) return rc;
	DB_cursor *cursor = calloc(1, sizeof(DB_cursor));
	if(!cursor) {
		mdb_cursor_close(c);
		return DB_ENOMEM;
	}
	cursor->isa = db_base_mdb;
	cursor->txn = txn;
	cursor->cursor = c;
	*out = cursor;
	return 0;
}
DB_FN void db__cursor_close(DB_cursor *const cursor) {
	if(!cursor) return;
	mdb_cursor_close(cursor->cursor);
	cursor->isa = NULL;
	cursor->txn = NULL;
	free(cursor);
}
DB_FN void db__cursor_reset(DB_cursor *const cursor) {
	// Do nothing.
}
DB_FN int db__cursor_renew(DB_txn *const txn, DB_cursor **const out) {
	if(!txn) return DB_EINVAL;
	if(!out) return DB_EINVAL;
	if(*out) {
		out[0]->txn = txn;
		return mdberr(mdb_cursor_renew(txn->txn, out[0]->cursor));
	}
	return db_cursor_open(txn, out);
}
DB_FN int db__cursor_clear(DB_cursor *const cursor) {
	if(!cursor) return DB_EINVAL;
	MDB_cursor *const c = cursor->cursor;
	int rc = mdberr(mdb_cursor_renew(mdb_cursor_txn(c), c));
	if(DB_EINVAL == rc) rc = 0;
	return rc;
}
DB_FN int db__cursor_txn(DB_cursor *const cursor, DB_txn **const out) {
	if(!cursor) return DB_EINVAL;
	if(out) *out = cursor->txn;
	return 0;
}
DB_FN int db__cursor_cmp(DB_cursor *const cursor, DB_val const *const a, DB_val const *const b) {
	assert(cursor);
	return mdb_cmp(mdb_cursor_txn(cursor->cursor), MDB_MAIN_DBI, (MDB_val const *)a, (MDB_val const *)b);
}

DB_FN int db__cursor_current(DB_cursor *const cursor, DB_val *const key, DB_val *const data) {
	if(!cursor) return DB_EINVAL;
	int rc = mdberr(mdb_cursor_get(cursor->cursor, (MDB_val *)key, (MDB_val *)data, MDB_GET_CURRENT));
	if(DB_EINVAL == rc) return DB_NOTFOUND;
	return rc;
}
DB_FN int db__cursor_seek(DB_cursor *const cursor, DB_val *const key, DB_val *const data, int const dir) {
	if(!cursor) return DB_EINVAL;
	if(!key) return DB_EINVAL;
	MDB_cursor *const c = cursor->cursor;
	MDB_val *const k = (MDB_val *)key;
	MDB_val *const d = (MDB_val *)data;
	MDB_val const orig = *k;
	MDB_cursor_op const op = 0 == dir ? MDB_SET : MDB_SET_RANGE;
	int rc = mdberr(mdb_cursor_get(c, k, d, op));
	if(dir >= 0) return rc;
	if(rc >= 0) {
		MDB_txn *const txn = mdb_cursor_txn(c);
		if(0 == mdb_cmp(txn, MDB_MAIN_DBI, &orig, k)) return rc;
		return mdb_cursor_get(c, k, d, MDB_PREV);
	} else if(DB_NOTFOUND == rc) {
		return mdberr(mdb_cursor_get(c, k, d, MDB_LAST));
	} else return rc;
}
DB_FN int db__cursor_first(DB_cursor *const cursor, DB_val *const key, DB_val *const data, int const dir) {
	if(!cursor) return DB_EINVAL;
	if(0 == dir) return DB_EINVAL;
	MDB_cursor_op const op = dir < 0 ? MDB_LAST : MDB_FIRST;
	MDB_val _k[1], _d[1];
	MDB_val *const k = key ? (MDB_val *)key : _k;
	MDB_val *const d = data ? (MDB_val *)data : _d;
	return mdberr(mdb_cursor_get(cursor->cursor, (MDB_val *)k, (MDB_val *)d, op));
}
DB_FN int db__cursor_next(DB_cursor *const cursor, DB_val *const key, DB_val *const data, int const dir) {
	if(!cursor) return DB_EINVAL;
	if(0 == dir) return DB_EINVAL;
	MDB_cursor_op const op = dir < 0 ? MDB_PREV : MDB_NEXT;
	MDB_val _k[1], _d[1];
	MDB_val *const k = key ? (MDB_val *)key : _k;
	MDB_val *const d = data ? (MDB_val *)data : _d;
	return mdberr(mdb_cursor_get(cursor->cursor, (MDB_val *)k, (MDB_val *)d, op));
}

DB_FN int db__cursor_seekr(DB_cursor *const cursor, DB_range const *const range, DB_val *const key, DB_val *const data, int const dir) {
	return db_helper_cursor_seekr(cursor, range, key, data, dir);
}
DB_FN int db__cursor_firstr(DB_cursor *const cursor, DB_range const *const range, DB_val *const key, DB_val *const data, int const dir) {
	return db_helper_cursor_firstr(cursor, range, key, data, dir);
}
DB_FN int db__cursor_nextr(DB_cursor *const cursor, DB_range const *const range, DB_val *const key, DB_val *const data, int const dir) {
	return db_helper_cursor_nextr(cursor, range, key, data, dir);
}

DB_FN int db__cursor_put(DB_cursor *const cursor, DB_val *const key, DB_val *const data, unsigned const flags) {
	if(!cursor) return DB_EINVAL;
	MDB_val null = { 0, NULL };
	MDB_val *const k = (MDB_val *)key;
	MDB_val *const d = data ? (MDB_val *)data : &null;
	return mdberr(mdb_cursor_put(cursor->cursor, k, d, flags));
}
DB_FN int db__cursor_del(DB_cursor *const cursor, unsigned const flags) {
	if(!cursor) return DB_EINVAL;
	if(flags) return DB_EINVAL;
	return mdberr(mdb_cursor_del(cursor->cursor, 0));
}

DB_BASE_V0(mdb)

