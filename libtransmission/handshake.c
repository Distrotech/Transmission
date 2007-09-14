/*
 * This file Copyright (C) 2007 Charles Kerr <charles@rebelbase.com>
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license. 
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h> /* UCHAR_MAX */
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

#include <sys/types.h> /* event.h needs this */
#include <event.h>

#include "transmission.h"
#include "bencode.h"
#include "crypto.h"
#include "handshake.h"
#include "peer-io.h"
#include "utils.h"

/* enable LibTransmission extension protocol */
#define ENABLE_LTEP

/* enable Azureus messaging protocol */
//#define ENABLE_AZMP

/***
****
***/

#define HANDSHAKE_NAME          "\023BitTorrent protocol"
#define HANDSHAKE_NAME_LEN      20
#define HANDSHAKE_FLAGS_LEN     8
#define HANDSHAKE_SIZE          68

#ifdef ENABLE_LTEP
#define HANDSHAKE_HAS_EXTMSGS( bits ) ( (bits)[5] & 0x10 )
#define HANDSHAKE_SET_EXTMSGS( bits ) ( (bits)[5] |= 0x10 )
#else
#define HANDSHAKE_HAS_EXTMSGS( bits ) ( 0 )
#define HANDSHAKE_SET_EXTMSGS( bits ) ( (void)0 )
#endif

#ifdef ENABLE_AZMP
#define HANDSHAKE_HAS_AZPROTO( bits ) ( (bits)[0] & 0x80 )
#define HANDSHAKE_SET_AZPROTO( bits ) ( (bits)[0] |= 0x80 )
#else
#define HANDSHAKE_HAS_AZPROTO( bits ) ( 0 )
#define HANDSHAKE_SET_AZPROTO( bits ) ( (void)0 )
#endif

/* http://www.azureuswiki.com/index.php/Extension_negotiation_protocol
   these macros are to be used if both extended messaging and the
   azureus protocol is supported, they indicate which protocol is preferred */
#define HANDSHAKE_GET_EXTPREF( reserved )      ( (reserved)[5] & 0x03 )
#define HANDSHAKE_SET_EXTPREF( reserved, val ) ( (reserved)[5] |= 0x03 & (val) )
#define HANDSHAKE_EXTPREF_LTEP_FORCE   ( 0x0 )
#define HANDSHAKE_EXTPREF_LTEP_PREFER  ( 0x1 )
#define HANDSHAKE_EXTPREF_AZMP_PREFER  ( 0x2 )
#define HANDSHAKE_EXTPREF_AZMP_FORCE   ( 0x3 )

extern const char* getPeerId( void ) ;

#define KEY_LEN 96
#define PRIME_LEN 96
#define VC_LENGTH 8


struct tr_handshake
{
    tr_peerIo * io;
    tr_crypto * crypto;
    struct tr_handle * handle;
    uint8_t myPublicKey[96];
    uint8_t mySecret[96];
    uint8_t state;
    uint8_t encryptionPreference;
    uint16_t pad_c_len;
    uint16_t pad_d_len;
    int ia_len;
    int crypto_select;
    uint8_t myReq1[SHA_DIGEST_LENGTH];
    uint8_t peer_id[20];
    int have_peer_id;
    handshakeDoneCB doneCB;
    void * doneUserData;
};

static void
fireDoneCB( tr_handshake * handshake, int isConnected );

/**
***
**/

enum /*ccc*/
{
    /* incoming */
    AWAITING_HANDSHAKE,
    AWAITING_YA,
    AWAITING_PAD_A,
    AWAITING_CRYPTO_PROVIDE,
    AWAITING_PAD_C,
    AWAITING_IA,

    /* outgoing */
    AWAITING_YB,
    AWAITING_VC,
    AWAITING_CRYPTO_SELECT,
    AWAITING_PAD_D,
};

/**
***
**/

