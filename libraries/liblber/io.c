/* io.c - ber general i/o routines */
/*
 * Copyright 1998-1999 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* Portions
 * Copyright (c) 1990 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#include "portable.h"

#include <stdio.h>
#include <stdlib.h>

#include <ac/ctype.h>
#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/unistd.h>

#ifdef HAVE_IO_H
#include <io.h>
#endif

#include "lber-int.h"

static int ber_realloc LDAP_P(( BerElement *ber, unsigned long len ));
static int ber_filbuf LDAP_P(( Sockbuf *sb, long len ));
static long BerRead LDAP_P(( Sockbuf *sb, char *buf, long len ));
#ifdef PCNFS
static int BerWrite LDAP_P(( Sockbuf *sb, char *buf, long len ));
#endif /* PCNFS */

#define bergetc( sb, len )    ( sb->sb_ber.ber_end > sb->sb_ber.ber_ptr ? \
			  (unsigned char)*sb->sb_ber.ber_ptr++ : \
			  ber_filbuf( sb, len ))

#ifdef MACOS
/*
 * MacTCP/OpenTransport
 */
#define read( s, b, l ) tcpread( s, 0, (unsigned char *)b, l, NULL )
#define MAX_WRITE	65535
#define BerWrite( sb, b, l )   tcpwrite( sb->sb_sd, (unsigned char *)(b), (l<MAX_WRITE)? l : MAX_WRITE )
#else /* MACOS */
#ifdef DOS
#ifdef PCNFS
/*
 * PCNFS (under DOS)
 */
#define read( s, b, l ) recv( s, b, l, 0 )
#define BerWrite( s, b, l ) send( s->sb_sd, b, (int) l, 0 )
#endif /* PCNFS */
#ifdef NCSA
/*
 * NCSA Telnet TCP/IP stack (under DOS)
 */
#define read( s, b, l ) nread( s, b, l )
#define BerWrite( s, b, l ) netwrite( s->sb_sd, b, l )
#endif /* NCSA */
#ifdef WINSOCK
/*
 * Windows Socket API (under DOS/Windows 3.x)
 */
#define read( s, b, l ) recv( s, b, l, 0 )
#define BerWrite( s, b, l ) send( s->sb_sd, b, l, 0 )
#endif /* WINSOCK */
#else /* DOS */
#ifdef _WIN32
/*
 * 32-bit Windows Socket API (under Windows NT or Windows 95)
 */
#define read( s, b, l )		recv( s, b, l, 0 )
#define BerWrite( s, b, l )	send( s->sb_sd, b, l, 0 )
#else /* _WIN32 */
#ifdef VMS
/*
 * VMS -- each write must be 64K or smaller
 */
#define MAX_WRITE 65535
#define BerWrite( sb, b, l ) write( sb->sb_sd, b, (l<MAX_WRITE)? l : MAX_WRITE)
#else /* VMS */
/*
 * everything else (Unix/BSD 4.3 socket API)
 */
#define BerWrite( sb, b, l )	write( sb->sb_sd, b, l )
#endif /* VMS */
#define udp_read( sb, b, l, al ) recvfrom(sb->sb_sd, (char *)b, l, 0, \
		(struct sockaddr *)sb->sb_fromaddr, \
		(al = sizeof(struct sockaddr), &al))
#define udp_write( sb, b, l ) sendto(sb->sb_sd, (char *)(b), l, 0, \
		(struct sockaddr *)sb->sb_useaddr, sizeof(struct sockaddr))
#endif /* _WIN32 */
#endif /* DOS */
#endif /* MACOS */

#ifndef udp_read
#define udp_read( sb, b, l, al )	CLDAP NOT SUPPORTED
#define udp_write( sb, b, l )		CLDAP NOT SUPPORTED
#endif /* udp_read */

#define EXBUFSIZ	1024

