/* tools.c - tools for slap tools */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2007 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#define AVL_INTERNAL
#include "back-bdb.h"
#include "idl.h"

static DBC *cursor = NULL;
static DBT key, data;

typedef struct dn_id {
	ID id;
	struct berval dn;
} dn_id;

#define	HOLE_SIZE	4096
static dn_id hbuf[HOLE_SIZE], *holes = hbuf;
static unsigned nhmax = HOLE_SIZE;
static unsigned nholes;

static int index_nattrs;

#ifdef BDB_TOOL_IDL_CACHING
#define bdb_tool_idl_cmp		BDB_SYMBOL(tool_idl_cmp)
#define bdb_tool_idl_flush_one		BDB_SYMBOL(tool_idl_flush_one)
#define bdb_tool_idl_flush		BDB_SYMBOL(tool_idl_flush)

static int bdb_tool_idl_flush( BackendDB *be );

#define	IDBLOCK	1024

typedef struct bdb_tool_idl_cache_entry {
	struct bdb_tool_idl_cache_entry *next;
	ID ids[IDBLOCK];
} bdb_tool_idl_cache_entry;
 
typedef struct bdb_tool_idl_cache {
	struct berval kstr;
	bdb_tool_idl_cache_entry *head, *tail;
	ID first, last;
	int count;
} bdb_tool_idl_cache;

static bdb_tool_idl_cache_entry *bdb_tool_idl_free_list;
#endif	/* BDB_TOOL_IDL_CACHING */

static ID bdb_tool_ix_id;
static Operation *bdb_tool_ix_op;
static int *bdb_tool_index_threads, bdb_tool_index_tcount;
static void *bdb_tool_index_rec;
static struct bdb_info *bdb_tool_info;
static ldap_pvt_thread_mutex_t bdb_tool_index_mutex;
static ldap_pvt_thread_cond_t bdb_tool_index_cond;

static void * bdb_tool_index_task( void *ctx, void *ptr );

int bdb_tool_entry_open(
	BackendDB *be, int mode )
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;

	/* initialize key and data thangs */
	DBTzero( &key );
	DBTzero( &data );
	key.flags = DB_DBT_REALLOC;
	data.flags = DB_DBT_REALLOC;

	if (cursor == NULL) {
		int rc = bdb->bi_id2entry->bdi_db->cursor(
			bdb->bi_id2entry->bdi_db, NULL, &cursor,
			bdb->bi_db_opflags );
		if( rc != 0 ) {
			return -1;
		}
	}

	/* Set up for threaded slapindex */
	if (( slapMode & (SLAP_TOOL_QUICK|SLAP_TOOL_READONLY)) == SLAP_TOOL_QUICK
		&& bdb->bi_nattrs ) {
		if ( !bdb_tool_info ) {
			int i;
			ldap_pvt_thread_mutex_init( &bdb_tool_index_mutex );
			ldap_pvt_thread_cond_init( &bdb_tool_index_cond );
			bdb_tool_index_threads = ch_malloc( slap_tool_thread_max * sizeof( int ));
			bdb_tool_index_rec = ch_malloc( bdb->bi_nattrs * sizeof( IndexRec ));
			bdb_tool_index_tcount = slap_tool_thread_max - 1;
			for (i=1; i<slap_tool_thread_max; i++) {
				int *ptr = ch_malloc( sizeof( int ));
				*ptr = i;
				ldap_pvt_thread_pool_submit( &connection_pool,
					bdb_tool_index_task, ptr );
			}
		}
		bdb_tool_info = bdb;
	}

	return 0;
}

int bdb_tool_entry_close(
	BackendDB *be )
{
	if ( bdb_tool_info ) {
		slapd_shutdown = 1;
		ldap_pvt_thread_mutex_lock( &bdb_tool_index_mutex );
		bdb_tool_index_tcount = slap_tool_thread_max - 1;
		ldap_pvt_thread_cond_broadcast( &bdb_tool_index_cond );
		ldap_pvt_thread_mutex_unlock( &bdb_tool_index_mutex );
	}

	if( key.data ) {
		ch_free( key.data );
		key.data = NULL;
	}
	if( data.data ) {
		ch_free( data.data );
		data.data = NULL;
	}

	if( cursor ) {
		cursor->c_close( cursor );
		cursor = NULL;
	}

#ifdef BDB_TOOL_IDL_CACHING
	bdb_tool_idl_flush( be );
#endif

	if( nholes ) {
		unsigned i;
		fprintf( stderr, "Error, entries missing!\n");
		for (i=0; i<nholes; i++) {
			fprintf(stderr, "  entry %ld: %s\n",
				holes[i].id, holes[i].dn.bv_val);
		}
		return -1;
	}
			
	return 0;
}