static const char* getStateName( short state )
{
    const char * str = "f00!";
    switch( state ) {
        case AWAITING_HANDSHAKE:      str = "awaiting handshake"; break;
        case AWAITING_YA:             str = "awaiting ya"; break;
        //case SENDING_YB:              str = "sending yb"; break;
        case AWAITING_PAD_A:          str = "awaiting pad a"; break;
        case AWAITING_CRYPTO_PROVIDE: str = "awaiting crypto_provide"; break;
        case AWAITING_PAD_C:          str = "awaiting pad c"; break;
        case AWAITING_IA:             str = "awaiting ia"; break;
        //case SENDING_YA:              str = "sending ya"; break;
        case AWAITING_YB:             str = "awaiting yb"; break;
        //case SENDING_CRYPTO_PROVIDE:  str = "sending crypto provide"; break;
        case AWAITING_VC:             str = "awaiting vc"; break;
        case AWAITING_CRYPTO_SELECT:  str = "awaiting crypto select"; break;
        case AWAITING_PAD_D:          str = "awaiting pad d"; break;
        //case SENDING_PLAINTEXT_HANDSHAKE: str = "sending plaintext handshake"; break;
    }
    return str;
}

static void
setState( tr_handshake * handshake, short state )
{
    fprintf( stderr, "handshake %p: setting to state [%s]\n", handshake, getStateName(state) );
    handshake->state = state;
}

static void
setReadState( tr_handshake * handshake, int state )
{
    setState( handshake, state );
    //tr_peerIoSetIOMode( handshake->io, EV_READ, EV_WRITE );
}

static void
sendPublicKey( tr_handshake * handshake )
{
    int i;
    int len;
    const uint8_t * public_key;
    struct evbuffer * outbuf = evbuffer_new( );
    uint8_t pad[512];

    /* add our public key (Ya) */
    public_key = tr_cryptoGetMyPublicKey( handshake->crypto, &len );
    assert( len == KEY_LEN );
    assert( public_key != NULL );
    evbuffer_add( outbuf, public_key, len );

    /* add some bullshit padding */
    len = tr_rand( 512 );
    for( i=0; i<len; ++i )
        pad[i] = tr_rand( UCHAR_MAX );
    evbuffer_add( outbuf, pad, len );

    /* send it */
    setReadState( handshake, AWAITING_YB );
    //setState( handshake, SENDING_YA );
    tr_peerIoWriteBuf( handshake->io, outbuf );

    /* cleanup */
    evbuffer_free( outbuf );
}

static void
buildHandshakeMessage( tr_handshake * handshake, uint8_t * buf )
{
    uint8_t *walk = buf;
    const uint8_t * torrentHash = tr_cryptoGetTorrentHash( handshake->crypto );

    memcpy( walk, HANDSHAKE_NAME, HANDSHAKE_NAME_LEN );
    walk += HANDSHAKE_NAME_LEN;
    memset( walk, 0, HANDSHAKE_FLAGS_LEN );
    HANDSHAKE_SET_EXTMSGS( walk );
    HANDSHAKE_SET_AZPROTO( walk );
    HANDSHAKE_SET_EXTPREF( walk, HANDSHAKE_EXTPREF_LTEP_PREFER );
    walk += HANDSHAKE_FLAGS_LEN;
    memcpy( walk, torrentHash, SHA_DIGEST_LENGTH );
    walk += SHA_DIGEST_LENGTH;
    memcpy( walk, getPeerId(), TR_ID_LEN );
    walk += TR_ID_LEN;
    assert( walk-buf == HANDSHAKE_SIZE );
}

static void
sendPlaintextHandshake( tr_handshake * handshake )
{
    uint8_t buf[HANDSHAKE_SIZE];
    buildHandshakeMessage( handshake, buf );

    setReadState( handshake, AWAITING_HANDSHAKE );
    tr_peerIoWrite( handshake->io, buf, HANDSHAKE_SIZE );
}

static void
sendHandshake( tr_handshake * handshake )
{
    if( ( handshake->encryptionPreference == HANDSHAKE_ENCRYPTION_PREFERRED ) ||
        ( handshake->encryptionPreference == HANDSHAKE_ENCRYPTION_REQUIRED ) )
    {
        sendPublicKey( handshake );
    }
    else
    {
        sendPlaintextHandshake( handshake );
    }
}

