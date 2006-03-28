/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2006 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by Kurt Spanier for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/stdlib.h>

#include <ac/ctype.h>
#include <ac/param.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/unistd.h>
#include <ac/wait.h>

#include <ldap.h>
#include <lutil.h>

#include "slapd-common.h"

#define LOOPS	100
#define RETRIES	0

static void
do_read( char *uri, char *manager, struct berval *passwd,
	char *entry, LDAP **ld, int noattrs, int maxloop,
	int maxretries, int delay, int force, int chaserefs );

static void
do_random( char *uri, char *manager, struct berval *passwd,
	char *sbase, char *filter, int noattrs,
	int innerloop, int maxretries, int delay, int force, int chaserefs );

static void
usage( char *name )
{
        fprintf( stderr,
		"usage: %s "
		"-H <uri> | ([-h <host>] -p <port>) "
		"-D <manager> "
		"-w <passwd> "
		"-e <entry> "
		"[-A] "
		"[-C] "
		"[-F] "
		"[-f filter] "
		"[-l <loops>] "
		"[-L <outerloops>] "
		"[-r <maxretries>] "
		"[-t <delay>]\n",
		name );
	exit( EXIT_FAILURE );
}

int
main( int argc, char **argv )
{
	int		i;
	char		*uri = NULL;
	char		*host = "localhost";
	int		port = -1;
	char		*manager = NULL;
	struct berval	passwd = { 0, NULL };
	char		*entry = NULL;
	char		*filter  = NULL;
	int		loops = LOOPS;
	int		outerloops = 1;
	int		retries = RETRIES;
	int		delay = 0;
	int		force = 0;
	int		chaserefs = 0;
	int		noattrs = 0;

	tester_init( "slapd-read" );

	while ( (i = getopt( argc, argv, "ACD:H:h:p:e:Ff:l:L:r:t:w:" )) != EOF ) {
		switch( i ) {
		case 'A':
			noattrs++;
			break;

		case 'C':
			chaserefs++;
			break;

		case 'H':		/* the server uri */
			uri = strdup( optarg );
			break;

		case 'h':		/* the servers host */
			host = strdup( optarg );
			break;

		case 'p':		/* the servers port */
			if ( lutil_atoi( &port, optarg ) != 0 ) {
				usage( argv[0] );
			}
			break;

		case 'D':		/* the servers manager */
			manager = strdup( optarg );
			break;

		case 'w':		/* the server managers password */
			passwd.bv_val = strdup( optarg );
			passwd.bv_len = strlen( optarg );
			break;

		case 'e':		/* DN to search for */
			entry = strdup( optarg );
			break;

		case 'f':		/* the search request */
			filter = strdup( optarg );
			break;

		case 'F':
			force++;
			break;

		case 'l':		/* the number of loops */
			if ( lutil_atoi( &loops, optarg ) != 0 ) {
				usage( argv[0] );
			}
			break;

		case 'L':		/* the number of outerloops */
			if ( lutil_atoi( &outerloops, optarg ) != 0 ) {
				usage( argv[0] );
			}
			break;

		case 'r':		/* the number of retries */
			if ( lutil_atoi( &retries, optarg ) != 0 ) {
				usage( argv[0] );
			}
			break;

		case 't':		/* delay in seconds */
			if ( lutil_atoi( &delay, optarg ) != 0 ) {
				usage( argv[0] );
			}
			break;

		default:
			usage( argv[0] );
			break;
		}
	}

	if (( entry == NULL ) || ( port == -1 && uri == NULL ))
		usage( argv[0] );

	if ( *entry == '\0' ) {
		fprintf( stderr, "%s: invalid EMPTY entry DN.\n",
				argv[0] );
		exit( EXIT_FAILURE );
	}

	uri = tester_uri( uri, host, port );

	for ( i = 0; i < outerloops; i++ ) {
		if ( filter != NULL ) {
			do_random( uri, manager, &passwd, entry, filter,
				noattrs, loops, retries, delay, force,
				chaserefs );

		} else {
			do_read( uri, manager, &passwd, entry, NULL, noattrs,
				loops, retries, delay, force, chaserefs );
		}
	}

	exit( EXIT_SUCCESS );
}

