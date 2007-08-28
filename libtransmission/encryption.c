/*
 * This file Copyright (C) 2007 Charles Kerr <charles@rebelbase.com>
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license. 
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id:$
 */

#include <assert.h>
#include <inttypes.h> /* uint8_t */
#include <string.h> /* memcpy */
#include <stdarg.h>

#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/rc4.h>
#include <openssl/sha.h>

#include <event.h>
#include "encryption.h"
#include "utils.h"

/**
***
**/

void
tr_sha1( uint8_t    * setme,
         const void * content1,
         int          content1_len,
         ... )
{
    va_list vl;
    SHA_CTX sha;

    SHA1_Init( &sha );
    SHA1_Update( &sha, content1, content1_len );

    va_start( vl, content1_len );
    for( ;; ) {
        const void * content = (const void*) va_arg( vl, const void* );
        const int content_len = content ? (int) va_arg( vl, int ) : -1;
        if( content==NULL || content_len<1 )
            break;
        SHA1_Update( &sha, content, content_len );
    }
    SHA1_Final( setme, &sha );
  

}

void
tr_sha1_buf( struct evbuffer  * outbuf,
             const void       * content1,
             int                content1_len,
             ... )
{
    uint8_t shabuf[SHA_DIGEST_LENGTH];
    va_list vl;
    SHA_CTX sha;

    SHA1_Init( &sha );
    SHA1_Update( &sha, content1, content1_len );

    va_start( vl, content1_len );
    for( ;; ) {
        const void * content = (const void*) va_arg( vl, const void* );
        const int content_len = content ? (int) va_arg( vl, int ) : -1;
        if( content==NULL || content_len<1 )
            break;
        SHA1_Update( &sha, content, content_len );
    }
    SHA1_Final( shabuf, &sha );

    evbuffer_add( outbuf, shabuf, SHA_DIGEST_LENGTH );
}

/**
***
**/

#define KEY_LEN 96

#define PRIME_LEN 96

static const uint8_t dh_P[PRIME_LEN] =
{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x68, 0xC2, 0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x3A, 0x36, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x05, 0x63,
};

static const uint8_t dh_G[] = { 2 };

struct tr_encryption
{
    DH * dh;
    RC4_KEY dec_key;
    RC4_KEY enc_key;
    uint8_t torrentHash[SHA_DIGEST_LENGTH];
    unsigned int isIncoming       : 1;
    unsigned int torrentHashIsSet : 1;
    unsigned int mySecretIsSet    : 1;
    uint8_t myPublicKey[KEY_LEN];
    uint8_t mySecret[KEY_LEN];

};

/**
***
**/

tr_encryption * 
tr_encryptionNew( const uint8_t * torrentHash,
                  int             isIncoming )
{
    int len, offset;
    tr_encryption * e;

    e = tr_new0( tr_encryption, 1 );
    e->isIncoming = isIncoming ? 1 : 0;
    tr_encryptionSetTorrentHash( e, torrentHash );

    e->dh = DH_new( );
    e->dh->p = BN_bin2bn( dh_P, sizeof(dh_P), NULL );
    e->dh->g = BN_bin2bn( dh_G, sizeof(dh_G), NULL );
    DH_generate_key( e->dh );

    // DH can generate key sizes that are smaller than the size of
    // P with exponentially decreasing probability, in which case
    // the msb's of myPublicKey need to be zeroed appropriately.
    len = DH_size( e->dh );
    offset = KEY_LEN - len;
    assert( len <= KEY_LEN );
    memset( e->myPublicKey, 0, offset );
    BN_bn2bin( e->dh->pub_key, e->myPublicKey + offset );

    return e;
}

void
tr_encryptionFree( tr_encryption * e )
{
    assert( e != NULL );
    assert( e->dh != NULL );

    DH_free( e->dh );
    tr_free( e );
}

/**
***
**/

const uint8_t*
tr_encryptionComputeSecret( tr_encryption * e,
                            const uint8_t * peerPublicKey )
{
    int len, offset;
    uint8_t secret[KEY_LEN];
    BIGNUM * bn = BN_bin2bn( peerPublicKey, KEY_LEN, NULL );
    assert( DH_size(e->dh) == KEY_LEN );

    len = DH_compute_key( secret, bn, e->dh );
    assert( len <= KEY_LEN );
    offset = KEY_LEN - len;
    memset( e->mySecret, 0, offset );
    memcpy( e->mySecret + offset, secret, len );
    e->mySecretIsSet = 1;
    
    BN_free( bn );

    return e->mySecret;
}

const uint8_t*
tr_encryptionGetMyPublicKey( const tr_encryption * e, int * setme_len )
{
    *setme_len = KEY_LEN;
    return e->myPublicKey;
}

/**
***
**/

static void
initRC4( tr_encryption * e, RC4_KEY * setme, const char * key )
{
    SHA_CTX sha;
    uint8_t buf[SHA_DIGEST_LENGTH];

    assert( e->torrentHashIsSet );
    assert( e->mySecretIsSet );

    SHA1_Init( &sha );
    SHA1_Update( &sha, key, 4 );
    SHA1_Update( &sha, e->mySecret, KEY_LEN );
    SHA1_Update( &sha, e->torrentHash, SHA_DIGEST_LENGTH );
    SHA1_Final( buf, &sha );
    RC4_set_key( setme, SHA_DIGEST_LENGTH, buf );
}

void
tr_encryptionDecryptInit( tr_encryption * encryption )
{
    unsigned char discard[1024];
    const char * txt = encryption->isIncoming ? "keyA" : "keyB";
    initRC4( encryption, &encryption->dec_key, txt );
    RC4( &encryption->dec_key, sizeof(discard), discard, discard );
}

void
tr_encryptionDecrypt( tr_encryption * encryption,
                      size_t          buf_len,
                      const void    * buf_in,
                      void          * buf_out )
{
    RC4( &encryption->dec_key, buf_len,
         (const unsigned char*)buf_in,
         (unsigned char*)buf_out );
}

void
tr_encryptionEncryptInit( tr_encryption * encryption )
{
    unsigned char discard[1024];
    const char * txt = encryption->isIncoming ? "keyB" : "keyA";
    initRC4( encryption, &encryption->enc_key, txt );
    RC4( &encryption->enc_key, sizeof(discard), discard, discard );
}

void
tr_encryptionEncrypt( tr_encryption * encryption,
                      size_t          buf_len,
                      const void    * buf_in,
                      void          * buf_out )
{
    RC4( &encryption->enc_key, buf_len,
         (const unsigned char*)buf_in,
         (unsigned char*)buf_out );
}

/**
***
**/

void
tr_encryptionSetTorrentHash( tr_encryption * e,
                             const uint8_t * hash )
{
    e->torrentHashIsSet = hash ? 1 : 0;

    if( hash != NULL )
        memcpy( e->torrentHash, hash, SHA_DIGEST_LENGTH );
    else
        memset( e->torrentHash, 0, SHA_DIGEST_LENGTH );
}

const uint8_t*
tr_encryptionGetTorrentHash( const tr_encryption * e )
{
    assert( e->torrentHashIsSet );

    return e->torrentHash;
}