static void
sendLtepHandshake( tr_handshake * handshake )
{
    benc_val_t val, *m;
    char * buf;
    int len;
    const char * v = TR_NAME " " USERAGENT_PREFIX;
    const int port = tr_getPublicPort( handshake->handle );
    struct evbuffer * outbuf = evbuffer_new( );
    uint32_t msglen;
    const uint8_t tr_msgid = 20; /* ltep extension id */
    const uint8_t ltep_msgid = 0; /* handshake id */

    tr_bencInit( &val, TYPE_DICT );
    tr_bencDictReserve( &val, 3 );
    m  = tr_bencDictAdd( &val, "m" );
    tr_bencInit( m, TYPE_DICT );
    tr_bencDictReserve( m, 1 );
    tr_bencInitInt( tr_bencDictAdd( m, "ut_pex" ), 1 );
    if( port > 0 )
        tr_bencInitInt( tr_bencDictAdd( &val, "p" ), port );
    tr_bencInitStr( tr_bencDictAdd( &val, "v" ), v, 0, 1 );

    fprintf( stderr, "handshake %p: sending ltep handshake...\n", handshake );
    buf = tr_bencSaveMalloc( &val,  &len );
    tr_bencPrint( &val );

    msglen = sizeof(tr_msgid) + sizeof(ltep_msgid) + len;
    tr_peerIoWriteUint32( handshake->io, outbuf, msglen );
    tr_peerIoWriteBytes ( handshake->io, outbuf, &tr_msgid, 1 );
    tr_peerIoWriteBytes ( handshake->io, outbuf, &ltep_msgid, 1 );
    tr_peerIoWriteBytes ( handshake->io, outbuf, buf, len );
    
    tr_peerIoWriteBuf( handshake->io, outbuf );
    fireDoneCB( handshake, TRUE );
  
    /* cleanup */ 
    tr_bencFree( &val );
    tr_free( buf ); 
    evbuffer_free( outbuf );
}


/**
***
**/

static int
readYa( tr_handshake * handshake, struct evbuffer  * inbuf )
{
    uint8_t ya[KEY_LEN];
    uint8_t *walk, outbuf[KEY_LEN + 512];
    const uint8_t *myKey, *secret;
    int len;

    if( EVBUFFER_LENGTH( inbuf ) < KEY_LEN )
        return READ_MORE;

    /* read the incoming peer's public key */
    evbuffer_remove( inbuf, ya, KEY_LEN );
    secret = tr_cryptoComputeSecret( handshake->crypto, ya );
    memcpy( handshake->mySecret, secret, KEY_LEN );
    tr_sha1( handshake->myReq1, "req1", 4, secret, KEY_LEN, NULL );

    /* send our public key to the peer */
    walk = outbuf;
    myKey = tr_cryptoGetMyPublicKey( handshake->crypto, &len );
    memcpy( walk, myKey, len );
    len = tr_rand( 512 );
    while( len-- )
        *walk++ = tr_rand( UCHAR_MAX );

    setReadState( handshake, AWAITING_PAD_A );
    tr_peerIoWrite( handshake->io, outbuf, walk-outbuf );

    return READ_DONE;
}

static int
readPadA( tr_handshake * handshake, struct evbuffer * inbuf )
{
    uint8_t * pch;

    /**
    *** Resynchronizing on HASH('req1',S)
    **/

    pch = memchr( EVBUFFER_DATA(inbuf),
                  handshake->myReq1[0],
                  EVBUFFER_LENGTH(inbuf) );
    if( pch == NULL ) {
        evbuffer_drain( inbuf, EVBUFFER_LENGTH(inbuf) );
        return READ_MORE;
    }
    evbuffer_drain( inbuf, pch-EVBUFFER_DATA(inbuf) );
    if( EVBUFFER_LENGTH(inbuf) < SHA_DIGEST_LENGTH )
        return READ_MORE;
    if( memcmp( EVBUFFER_DATA(inbuf), handshake->myReq1, SHA_DIGEST_LENGTH ) ) {
        evbuffer_drain( inbuf, 1 );
        return READ_AGAIN;
    }

    setState( handshake, AWAITING_CRYPTO_PROVIDE );
    return READ_AGAIN;
}