static int
ber_filbuf( Sockbuf *sb, long len )
{
	short	rc;
#ifdef LDAP_CONNECTIONLESS
	int	addrlen;
#endif /* LDAP_CONNECTIONLESS */

	if ( sb->sb_ber.ber_buf == NULL ) {
		if ( (sb->sb_ber.ber_buf = (char *) malloc( READBUFSIZ )) ==
		    NULL )
			return( -1 );
		sb->sb_ber.ber_ptr = sb->sb_ber.ber_buf;
		sb->sb_ber.ber_end = sb->sb_ber.ber_buf;
	}

	if ( sb->sb_naddr > 0 ) {
#ifdef LDAP_CONNECTIONLESS
		rc = udp_read(sb, sb->sb_ber.ber_buf, READBUFSIZ, addrlen );

		if ( sb->sb_debug ) {
			lber_log_printf( LDAP_DEBUG_ANY, sb->sb_debug,
				"ber_filbuf udp_read %d bytes\n",
					rc );
			if ( rc > 0 )
				lber_log_bprint( LDAP_DEBUG_PACKETS, sb->sb_debug,
					sb->sb_ber.ber_buf, rc );
		}
#else /* LDAP_CONNECTIONLESS */
		rc = -1;
#endif /* LDAP_CONNECTIONLESS */
	} else {
		rc = read( sb->sb_sd, sb->sb_ber.ber_buf,
			((sb->sb_options & LBER_NO_READ_AHEAD) && (len < READBUFSIZ)) ?
				len : READBUFSIZ );
	}

	if ( rc > 0 ) {
		sb->sb_ber.ber_ptr = sb->sb_ber.ber_buf + 1;
		sb->sb_ber.ber_end = sb->sb_ber.ber_buf + rc;
		return( (unsigned char)*sb->sb_ber.ber_buf );
	}

	return( -1 );
}


static long
BerRead( Sockbuf *sb, char *buf, long len )
{
	int	c;
	long	nread = 0;

	while ( len > 0 ) {
		if ( (c = bergetc( sb, len )) < 0 ) {
			if ( nread > 0 )
				break;
			return( c );
		}
		*buf++ = (char) c;
		nread++;
		len--;
	}

	return( nread );
}


long
ber_read( BerElement *ber, char *buf, unsigned long len )
{
	unsigned long	actuallen, nleft;

	nleft = ber->ber_end - ber->ber_ptr;
	actuallen = nleft < len ? nleft : len;

	SAFEMEMCPY( buf, ber->ber_ptr, (size_t)actuallen );

	ber->ber_ptr += actuallen;

	return( (long)actuallen );
}

long
ber_write( BerElement *ber, char *buf, unsigned long len, int nosos )
{
	if ( nosos || ber->ber_sos == NULL ) {
		if ( ber->ber_ptr + len > ber->ber_end ) {
			if ( ber_realloc( ber, len ) != 0 )
				return( -1 );
		}
		SAFEMEMCPY( ber->ber_ptr, buf, (size_t)len );
		ber->ber_ptr += len;
		return( len );
	} else {
		if ( ber->ber_sos->sos_ptr + len > ber->ber_end ) {
			if ( ber_realloc( ber, len ) != 0 )
				return( -1 );
		}
		SAFEMEMCPY( ber->ber_sos->sos_ptr, buf, (size_t)len );
		ber->ber_sos->sos_ptr += len;
		ber->ber_sos->sos_clen += len;
		return( len );
	}
}

static int
ber_realloc( BerElement *ber, unsigned long len )
{
	unsigned long	need, have, total;
	Seqorset	*s;
	long		off;
	char		*oldbuf;

	have = (ber->ber_end - ber->ber_buf) / EXBUFSIZ;
	need = (len < EXBUFSIZ ? 1 : (len + (EXBUFSIZ - 1)) / EXBUFSIZ);
	total = have * EXBUFSIZ + need * EXBUFSIZ;

	oldbuf = ber->ber_buf;

	if ( ber->ber_buf == NULL ) {
		if ( (ber->ber_buf = (char *) malloc( (size_t)total )) == NULL )
			return( -1 );
	} else if ( (ber->ber_buf = (char *) realloc( ber->ber_buf,
	    (size_t)total )) == NULL )
		return( -1 );

	ber->ber_end = ber->ber_buf + total;

	/*
	 * If the stinking thing was moved, we need to go through and
	 * reset all the sos and ber pointers.  Offsets would've been
	 * a better idea... oh well.
	 */

	if ( ber->ber_buf != oldbuf ) {
		ber->ber_ptr = ber->ber_buf + (ber->ber_ptr - oldbuf);

		for ( s = ber->ber_sos; s != NULLSEQORSET; s = s->sos_next ) {
			off = s->sos_first - oldbuf;
			s->sos_first = ber->ber_buf + off;

			off = s->sos_ptr - oldbuf;
			s->sos_ptr = ber->ber_buf + off;
		}
	}

	return( 0 );
}