static void
do_random( char *uri, char *manager, struct berval *passwd,
	char *sbase, char *filter, int noattrs,
	int innerloop, int maxretries, int delay, int force, int chaserefs )
{
	LDAP	*ld = NULL;
	int  	i = 0, do_retry = maxretries;
	char	*attrs[ 2 ];
	pid_t	pid = getpid();
	int     rc = LDAP_SUCCESS;
	int	version = LDAP_VERSION3;
	int	nvalues = 0;
	char	**values = NULL;
	LDAPMessage *res = NULL;

	attrs[ 0 ] = LDAP_NO_ATTRS;
	attrs[ 1 ] = NULL;

	ldap_initialize( &ld, uri );
	if ( ld == NULL ) {
		tester_perror( "ldap_initialize" );
		exit( EXIT_FAILURE );
	}

	(void) ldap_set_option( ld, LDAP_OPT_PROTOCOL_VERSION, &version ); 
	(void) ldap_set_option( ld, LDAP_OPT_REFERRALS,
		chaserefs ? LDAP_OPT_ON : LDAP_OPT_OFF );

	if ( do_retry == maxretries ) {
		fprintf( stderr, "PID=%ld - Read(%d): base=\"%s\", filter=\"%s\".\n",
				(long) pid, innerloop, sbase, filter );
	}

	rc = ldap_sasl_bind_s( ld, manager, LDAP_SASL_SIMPLE, passwd, NULL, NULL, NULL );
	if ( rc != LDAP_SUCCESS ) {
		tester_ldap_error( ld, "ldap_sasl_bind_s" );
		switch ( rc ) {
		case LDAP_BUSY:
		case LDAP_UNAVAILABLE:
		/* fallthru */
		default:
			break;
		}
		exit( EXIT_FAILURE );
	}

	rc = ldap_search_ext_s( ld, sbase, LDAP_SCOPE_SUBTREE,
		filter, attrs, 0, NULL, NULL, NULL, LDAP_NO_LIMIT, &res );
	if ( rc != LDAP_SUCCESS ) {
		tester_ldap_error( ld, "ldap_search_ext_s" );

	} else {
		nvalues = ldap_count_entries( ld, res );
		if ( nvalues > 0 ) {
			LDAPMessage	*e;

			values = malloc( ( nvalues + 1 ) * sizeof( char * ) );
			for ( i = 0, e = ldap_first_entry( ld, res ); e != NULL; i++, e = ldap_next_entry( ld, e ) )
			{
				values[ i ] = ldap_get_dn( ld, e );
			}
			values[ i ] = NULL;
		}

		ldap_msgfree( res );

		if ( do_retry == maxretries ) {
			fprintf( stderr, "PID=%ld - got %d values.\n", (long) pid, nvalues );
		}

		for ( i = 0; i < innerloop; i++ ) {
			do_read( uri, manager, passwd, values[ rand() % nvalues ], &ld,
				noattrs, 1, maxretries, delay, force,
				chaserefs );
		}
	}
		

	fprintf( stderr, " PID=%ld - Search done (%d).\n", (long) pid, rc );

	if ( ld != NULL ) {
		ldap_unbind_ext( ld, NULL, NULL );
	}
}

static void
do_read( char *uri, char *manager, struct berval *passwd, char *entry,
	LDAP **ldp, int noattrs, int maxloop,
	int maxretries, int delay, int force, int chaserefs )
{
	LDAP	*ld = ldp ? *ldp : NULL;
	int  	i = 0, do_retry = maxretries;
	char	*attrs[] = { "1.1", NULL };
	pid_t	pid = getpid();
	int     rc = LDAP_SUCCESS;
	int	version = LDAP_VERSION3;
	int	first = 1;

retry:;
	if ( ld == NULL ) {
		ldap_initialize( &ld, uri );
		if ( ld == NULL ) {
			tester_perror( "ldap_initialize" );
			exit( EXIT_FAILURE );
		}

		(void) ldap_set_option( ld, LDAP_OPT_PROTOCOL_VERSION, &version ); 
		(void) ldap_set_option( ld, LDAP_OPT_REFERRALS,
			chaserefs ? LDAP_OPT_ON : LDAP_OPT_OFF );

		if ( do_retry == maxretries ) {
			fprintf( stderr, "PID=%ld - Read(%d): entry=\"%s\".\n",
				(long) pid, maxloop, entry );
		}

		rc = ldap_sasl_bind_s( ld, manager, LDAP_SASL_SIMPLE, passwd, NULL, NULL, NULL );
		if ( rc != LDAP_SUCCESS ) {
			tester_ldap_error( ld, "ldap_sasl_bind_s" );
			switch ( rc ) {
			case LDAP_BUSY:
			case LDAP_UNAVAILABLE:
				if ( do_retry > 0 ) {
					ldap_unbind_ext( ld, NULL, NULL );
					do_retry--;
					if ( delay != 0 ) {
					    sleep( delay );
					}
					goto retry;
				}
			/* fallthru */
			default:
				break;
			}
			exit( EXIT_FAILURE );
		}
	}

	for ( ; i < maxloop; i++ ) {
		LDAPMessage *res = NULL;

		rc = ldap_search_ext_s( ld, entry, LDAP_SCOPE_BASE,
				NULL, attrs, noattrs, NULL, NULL, NULL,
				LDAP_NO_LIMIT, &res );
		switch ( rc ) {
		case LDAP_REFERRAL:
			/* don't log: it's intended */
			if ( force >= 2 ) {
				if ( !first ) {
					break;
				}
				first = 0;
			}
			tester_ldap_error( ld, "ldap_search_ext_s" );
			/* fallthru */

		case LDAP_SUCCESS:
			break;

		default:
			tester_ldap_error( ld, "ldap_search_ext_s" );
			if ( rc == LDAP_BUSY && do_retry > 0 ) {
				do_retry--;
				goto retry;
			}
			if ( rc != LDAP_NO_SUCH_OBJECT ) {
				goto done;
			}
			break;
		}

		if ( res != NULL ) {
			ldap_msgfree( res );
		}
	}

done:;
	if ( ldp != NULL ) {
		*ldp = ld;

	} else {
		fprintf( stderr, " PID=%ld - Read done (%d).\n", (long) pid, rc );

		if ( ld != NULL ) {
			ldap_unbind_ext( ld, NULL, NULL );
		}
	}
}