static int
readCryptoProvide( tr_handshake * handshake, struct evbuffer * inbuf )
{
    /* HASH('req2', SKEY) xor HASH('req3', S), ENCRYPT(VC, crypto_provide, len(PadC)) */

    int i;
    uint8_t vc_in[VC_LENGTH];
    uint8_t req2[SHA_DIGEST_LENGTH];
    uint8_t req3[SHA_DIGEST_LENGTH];
    uint8_t obfuscatedTorrentHash[SHA_DIGEST_LENGTH];
    uint16_t padc_len = 0;
    uint32_t crypto_provide = 0;
    const size_t needlen = SHA_DIGEST_LENGTH + VC_LENGTH + sizeof(crypto_provide) + sizeof(padc_len);
    tr_torrent * tor = NULL;

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    /* TODO: confirm they sent HASH('req1',S) here? */
    evbuffer_drain( inbuf, SHA_DIGEST_LENGTH );

    /* This next piece is HASH('req2', SKEY) xor HASH('req3', S) ...
     * we can get the first half of that (the obufscatedTorrentHash)
     * by building the latter and xor'ing it with what the peer sent us */
    fprintf( stderr, "reading obfuscated torrent hash...\n" );
    evbuffer_remove( inbuf, req2, SHA_DIGEST_LENGTH );
    tr_sha1( req3, "req3", 4, handshake->mySecret, KEY_LEN, NULL );
    for( i=0; i<SHA_DIGEST_LENGTH; ++i )
        obfuscatedTorrentHash[i] = req2[i] ^ req3[i];
    tor = tr_torrentFindFromObfuscatedHash( handshake->handle, obfuscatedTorrentHash );
    assert( tor != NULL );
    fprintf( stderr, "found the torrent; it's [%s]\n", tor->info.name );
    tr_peerIoSetTorrentHash( handshake->io, tor->info.hash );

    /* next part: ENCRYPT(VC, crypto_provide, len(PadC), */

    tr_cryptoDecryptInit( handshake->crypto );

    tr_peerIoReadBytes( handshake->io, inbuf, vc_in, VC_LENGTH );

    tr_peerIoReadUint32( handshake->io, inbuf, &crypto_provide );
    fprintf( stderr, "crypto_provide is %d\n", (int)crypto_provide );

    tr_peerIoReadUint16( handshake->io, inbuf, &padc_len );
    fprintf( stderr, "padc is %d\n", (int)padc_len );
    handshake->pad_c_len = padc_len;
    setState( handshake, AWAITING_PAD_C );
    return READ_AGAIN;
}

static int
readPadC( tr_handshake * handshake, struct evbuffer * inbuf )
{
    uint16_t ia_len;
    const size_t needlen = handshake->pad_c_len + sizeof(uint16_t);

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    evbuffer_drain( inbuf, needlen );

    tr_peerIoReadUint16( handshake->io, inbuf, &ia_len );
    fprintf( stderr, "ia_len is %d\n", (int)ia_len );
    handshake->ia_len = ia_len;
    setState( handshake, AWAITING_IA );
    return READ_AGAIN;
}

static int
readIA( tr_handshake * handshake, struct evbuffer * inbuf )
{
    const size_t needlen = handshake->ia_len;
    uint8_t * ia;

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    ia = tr_new( uint8_t, handshake->ia_len );
    tr_peerIoReadBytes( handshake->io, inbuf, ia, handshake->ia_len );
    fprintf( stderr, "got their payload ia: [%*.*s]\n", (int)needlen, (int)needlen, ia );

    handshake->state = -1;
    assert( 0 && "asdf" );
}

/**
***
**/