void
ber_free( BerElement *ber, int freebuf )
{
	if ( freebuf && ber->ber_buf != NULL )
		free( ber->ber_buf );
	ber->ber_buf = NULL;
	free( (char *) ber );
}

int
ber_flush( Sockbuf *sb, BerElement *ber, int freeit )
{
	long	nwritten, towrite, rc;

	if ( ber->ber_rwptr == NULL ) {
		ber->ber_rwptr = ber->ber_buf;
	}
	towrite = ber->ber_ptr - ber->ber_rwptr;

	if ( sb->sb_debug ) {
		lber_log_printf( LDAP_DEBUG_ANY, sb->sb_debug,
			"ber_flush: %ld bytes to sd %ld%s\n", towrite,
		    (long) sb->sb_sd, ber->ber_rwptr != ber->ber_buf ? " (re-flush)"
		    : "" );
		lber_log_bprint( LDAP_DEBUG_PACKETS, sb->sb_debug,
			ber->ber_rwptr, towrite );
	}

#if !defined(MACOS) && !defined(DOS)
	if ( sb->sb_options & (LBER_TO_FILE | LBER_TO_FILE_ONLY) ) {
		rc = write( sb->sb_fd, ber->ber_buf, towrite );
		if ( sb->sb_options & LBER_TO_FILE_ONLY ) {
			return( (int)rc );
		}
	}
#endif

	nwritten = 0;
	do {
		if (sb->sb_naddr > 0) {
#ifdef LDAP_CONNECTIONLESS
			rc = udp_write( sb, ber->ber_buf + nwritten,
			    (size_t)towrite );
#else /* LDAP_CONNECTIONLESS */
			rc = -1;
#endif /* LDAP_CONNECTIONLESS */
			if ( rc <= 0 )
				return( -1 );

			/* fake error if write was not atomic */
			if (rc < towrite) {
#ifdef EMSGSIZE
			    errno = EMSGSIZE;
#endif
			    return( -1 );
			}
		} else {
			if ( (rc = BerWrite( sb, ber->ber_rwptr,
			    (size_t) towrite )) <= 0 ) {
				return( -1 );
			}
		}
		towrite -= rc;
		nwritten += rc;
		ber->ber_rwptr += rc;
	} while ( towrite > 0 );

	if ( freeit )
		ber_free( ber, 1 );

	return( 0 );
}

BerElement *
ber_alloc_t( int options )
{
	BerElement	*ber;

	if ( (ber = (BerElement *) calloc( 1, sizeof(BerElement) )) == NULLBER )
		return( NULLBER );
	ber->ber_tag = LBER_DEFAULT;
	ber->ber_options = options;
	ber->ber_debug = lber_int_debug;

	return( ber );
}

BerElement *
ber_alloc( void )
{
	return( ber_alloc_t( 0 ) );
}

BerElement *
der_alloc( void )
{
	return( ber_alloc_t( LBER_USE_DER ) );
}

BerElement *
ber_dup( BerElement *ber )
{
	BerElement	*new;

	if ( (new = ber_alloc()) == NULLBER )
		return( NULLBER );

	*new = *ber;

	return( new );
}


/* OLD U-Mich ber_init() */
void
ber_init_w_nullc( BerElement *ber, int options )
{
	(void) memset( (char *)ber, '\0', sizeof( BerElement ));
	ber->ber_tag = LBER_DEFAULT;
	ber->ber_options = (char) options;
}