ID bdb_tool_entry_next(
	BackendDB *be )
{
	int rc;
	ID id;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );
	assert( bdb != NULL );
	
	rc = cursor->c_get( cursor, &key, &data, DB_NEXT );

	if( rc != 0 ) {
		/* If we're doing linear indexing and there are more attrs to
		 * index, and we're at the end of the database, start over.
		 */
		if ( index_nattrs && rc == DB_NOTFOUND ) {
			/* optional - do a checkpoint here? */
			bdb_attr_info_free( bdb->bi_attrs[0] );
			bdb->bi_attrs[0] = bdb->bi_attrs[index_nattrs];
			index_nattrs--;
			rc = cursor->c_get( cursor, &key, &data, DB_FIRST );
			if ( rc ) {
				return NOID;
			}
		} else {
			return NOID;
		}
	}

	if( data.data == NULL ) {
		return NOID;
	}

	BDB_DISK2ID( key.data, &id );
	return id;
}

ID bdb_tool_dn2id_get(
	Backend *be,
	struct berval *dn
)
{
	Operation op = {0};
	Opheader ohdr = {0};
	EntryInfo *ei = NULL;
	int rc;

	if ( BER_BVISEMPTY(dn) )
		return 0;

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	rc = bdb_cache_find_ndn( &op, NULL, dn, &ei );
	if ( ei ) bdb_cache_entryinfo_unlock( ei );
	if ( rc == DB_NOTFOUND )
		return NOID;
	
	return ei->bei_id;
}

int bdb_tool_id2entry_get(
	Backend *be,
	ID id,
	Entry **e
)
{
	int rc;
	ID nid;

	BDB_ID2DISK( id, &nid );
	key.ulen = key.size = sizeof(ID);
	key.flags = DB_DBT_USERMEM;
	key.data = &nid;

	rc = cursor->c_get( cursor, &key, &data, DB_SET );
	if ( rc == 0 ) {
		*e = bdb_tool_entry_get( be, id );
		if ( *e == NULL )
			rc = LDAP_OTHER;
	}
	key.data = NULL;

	return rc;
}

Entry* bdb_tool_entry_get( BackendDB *be, ID id )
{
	int rc;
	Entry *e = NULL;
	struct berval bv;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );
	assert( data.data != NULL );

	DBT2bv( &data, &bv );

#ifdef SLAP_ZONE_ALLOC
	/* FIXME: will add ctx later */
	rc = entry_decode( &bv, &e, NULL );
#else
	rc = entry_decode( &bv, &e );
#endif

	if( rc == LDAP_SUCCESS ) {
		e->e_id = id;
	}
#ifdef BDB_HIER
	if ( slapMode & SLAP_TOOL_READONLY ) {
		EntryInfo *ei = NULL;
		Operation op = {0};
		Opheader ohdr = {0};

		op.o_hdr = &ohdr;
		op.o_bd = be;
		op.o_tmpmemctx = NULL;
		op.o_tmpmfuncs = &ch_mfuncs;

		rc = bdb_cache_find_parent( &op, NULL, cursor->locker, id, &ei );
		if ( rc == LDAP_SUCCESS ) {
			bdb_cache_entryinfo_unlock( ei );
			e->e_private = ei;
			ei->bei_e = e;
			bdb_fix_dn( e, 0 );
			ei->bei_e = NULL;
			e->e_private = NULL;
		}
	}
#endif
	return e;
}

