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

#ifndef TR_ENCRYPTION_H
#define TR_ENCRYPTION_H

#include <inttypes.h>

/**
***
**/

struct evbuffer;
typedef struct tr_encryption tr_encryption;

/**
***
**/

tr_encryption*  tr_encryptionNew ( const uint8_t * torrentHash,
                                   int             isIncoming );

void            tr_encryptionFree( tr_encryption * encryption );

/**
***
**/

void            tr_encryptionSetTorrentHash( tr_encryption * encryption,
                                             const uint8_t * torrentHash );

const uint8_t*  tr_encryptionGetTorrentHash( const tr_encryption * encryption );

/**
***
**/

const uint8_t*  tr_encryptionComputeSecret   ( tr_encryption * encryption,
                                               const uint8_t * peerPublicKey );

const uint8_t*  tr_encryptionGetMyPublicKey ( const tr_encryption * encryption,
                                              int                 * setme_len );

void            tr_encryptionDecryptInit( tr_encryption * encryption );

void            tr_encryptionDecrypt    ( tr_encryption  * encryption,
                                          size_t           buflen,
                                          const void     * buf_in,
                                          void           * buf_out );

void            tr_encryptionEncryptInit( tr_encryption * encryption );

void            tr_encryptionEncrypt    ( tr_encryption  * encryption,
                                          size_t           buflen,
                                          const void     * buf_in,
                                          void           * buf_out );

/**
***
**/



void            tr_sha1( uint8_t* setme,
                         const void * content1, int content1_len,
                         ... );

void            tr_sha1_buf( struct evbuffer* outbuf,
                             const void * content1, int content1_len,
                             ... );

/**
***
**/

#endif