/* New C-API ber_init() */
/* This function constructs a BerElement containing a copy
** of the data in the bv argument.
*/
BerElement *
ber_init( struct berval *bv )
{
	BerElement *ber;

	if ( bv == NULL ) {
		return NULL;
	}

	ber = ber_alloc_t( 0 );

	if( ber == NULLBER ) {
		/* allocation failed */
		return ( NULL );
	}

	/* copy the data */
	if ( ( (unsigned int) ber_write ( ber, bv->bv_val, bv->bv_len, 0 )) != bv->bv_len ) {
		/* write failed, so free and return NULL */
		ber_free( ber, 1 );
		return( NULL );
	}

	ber_reset( ber, 1 );	/* reset the pointer to the start of the buffer */

	return ( ber );
}

/* New C-API ber_flatten routine */
/* This routine allocates a struct berval whose contents are a BER
** encoding taken from the ber argument.  The bvPtr pointer pointers to
** the returned berval.
*/
int ber_flatten(
	BerElement *ber,
	struct berval **bvPtr)
{
	struct berval *bv;
 
	if(bvPtr == NULL) {
		return( -1 );
	}

	if ( (bv = malloc( sizeof(struct berval))) == NULL ) {
		return( -1 );
	}

	if ( ber == NULL ) {
		/* ber is null, create an empty berval */
		bv->bv_val = NULL;
		bv->bv_len = 0;

	} else {
		/* copy the berval */
		ptrdiff_t len = ber->ber_ptr - ber->ber_buf;

		if ( (bv->bv_val = (char *) malloc( len + 1 )) == NULL ) {
			ber_bvfree( bv );
			return( -1 );
		}

		SAFEMEMCPY( bv->bv_val, ber->ber_buf, (size_t)len );
		bv->bv_val[len] = '\0';
		bv->bv_len = len;
	}
    
	*bvPtr = bv;
	return( 0 );
}

void
ber_reset( BerElement *ber, int was_writing )
{
	if ( was_writing ) {
		ber->ber_end = ber->ber_ptr;
		ber->ber_ptr = ber->ber_buf;
	} else {
		ber->ber_ptr = ber->ber_end;
	}

	ber->ber_rwptr = NULL;
}

/* return the tag - LBER_DEFAULT returned means trouble */
static unsigned long
get_tag( Sockbuf *sb )
{
	unsigned char	xbyte;
	unsigned long	tag;
	char		*tagp;
	unsigned int	i;

	if ( BerRead( sb, (char *) &xbyte, 1 ) != 1 )
		return( LBER_DEFAULT );

	if ( (xbyte & LBER_BIG_TAG_MASK) != LBER_BIG_TAG_MASK )
		return( (unsigned long) xbyte );

	tagp = (char *) &tag;
	tagp[0] = xbyte;
	for ( i = 1; i < sizeof(long); i++ ) {
		if ( BerRead( sb, (char *) &xbyte, 1 ) != 1 )
			return( LBER_DEFAULT );

		tagp[i] = xbyte;

		if ( ! (xbyte & LBER_MORE_TAG_MASK) )
			break;
	}

	/* tag too big! */
	if ( i == sizeof(long) )
		return( LBER_DEFAULT );

	/* want leading, not trailing 0's */
	return( tag >> (sizeof(long) - i - 1) );
}