static int bdb_tool_next_id(
	Operation *op,
	DB_TXN *tid,
	Entry *e,
	struct berval *text,
	int hole )
{
	struct berval dn = e->e_name;
	struct berval ndn = e->e_nname;
	struct berval pdn, npdn;
	EntryInfo *ei = NULL, eidummy;
	int rc;

	if (ndn.bv_len == 0) {
		e->e_id = 0;
		return 0;
	}

	rc = bdb_cache_find_ndn( op, tid, &ndn, &ei );
	if ( ei ) bdb_cache_entryinfo_unlock( ei );
	if ( rc == DB_NOTFOUND ) {
		if ( !be_issuffix( op->o_bd, &ndn ) ) {
			ID eid = e->e_id;
			dnParent( &dn, &pdn );
			dnParent( &ndn, &npdn );
			e->e_name = pdn;
			e->e_nname = npdn;
			rc = bdb_tool_next_id( op, tid, e, text, 1 );
			e->e_name = dn;
			e->e_nname = ndn;
			if ( rc ) {
				return rc;
			}
			/* If parent didn't exist, it was created just now
			 * and its ID is now in e->e_id. Make sure the current
			 * entry gets added under the new parent ID.
			 */
			if ( eid != e->e_id ) {
				eidummy.bei_id = e->e_id;
				ei = &eidummy;
			}
		}
		rc = bdb_next_id( op->o_bd, tid, &e->e_id );
		if ( rc ) {
			snprintf( text->bv_val, text->bv_len,
				"next_id failed: %s (%d)",
				db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_next_id: %s\n", text->bv_val, 0, 0 );
			return rc;
		}
		rc = bdb_dn2id_add( op, tid, ei, e );
		if ( rc ) {
			snprintf( text->bv_val, text->bv_len, 
				"dn2id_add failed: %s (%d)",
				db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_next_id: %s\n", text->bv_val, 0, 0 );
		} else if ( hole ) {
			if ( nholes == nhmax - 1 ) {
				if ( holes == hbuf ) {
					holes = ch_malloc( nhmax * sizeof(dn_id) * 2 );
					AC_MEMCPY( holes, hbuf, sizeof(hbuf) );
				} else {
					holes = ch_realloc( holes, nhmax * sizeof(dn_id) * 2 );
				}
				nhmax *= 2;
			}
			ber_dupbv( &holes[nholes].dn, &ndn );
			holes[nholes++].id = e->e_id;
		}
	} else if ( !hole ) {
		unsigned i;

		e->e_id = ei->bei_id;

		for ( i=0; i<nholes; i++) {
			if ( holes[i].id == e->e_id ) {
				int j;
				free(holes[i].dn.bv_val);
				for (j=i;j<nholes;j++) holes[j] = holes[j+1];
				holes[j].id = 0;
				nholes--;
				break;
			} else if ( holes[i].id > e->e_id ) {
				break;
			}
		}
	}
	return rc;
}

static int
bdb_tool_index_add(
	Operation *op,
	DB_TXN *txn,
	Entry *e )
{
	struct bdb_info *bdb = (struct bdb_info *) op->o_bd->be_private;

	if (!bdb->bi_nattrs)
		return 0;

	if ( slapMode & SLAP_TOOL_QUICK ) {
		IndexRec *ir;
		int i, rc;
		Attribute *a;
		
		ir = bdb_tool_index_rec;
		memset(ir, 0, bdb->bi_nattrs * sizeof( IndexRec ));

		for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
			rc = bdb_index_recset( bdb, a, a->a_desc->ad_type, 
				&a->a_desc->ad_tags, ir );
			if ( rc )
				return rc;
		}
		bdb_tool_ix_id = e->e_id;
		bdb_tool_ix_op = op;
		ldap_pvt_thread_mutex_lock( &bdb_tool_index_mutex );
		/* Wait for all threads to be ready */
		while ( bdb_tool_index_tcount ) {
			ldap_pvt_thread_mutex_unlock( &bdb_tool_index_mutex );
			ldap_pvt_thread_yield();
			ldap_pvt_thread_mutex_lock( &bdb_tool_index_mutex );
		}
		for ( i=1; i<slap_tool_thread_max; i++ )
			bdb_tool_index_threads[i] = LDAP_BUSY;
		bdb_tool_index_tcount = slap_tool_thread_max - 1;
		ldap_pvt_thread_cond_broadcast( &bdb_tool_index_cond );
		ldap_pvt_thread_mutex_unlock( &bdb_tool_index_mutex );
		rc = bdb_index_recrun( op, bdb, ir, e->e_id, 0 );
		if ( rc )
			return rc;
		for ( i=1; i<slap_tool_thread_max; i++ ) {
			if ( bdb_tool_index_threads[i] == LDAP_BUSY ) {
				ldap_pvt_thread_yield();
				i--;
				continue;
			}
			if ( bdb_tool_index_threads[i] )
				return bdb_tool_index_threads[i];
		}
		return 0;
	} else {
		return bdb_index_entry_add( op, txn, e );
	}
}

ID bdb_tool_entry_put(
	BackendDB *be,
	Entry *e,
	struct berval *text )
{
	int rc;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB_TXN *tid = NULL;
	Operation op = {0};
	Opheader ohdr = {0};

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	assert( text != NULL );
	assert( text->bv_val != NULL );
	assert( text->bv_val[0] == '\0' );	/* overconservative? */

	Debug( LDAP_DEBUG_TRACE, "=> " LDAP_XSTRING(bdb_tool_entry_put)
		"( %ld, \"%s\" )\n", (long) e->e_id, e->e_dn, 0 );

	if (! (slapMode & SLAP_TOOL_QUICK)) {
	rc = TXN_BEGIN( bdb->bi_dbenv, NULL, &tid, 
		bdb->bi_db_opflags );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
			"txn_begin failed: %s (%d)",
			db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_put) ": %s\n",
			 text->bv_val, 0, 0 );
		return NOID;
	}
	}

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	/* add dn2id indices */
	rc = bdb_tool_next_id( &op, tid, e, text, 0 );
	if( rc != 0 ) {
		goto done;
	}

	if ( !bdb->bi_linear_index )
		rc = bdb_tool_index_add( &op, tid, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"index_entry_add failed: %s (%d)",
				db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_put) ": %s\n",
			text->bv_val, 0, 0 );
		goto done;
	}

	/* id2entry index */
	rc = bdb_id2entry_add( be, tid, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"id2entry_add failed: %s (%d)",
				db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_put) ": %s\n",
			text->bv_val, 0, 0 );
		goto done;
	}