static int
readYb( tr_handshake * handshake, struct evbuffer * inbuf )
{
    int isEncrypted;
    const uint8_t * secret;
    uint8_t yb[KEY_LEN];
    struct evbuffer * outbuf;
    size_t needlen = HANDSHAKE_NAME_LEN;

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;
fprintf( stderr, "%*.*s\n", HANDSHAKE_NAME_LEN, HANDSHAKE_NAME_LEN, EVBUFFER_DATA(inbuf) );

    isEncrypted = memcmp( EVBUFFER_DATA(inbuf), HANDSHAKE_NAME, HANDSHAKE_NAME_LEN );
    if( isEncrypted ) {
        needlen = KEY_LEN;
        if( EVBUFFER_LENGTH(inbuf) < needlen )
            return READ_MORE;
    }

    fprintf( stderr, "got a %s handshake\n", (isEncrypted ? "encrypted" : "plaintext") );
    tr_peerIoSetEncryption( handshake->io, isEncrypted
        ? PEER_ENCRYPTION_RC4
        : PEER_ENCRYPTION_PLAINTEXT );
    if( !isEncrypted ) {
        setState( handshake, AWAITING_HANDSHAKE );
        return READ_AGAIN;
    }

    /* compute the secret */
    evbuffer_remove( inbuf, yb, KEY_LEN );
    secret = tr_cryptoComputeSecret( handshake->crypto, yb );
    memcpy( handshake->mySecret, secret, KEY_LEN );

    /* now send these: HASH('req1', S), HASH('req2', SKEY) xor HASH('req3', S),
     * ENCRYPT(VC, crypto_provide, len(PadC), PadC, len(IA)), ENCRYPT(IA) */
    outbuf = evbuffer_new( );

    /* HASH('req1', S) */
    {
        uint8_t req1[SHA_DIGEST_LENGTH];
        tr_sha1( req1, "req1", 4, secret, KEY_LEN, NULL );
        evbuffer_add( outbuf, req1, SHA_DIGEST_LENGTH );
    }

    /* HASH('req2', SKEY) xor HASH('req3', S) */
    {
        int i;
        uint8_t req2[SHA_DIGEST_LENGTH];
        uint8_t req3[SHA_DIGEST_LENGTH];
        uint8_t buf[SHA_DIGEST_LENGTH];
        tr_sha1( req2, "req2", 4, tr_cryptoGetTorrentHash(handshake->crypto), SHA_DIGEST_LENGTH, NULL );
        tr_sha1( req3, "req3", 4, secret, KEY_LEN, NULL );
        for( i=0; i<SHA_DIGEST_LENGTH; ++i )
            buf[i] = req2[i] ^ req3[i];
        evbuffer_add( outbuf, buf, SHA_DIGEST_LENGTH );
    }
      
    /* ENCRYPT(VC, crypto_provide, len(PadC), PadC */
    {
        uint8_t vc[VC_LENGTH] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t pad[512];
        uint16_t i, len;
        uint32_t crypto_provide;

        tr_cryptoEncryptInit( handshake->crypto );
       
        /* vc */ 
        tr_cryptoEncrypt( handshake->crypto, VC_LENGTH, vc, vc );
        evbuffer_add( outbuf, vc, VC_LENGTH );

        /* crypto_provide */
        crypto_provide = 0;
        if( handshake->encryptionPreference != HANDSHAKE_PLAINTEXT_REQUIRED )
            crypto_provide |= (1<<0);
        if( handshake->encryptionPreference != HANDSHAKE_ENCRYPTION_REQUIRED )
            crypto_provide |= (1<<1);
        assert( 1<=crypto_provide && crypto_provide<=3 );

        crypto_provide = htonl( crypto_provide );
        tr_cryptoEncrypt( handshake->crypto, sizeof(crypto_provide), &crypto_provide, &crypto_provide );
        evbuffer_add( outbuf, &crypto_provide, sizeof(crypto_provide) );

        /* len(padc) */
        i = len = tr_rand( 512 );
        i = htons( i );
        tr_cryptoEncrypt( handshake->crypto, sizeof(i), &i, &i );
        evbuffer_add( outbuf, &i, sizeof(i) );

        /* padc */
        for( i=0; i<len; ++i ) pad[i] = tr_rand( UCHAR_MAX );
        tr_cryptoEncrypt( handshake->crypto, len, pad, pad );
        evbuffer_add( outbuf, pad, len );
    }

    /* ENCRYPT len(IA)), ENCRYPT(IA) */
    {
        uint16_t i;
        uint8_t msg[HANDSHAKE_SIZE];
        buildHandshakeMessage( handshake, msg );

        i = htons( HANDSHAKE_SIZE );
        tr_cryptoEncrypt( handshake->crypto, sizeof(uint16_t), &i, &i );
        evbuffer_add( outbuf, &i, sizeof(uint16_t) );

        tr_cryptoEncrypt( handshake->crypto, HANDSHAKE_SIZE, msg, msg );
        evbuffer_add( outbuf, msg, HANDSHAKE_SIZE );
    }

    /* send it */
    tr_cryptoDecryptInit( handshake->crypto );
    setReadState( handshake, AWAITING_VC );
    tr_peerIoWriteBuf( handshake->io, outbuf );

    /* cleanup */
    evbuffer_free( outbuf );
    return READ_DONE;
}