unsigned long
ber_get_next( Sockbuf *sb, unsigned long *len, BerElement *ber )
{
	unsigned long	tag = 0, netlen, toread;
	unsigned char	lc;
	long		rc;
	long		noctets;
	unsigned int	diff;

	if ( ber->ber_debug ) {
		lber_log_printf( LDAP_DEBUG_TRACE, ber->ber_debug,
			"ber_get_next\n" );
	}

	/*
	 * Any ber element looks like this: tag length contents.
	 * Assuming everything's ok, we return the tag byte (we
	 * can assume a single byte), return the length in len,
	 * and the rest of the undecoded element in buf.
	 *
	 * Assumptions:
	 *	1) small tags (less than 128)
	 *	2) definite lengths
	 *	3) primitive encodings used whenever possible
	 */

	/*
	 * first time through - malloc the buffer, set up ptrs, and
	 * read the tag and the length and as much of the rest as we can
	 */

	if ( ber->ber_rwptr == NULL ) {
		/*
		 * First, we read the tag.
		 */

		if ( (tag = get_tag( sb )) == LBER_DEFAULT ) {
			return( LBER_DEFAULT );
		}
		ber->ber_tag = tag;

		/*
		 * Next, read the length.  The first byte contains the length
		 * of the length.  If bit 8 is set, the length is the long
		 * form, otherwise it's the short form.  We don't allow a
		 * length that's greater than what we can hold in an unsigned
		 * long.
		 */

		*len = netlen = 0;
		if ( BerRead( sb, (char *) &lc, 1 ) != 1 ) {
			return( LBER_DEFAULT );
		}
		if ( lc & 0x80 ) {
			noctets = (lc & 0x7f);
			if ( noctets > sizeof(unsigned long) )
				return( LBER_DEFAULT );
			diff = sizeof(unsigned long) - noctets;
			if ( BerRead( sb, (char *) &netlen + diff, noctets ) !=
			    noctets ) {
				return( LBER_DEFAULT );
			}
			*len = AC_NTOHL( netlen );
		} else {
			*len = lc;
		}
		ber->ber_len = *len;

		/*
		 * Finally, malloc a buffer for the contents and read it in.
		 * It's this buffer that's passed to all the other ber decoding
		 * routines.
		 */

#if defined( DOS ) && !defined( _WIN32 )
		if ( *len > 65535 ) {	/* DOS can't allocate > 64K */
		    return( LBER_DEFAULT );
		}
#endif /* DOS && !_WIN32 */

		if ( ( sb->sb_options & LBER_MAX_INCOMING_SIZE ) &&
		    *len > (unsigned long) sb->sb_max_incoming ) {
			return( LBER_DEFAULT );
		}

		if ( (ber->ber_buf = (char *) malloc( (size_t)*len )) == NULL ) {
			return( LBER_DEFAULT );
		}
		ber->ber_ptr = ber->ber_buf;
		ber->ber_end = ber->ber_buf + *len;
		ber->ber_rwptr = ber->ber_buf;
	}

	toread = (unsigned long)ber->ber_end - (unsigned long)ber->ber_rwptr;
	do {
		if ( (rc = BerRead( sb, ber->ber_rwptr, (long)toread )) <= 0 ) {
			return( LBER_DEFAULT );
		}

		toread -= rc;
		ber->ber_rwptr += rc;
	} while ( toread > 0 );

	if ( ber->ber_debug ) {
		lber_log_printf( LDAP_DEBUG_TRACE, ber->ber_debug,
			"ber_get_next: tag 0x%lx len %ld contents:\n",
		    tag, ber->ber_len );

		lber_log_dump( LDAP_DEBUG_BER, ber->ber_debug, ber, 1 );
	}

	*len = ber->ber_len;
	ber->ber_rwptr = NULL;
	return( ber->ber_tag );
}

Sockbuf *lber_sockbuf_alloc( void )
{
	Sockbuf *sb = calloc(1, sizeof(Sockbuf));
	sb->sb_debug = lber_int_debug;
	return sb;
}

Sockbuf *lber_sockbuf_alloc_fd( int fd )
{
	Sockbuf *sb = lber_sockbuf_alloc();
	sb->sb_sd = fd;

	return sb;
}

void lber_sockbuf_free( Sockbuf *sb )
{
	if(sb == NULL) return;

	free(sb);
}

int lber_sockbuf_get_option( Sockbuf *sb, int opt, void *outvalue )
{
	return LBER_OPT_ERROR;
}

int lber_sockbuf_set_option( Sockbuf *sb, int opt, void *invalue )
{
	return LBER_OPT_ERROR;
}