done:
	if( rc == 0 ) {
		if ( !( slapMode & SLAP_TOOL_QUICK )) {
		rc = TXN_COMMIT( tid, 0 );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
					"txn_commit failed: %s (%d)",
					db_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(bdb_tool_entry_put) ": %s\n",
				text->bv_val, 0, 0 );
			e->e_id = NOID;
		}
		}

	} else {
		if ( !( slapMode & SLAP_TOOL_QUICK )) {
		TXN_ABORT( tid );
		snprintf( text->bv_val, text->bv_len,
			"txn_aborted! %s (%d)",
			db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_put) ": %s\n",
			text->bv_val, 0, 0 );
		}
		e->e_id = NOID;
	}

	return e->e_id;
}

int bdb_tool_entry_reindex(
	BackendDB *be,
	ID id )
{
	struct bdb_info *bi = (struct bdb_info *) be->be_private;
	int rc;
	Entry *e;
	DB_TXN *tid = NULL;
	Operation op = {0};
	Opheader ohdr = {0};

	Debug( LDAP_DEBUG_ARGS,
		"=> " LDAP_XSTRING(bdb_tool_entry_reindex) "( %ld )\n",
		(long) id, 0, 0 );

	/* No indexes configured, nothing to do. Could return an
	 * error here to shortcut things.
	 */
	if (!bi->bi_attrs) {
		return 0;
	}

	/* Get the first attribute to index */
	if (bi->bi_linear_index && !index_nattrs) {
		index_nattrs = bi->bi_nattrs - 1;
		bi->bi_nattrs = 1;
	}

	e = bdb_tool_entry_get( be, id );

	if( e == NULL ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(bdb_tool_entry_reindex)
			": could not locate id=%ld\n",
			(long) id, 0, 0 );
		return -1;
	}

	if (! (slapMode & SLAP_TOOL_QUICK)) {
	rc = TXN_BEGIN( bi->bi_dbenv, NULL, &tid, bi->bi_db_opflags );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_reindex) ": "
			"txn_begin failed: %s (%d)\n",
			db_strerror(rc), rc, 0 );
		goto done;
	}
	}
 	
	/*
	 * just (re)add them for now
	 * assume that some other routine (not yet implemented)
	 * will zap index databases
	 *
	 */

	Debug( LDAP_DEBUG_TRACE,
		"=> " LDAP_XSTRING(bdb_tool_entry_reindex) "( %ld, \"%s\" )\n",
		(long) id, e->e_dn, 0 );

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	rc = bdb_tool_index_add( &op, tid, e );