static int
readVC( tr_handshake * handshake, struct evbuffer * inbuf )
{
    const uint8_t key[VC_LENGTH] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const int key_len = VC_LENGTH;
    uint8_t tmp[VC_LENGTH];

    /* note: this works w/o having to `unwind' the buffer if
     * we read too much, but it is pretty brute-force.
     * it would be nice to make this cleaner. */
    for( ;; )
    {
        if( EVBUFFER_LENGTH(inbuf) < VC_LENGTH ) {
            fprintf( stderr, "not enough bytes... returning read_more\n" );
            return READ_MORE;
        }

        memcpy( tmp, EVBUFFER_DATA(inbuf), key_len );
        tr_cryptoDecryptInit( handshake->crypto );
        tr_cryptoDecrypt( handshake->crypto, key_len, tmp, tmp );
        if( !memcmp( tmp, key, key_len ) )
            break;

        evbuffer_drain( inbuf, 1 );
    }

    fprintf( stderr, "got it!\n" );
    evbuffer_drain( inbuf, key_len );
    setState( handshake, AWAITING_CRYPTO_SELECT );
    return READ_AGAIN;
}

static int
readCryptoSelect( tr_handshake * handshake, struct evbuffer * inbuf )
{
    uint32_t crypto_select;
    uint16_t pad_d_len;
    const size_t needlen = sizeof(uint32_t) + sizeof(uint16_t);

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    tr_peerIoReadUint32( handshake->io, inbuf, &crypto_select );
    assert( crypto_select==1 || crypto_select==2 );
    handshake->crypto_select = crypto_select;
    fprintf( stderr, "crypto select is %d\n", crypto_select );

    tr_peerIoReadUint16( handshake->io, inbuf, &pad_d_len );
    fprintf( stderr, "pad_d_len is %d\n", (int)pad_d_len );
    assert( pad_d_len <= 512 );
    handshake->pad_d_len = pad_d_len;

    setState( handshake, AWAITING_PAD_D );
    return READ_AGAIN;
}

static int
readPadD( tr_handshake * handshake, struct evbuffer * inbuf )
{
    const size_t needlen = handshake->pad_d_len;
    uint8_t * tmp;

fprintf( stderr, "pad d: need %d, got %d\n", (int)needlen, (int)EVBUFFER_LENGTH(inbuf) );
    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    tmp = tr_new( uint8_t, needlen );
    tr_peerIoReadBytes( handshake->io, inbuf, tmp, needlen );
    tr_free( tmp );

    tr_peerIoSetEncryption( handshake->io,
                                    handshake->crypto_select );

    setState( handshake, AWAITING_HANDSHAKE );
    return READ_AGAIN;
}