done:
	if( rc == 0 ) {
		if (! (slapMode & SLAP_TOOL_QUICK)) {
		rc = TXN_COMMIT( tid, 0 );
		if( rc != 0 ) {
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(bdb_tool_entry_reindex)
				": txn_commit failed: %s (%d)\n",
				db_strerror(rc), rc, 0 );
			e->e_id = NOID;
		}
		}

	} else {
		if (! (slapMode & SLAP_TOOL_QUICK)) {
		TXN_ABORT( tid );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_reindex)
			": txn_aborted! %s (%d)\n",
			db_strerror(rc), rc, 0 );
		}
		e->e_id = NOID;
	}
	bdb_entry_release( &op, e, 0 );

	return rc;
}

ID bdb_tool_entry_modify(
	BackendDB *be,
	Entry *e,
	struct berval *text )
{
	int rc;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB_TXN *tid = NULL;
	Operation op = {0};
	Opheader ohdr = {0};

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	assert( text != NULL );
	assert( text->bv_val != NULL );
	assert( text->bv_val[0] == '\0' );	/* overconservative? */

	assert ( e->e_id != NOID );

	Debug( LDAP_DEBUG_TRACE,
		"=> " LDAP_XSTRING(bdb_tool_entry_modify) "( %ld, \"%s\" )\n",
		(long) e->e_id, e->e_dn, 0 );

	if (! (slapMode & SLAP_TOOL_QUICK)) {
		if( cursor ) {
			cursor->c_close( cursor );
			cursor = NULL;
		}
	rc = TXN_BEGIN( bdb->bi_dbenv, NULL, &tid, 
		bdb->bi_db_opflags );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
			"txn_begin failed: %s (%d)",
			db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_modify) ": %s\n",
			 text->bv_val, 0, 0 );
		return NOID;
	}
	}

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	/* id2entry index */
	rc = bdb_id2entry_update( be, tid, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"id2entry_add failed: %s (%d)",
				db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_modify) ": %s\n",
			text->bv_val, 0, 0 );
		goto done;
	}

done:
	if( rc == 0 ) {
		if (! (slapMode & SLAP_TOOL_QUICK)) {
		rc = TXN_COMMIT( tid, 0 );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
					"txn_commit failed: %s (%d)",
					db_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(bdb_tool_entry_modify) ": "
				"%s\n", text->bv_val, 0, 0 );
			e->e_id = NOID;
		}
		}

	} else {
		if (! (slapMode & SLAP_TOOL_QUICK)) {
		TXN_ABORT( tid );
		snprintf( text->bv_val, text->bv_len,
			"txn_aborted! %s (%d)",
			db_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(bdb_tool_entry_modify) ": %s\n",
			text->bv_val, 0, 0 );
		}
		e->e_id = NOID;
	}

	return e->e_id;
}

#ifdef BDB_TOOL_IDL_CACHING
static int
bdb_tool_idl_cmp( const void *v1, const void *v2 )
{
	const bdb_tool_idl_cache *c1 = v1, *c2 = v2;
	int rc;

	if (( rc = c1->kstr.bv_len - c2->kstr.bv_len )) return rc;
	return memcmp( c1->kstr.bv_val, c2->kstr.bv_val, c1->kstr.bv_len );
}

static int
bdb_tool_idl_flush_one( void *v1, void *arg )
{
	bdb_tool_idl_cache *ic = v1;
	DB *db = arg;
	struct bdb_info *bdb = bdb_tool_info;
	bdb_tool_idl_cache_entry *ice;
	DBC *curs;
	DBT key, data;
	int i, rc;
	ID id, nid;

	/* Freshly allocated, ignore it */
	if ( !ic->head && ic->count <= BDB_IDL_DB_SIZE ) {
		return 0;
	}

	rc = db->cursor( db, NULL, &curs, 0 );
	if ( rc )
		return -1;

	DBTzero( &key );
	DBTzero( &data );

	bv2DBT( &ic->kstr, &key );

	data.size = data.ulen = sizeof( ID );
	data.flags = DB_DBT_USERMEM;
	data.data = &nid;

	rc = curs->c_get( curs, &key, &data, DB_SET );
	/* If key already exists and we're writing a range... */
	if ( rc == 0 && ic->count > BDB_IDL_DB_SIZE ) {
		/* If it's not currently a range, must delete old info */
		if ( nid ) {
			/* Skip lo */
			while ( curs->c_get( curs, &key, &data, DB_NEXT_DUP ) == 0 )
				curs->c_del( curs, 0 );

			nid = 0;
			/* Store range marker */
			curs->c_put( curs, &key, &data, DB_KEYFIRST );
		} else {
			
			/* Skip lo */
			rc = curs->c_get( curs, &key, &data, DB_NEXT_DUP );

			/* Get hi */
			rc = curs->c_get( curs, &key, &data, DB_NEXT_DUP );

			/* Delete hi */
			curs->c_del( curs, 0 );
		}
		BDB_ID2DISK( ic->last, &nid );
		curs->c_put( curs, &key, &data, DB_KEYLAST );
		rc = 0;
	} else if ( rc && rc != DB_NOTFOUND ) {
		rc = -1;
	} else if ( ic->count > BDB_IDL_DB_SIZE ) {
		/* range, didn't exist before */
		nid = 0;
		rc = curs->c_put( curs, &key, &data, DB_KEYLAST );
		if ( rc == 0 ) {
			BDB_ID2DISK( ic->first, &nid );
			rc = curs->c_put( curs, &key, &data, DB_KEYLAST );
			if ( rc == 0 ) {
				BDB_ID2DISK( ic->last, &nid );
				rc = curs->c_put( curs, &key, &data, DB_KEYLAST );
			}
		}
		if ( rc ) {
			rc = -1;
		}
	} else {
		int n;

		/* Just a normal write */
		rc = 0;
		for ( ice = ic->head, n=0; ice; ice = ice->next, n++ ) {
			int end;
			if ( ice->next ) {
				end = IDBLOCK;
			} else {
				end = ic->count & (IDBLOCK-1);
				if ( !end )
					end = IDBLOCK;
			}
			for ( i=0; i<end; i++ ) {
				if ( !ice->ids[i] ) continue;
				BDB_ID2DISK( ice->ids[i], &nid );
				rc = curs->c_put( curs, &key, &data, DB_NODUPDATA );
				if ( rc ) {
					if ( rc == DB_KEYEXIST ) {
						rc = 0;
						continue;
					}
					rc = -1;
					break;
				}
			}
			if ( rc ) {
				rc = -1;
				break;
			}
		}
		if ( ic->head ) {
			ldap_pvt_thread_mutex_lock( &bdb->bi_idl_tree_lrulock );
			ic->tail->next = bdb_tool_idl_free_list;
			bdb_tool_idl_free_list = ic->head;
			bdb->bi_idl_cache_size -= n;
			ldap_pvt_thread_mutex_unlock( &bdb->bi_idl_tree_lrulock );
		}
	}
	if ( ic != db->app_private ) {
		ch_free( ic );
	} else {
		ic->head = ic->tail = NULL;
	}
	curs->c_close( curs );
	return rc;
}

static int
bdb_tool_idl_flush_db( DB *db, bdb_tool_idl_cache *ic )
{
	Avlnode *root = db->app_private;
	int rc;

	db->app_private = ic;
	rc = avl_apply( root, bdb_tool_idl_flush_one, db, -1, AVL_INORDER );
	avl_free( root, NULL );
	db->app_private = NULL;
	if ( rc != -1 )
		rc = 0;
	return rc;
}

static int
bdb_tool_idl_flush( BackendDB *be )
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db;
	Avlnode *root;
	int i, rc = 0;

	for ( i=BDB_NDB; i < bdb->bi_ndatabases; i++ ) {
		db = bdb->bi_databases[i]->bdi_db;
		if ( !db->app_private ) continue;
		rc = bdb_tool_idl_flush_db( db, NULL );
		if ( rc )
			break;
	}
	if ( !rc ) {
		bdb->bi_idl_cache_size = 0;
	}
	return rc;
}