/*ccc*/
static int
readHandshake( tr_handshake * handshake, struct evbuffer * inbuf )
{
    int i;
    int ltep = 0;
    int azmp = 0;
    int isEncrypted;
    uint8_t pstrlen;
    uint8_t * pstr;
    uint8_t reserved[8];
    uint8_t hash[SHA_DIGEST_LENGTH];
    int bytesRead = 0;

fprintf( stderr, "handshake payload: need %d, got %d\n", (int)HANDSHAKE_SIZE, (int)EVBUFFER_LENGTH(inbuf) );

    if( EVBUFFER_LENGTH(inbuf) < HANDSHAKE_SIZE )
        return READ_MORE;

    /* pstrlen */
    pstrlen = EVBUFFER_DATA(inbuf)[0];
    fprintf( stderr, "pstrlen 1 is %d [%c]\n", (int)pstrlen, pstrlen );
    fprintf( stderr, "the buf is [%c][%c][%c][%c]",
        EVBUFFER_DATA(inbuf)[0], 
        EVBUFFER_DATA(inbuf)[1], 
        EVBUFFER_DATA(inbuf)[2], 
        EVBUFFER_DATA(inbuf)[3] );
    isEncrypted = pstrlen != 19;
    tr_peerIoSetEncryption( handshake->io, isEncrypted
        ? PEER_ENCRYPTION_RC4
        : PEER_ENCRYPTION_PLAINTEXT );
    if( isEncrypted ) {
        fprintf( stderr, "I guess it's encrypted...\n" );
        if( tr_peerIoIsIncoming( handshake->io ) ) {
            setState( handshake, AWAITING_YA );
            return READ_AGAIN;
        }
        tr_cryptoDecrypt( handshake->crypto, 1, &pstrlen, &pstrlen );
    }
    bytesRead++;
    evbuffer_drain( inbuf, 1 );
    fprintf( stderr, "pstrlen is %d [%c]\n", (int)pstrlen, pstrlen );
    assert( pstrlen == 19 );

    /* pstr (BitTorrent) */
    pstr = tr_new( uint8_t, pstrlen+1 );
    tr_peerIoReadBytes( handshake->io, inbuf, pstr, pstrlen );
    pstr[pstrlen] = '\0';
    fprintf( stderr, "pstrlen is [%s]\n", pstr );
    bytesRead += pstrlen;
    assert( !strcmp( (char*)pstr, "BitTorrent protocol" ) );
    tr_free( pstr );

    /* reserved bytes */
    tr_peerIoReadBytes( handshake->io, inbuf, reserved, sizeof(reserved) );
    bytesRead += sizeof(reserved);

    /* torrent hash */
    tr_peerIoReadBytes( handshake->io, inbuf, hash, sizeof(hash) );
    bytesRead += sizeof(hash);
    if( tr_peerIoIsIncoming( handshake->io ) )
    {
        assert( !tr_peerIoHasTorrentHash( handshake->io ) );
        tr_peerIoSetTorrentHash( handshake->io, hash );
    }
    else
    {
        assert( tr_peerIoHasTorrentHash( handshake->io ) );
        assert( !memcmp( hash, tr_peerIoGetTorrentHash(handshake->io), SHA_DIGEST_LENGTH ) );
    }

    /* peer id */
    tr_peerIoReadBytes( handshake->io, inbuf, handshake->peer_id, sizeof(handshake->peer_id) );
    tr_peerIoSetPeersId( handshake->io, handshake->peer_id );
    bytesRead += sizeof(handshake->peer_id);
    handshake->have_peer_id = TRUE;

    assert( bytesRead == HANDSHAKE_SIZE );

    /**
    ***
    **/

    ltep = HANDSHAKE_HAS_EXTMSGS( reserved ) ? 1 : 0;
    azmp = HANDSHAKE_HAS_AZPROTO( reserved ) ? 1 : 0;
    if( ltep && azmp ) {
        switch( HANDSHAKE_GET_EXTPREF( reserved ) ) {
            case HANDSHAKE_EXTPREF_LTEP_FORCE:
            case HANDSHAKE_EXTPREF_LTEP_PREFER:
                azmp = 0;
                break;
            case HANDSHAKE_EXTPREF_AZMP_FORCE:
            case HANDSHAKE_EXTPREF_AZMP_PREFER:
                ltep = 0;
                break;
        }
    }
    assert( !ltep || !azmp );
         if( ltep ) { i = LT_EXTENSIONS_LTEP; fprintf(stderr,"using ltep\n" ); }
    else if( azmp ) { i = LT_EXTENSIONS_AZMP; fprintf(stderr,"using azmp\n" ); }
    else            { i = LT_EXTENSIONS_NONE; fprintf(stderr,"using no extensions\n" ); }
    tr_peerIoSetExtension( handshake->io, i );


    if( i == LT_EXTENSIONS_LTEP )
    {
        sendLtepHandshake( handshake );
        return READ_DONE;
    }
    else if( !tr_peerIoIsIncoming( handshake->io ) && ( i != LT_EXTENSIONS_AZMP ) )
    {
        fireDoneCB( handshake, TRUE );
        return READ_DONE;
    }


    fprintf( stderr, " UNHANDLED -- azmp " );
    return 0;
}

/**
***
**/