int bdb_tool_idl_add(
	BackendDB *be,
	DB *db,
	DB_TXN *txn,
	DBT *key,
	ID id )
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	bdb_tool_idl_cache *ic, itmp;
	bdb_tool_idl_cache_entry *ice;
	int rc;

	if ( !bdb->bi_idl_cache_max_size )
		return bdb_idl_insert_key( be, db, txn, key, id );

	DBT2bv( key, &itmp.kstr );

	ic = avl_find( (Avlnode *)db->app_private, &itmp, bdb_tool_idl_cmp );

	/* No entry yet, create one */
	if ( !ic ) {
		DBC *curs;
		DBT data;
		ID nid;
		int rc;

		ic = ch_malloc( sizeof( bdb_tool_idl_cache ) + itmp.kstr.bv_len );
		ic->kstr.bv_len = itmp.kstr.bv_len;
		ic->kstr.bv_val = (char *)(ic+1);
		AC_MEMCPY( ic->kstr.bv_val, itmp.kstr.bv_val, ic->kstr.bv_len );
		ic->head = ic->tail = NULL;
		ic->last = 0;
		ic->count = 0;
		avl_insert( (Avlnode **)&db->app_private, ic, bdb_tool_idl_cmp,
			avl_dup_error );

		/* load existing key count here */
		rc = db->cursor( db, NULL, &curs, 0 );
		if ( rc ) return rc;

		data.ulen = sizeof( ID );
		data.flags = DB_DBT_USERMEM;
		data.data = &nid;
		rc = curs->c_get( curs, key, &data, DB_SET );
		if ( rc == 0 ) {
			if ( nid == 0 ) {
				ic->count = BDB_IDL_DB_SIZE+1;
			} else {
				db_recno_t count;

				curs->c_count( curs, &count, 0 );
				ic->count = count;
				BDB_DISK2ID( &nid, &ic->first );
			}
		}
		curs->c_close( curs );
	}
	/* are we a range already? */
	if ( ic->count > BDB_IDL_DB_SIZE ) {
		ic->last = id;
		return 0;
	/* Are we at the limit, and converting to a range? */
	} else if ( ic->count == BDB_IDL_DB_SIZE ) {
		int n;
		for ( ice = ic->head, n=0; ice; ice = ice->next, n++ )
			/* counting */ ;
		if ( n ) {
			ldap_pvt_thread_mutex_lock( &bdb->bi_idl_tree_lrulock );
			ic->tail->next = bdb_tool_idl_free_list;
			bdb_tool_idl_free_list = ic->head;
			bdb->bi_idl_cache_size -= n;
			ldap_pvt_thread_mutex_unlock( &bdb->bi_idl_tree_lrulock );
		}
		ic->head = ic->tail = NULL;
		ic->last = id;
		ic->count++;
		return 0;
	}
	/* No free block, create that too */
	if ( !ic->tail || ( ic->count & (IDBLOCK-1)) == 0) {
		ice = NULL;
		ldap_pvt_thread_mutex_lock( &bdb->bi_idl_tree_lrulock );
		if ( bdb->bi_idl_cache_size >= bdb->bi_idl_cache_max_size ) {
			ldap_pvt_thread_mutex_unlock( &bdb->bi_idl_tree_lrulock );
			rc = bdb_tool_idl_flush_db( db, ic );
			if ( rc )
				return rc;
			avl_insert( (Avlnode **)&db->app_private, ic, bdb_tool_idl_cmp,
				avl_dup_error );
			ldap_pvt_thread_mutex_lock( &bdb->bi_idl_tree_lrulock );
		}
		bdb->bi_idl_cache_size++;
		if ( bdb_tool_idl_free_list ) {
			ice = bdb_tool_idl_free_list;
			bdb_tool_idl_free_list = ice->next;
		}
		ldap_pvt_thread_mutex_unlock( &bdb->bi_idl_tree_lrulock );
		if ( !ice ) {
			ice = ch_malloc( sizeof( bdb_tool_idl_cache_entry ));
		}
		memset( ice, 0, sizeof( *ice ));
		if ( !ic->head ) {
			ic->head = ice;
		} else {
			ic->tail->next = ice;
		}
		ic->tail = ice;
		if ( !ic->count )
			ic->first = id;
	}
	ice = ic->tail;
	ice->ids[ ic->count & (IDBLOCK-1) ] = id;
	ic->count++;

	return 0;
}
#endif

static void *
bdb_tool_index_task( void *ctx, void *ptr )
{
	int base = *(int *)ptr;

	free( ptr );
	while ( 1 ) {
		ldap_pvt_thread_mutex_lock( &bdb_tool_index_mutex );
		bdb_tool_index_tcount--;
		ldap_pvt_thread_cond_wait( &bdb_tool_index_cond,
			&bdb_tool_index_mutex );
		ldap_pvt_thread_mutex_unlock( &bdb_tool_index_mutex );
		if ( slapd_shutdown )
			break;

		bdb_tool_index_threads[base] = bdb_index_recrun( bdb_tool_ix_op,
			bdb_tool_info, bdb_tool_index_rec, bdb_tool_ix_id, base );
	}

	return NULL;
}