static ReadState
canRead( struct bufferevent * evin, void * arg )
{
    tr_handshake * handshake = (tr_handshake *) arg;
    struct evbuffer * inbuf = EVBUFFER_INPUT ( evin );
    ReadState ret;
    fprintf( stderr, "handshake %p handling canRead; state is [%s]\n", handshake, getStateName(handshake->state) );

    switch( handshake->state )
    {
        case AWAITING_HANDSHAKE:       ret = readHandshake    ( handshake, inbuf ); break;
        case AWAITING_YA:              ret = readYa           ( handshake, inbuf ); break;
        case AWAITING_PAD_A:           ret = readPadA         ( handshake, inbuf ); break;
        case AWAITING_CRYPTO_PROVIDE:  ret = readCryptoProvide( handshake, inbuf ); break;
        case AWAITING_PAD_C:           ret = readPadC         ( handshake, inbuf ); break;
        case AWAITING_IA:              ret = readIA           ( handshake, inbuf ); break;

        case AWAITING_YB:              ret = readYb           ( handshake, inbuf ); break;
        case AWAITING_VC:              ret = readVC           ( handshake, inbuf ); break;
        case AWAITING_CRYPTO_SELECT:   ret = readCryptoSelect ( handshake, inbuf ); break;
        case AWAITING_PAD_D:           ret = readPadD         ( handshake, inbuf ); break;

        default: assert( 0 );
    }

    return ret;
}

static void
tr_handshakeFree( tr_handshake * handshake )
{
    tr_free( handshake );
}

static void
fireDoneCB( tr_handshake * handshake, int isConnected )
{
    const uint8_t * peer_id = isConnected && handshake->have_peer_id
        ? handshake->peer_id
        : NULL;
fprintf( stderr, "handshake %p: firing done.  connected==%d\n", handshake, isConnected );
    (*handshake->doneCB)(handshake, handshake->io, isConnected, peer_id, handshake->doneUserData);
    tr_handshakeFree( handshake );
}

static void
gotError( struct bufferevent * evbuf UNUSED, short what, void * arg )
{
    tr_handshake * handshake = (tr_handshake *) arg;
fprintf( stderr, "handshake %p: got error [%s]; what==%hd... state was [%s]\n", handshake, strerror(errno), what, getStateName(handshake->state) );

    /* if the error happened while we were sending a public key, we might
     * have encountered a peer that doesn't do encryption... reconnect and
     * try a plaintext handshake */
    if(    ( ( handshake->state == AWAITING_YB ) || ( handshake->state == AWAITING_VC ) )
        && ( handshake->encryptionPreference != HANDSHAKE_ENCRYPTION_REQUIRED )
        && ( !tr_peerIoReconnect( handshake->io ) ) )
    {
fprintf( stderr, "handshake %p trying again in plaintext...\n", handshake );
        handshake->encryptionPreference = HANDSHAKE_PLAINTEXT_REQUIRED;
        sendPlaintextHandshake( handshake );
    }
    else
    {
        tr_peerIoSetIOFuncs( handshake->io, NULL, NULL, NULL, NULL );
        fireDoneCB( handshake, FALSE );
    }
}

/**
***
**/

tr_handshake*
tr_handshakeNew( tr_peerIo        * io,
                 int                encryptionPreference,
                 handshakeDoneCB    doneCB,
                 void             * doneUserData )
{
    tr_handshake * handshake;

//w00t
//static int count = 0;
//if( count++ ) return NULL;

    handshake = tr_new0( tr_handshake, 1 );
    handshake->io = io;
    handshake->crypto = tr_peerIoGetCrypto( io );
    handshake->encryptionPreference = encryptionPreference;
    handshake->doneCB = doneCB;
    handshake->doneUserData = doneUserData;
    handshake->handle = tr_peerIoGetHandle( io );

    tr_peerIoSetIOMode( io, EV_READ|EV_WRITE, 0 );
    tr_peerIoSetIOFuncs( io, canRead, NULL, gotError, handshake );

fprintf( stderr, "handshake %p: new handshake for io %p\n", handshake, io );

    if( tr_peerIoIsIncoming( io ) )
    {
        setReadState( handshake, AWAITING_HANDSHAKE );
    }
    else
    {
        sendHandshake( handshake );
    }

    return handshake;
}

void
tr_handshakeAbort( tr_handshake * handshake )
{
    tr_peerIoFree( handshake->io );
    tr_handshakeFree( handshake );
}
