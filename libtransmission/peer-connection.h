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

#ifndef TR_PEER_CONNECTION_H
#define TR_PEER_CONNECTION_H

/**
***
**/

struct in_addr;
struct evbuffer;
struct bufferevent;
struct tr_handle;
struct tr_crypto;
struct tr_torrent;
typedef struct tr_peerConnection tr_peerConnection;

enum
{
    LT_EXTENSIONS_NONE,
    LT_EXTENSIONS_LTEP,
    LT_EXTENSIONS_AZMP
};

/**
***
**/

tr_peerConnection*
      tr_peerConnectionNewOutgoing( struct tr_handle   * handle,
                                    struct in_addr     * addr,
                                    int                  port,
                                    struct tr_torrent  * torrent );

tr_peerConnection*
      tr_peerConnectionNewIncoming( struct tr_handle   * handle,
                                    struct in_addr     * addr,
                                    int                  socket );

void  tr_peerConnectionFree      ( tr_peerConnection  * connection );

tr_handle* tr_peerConnectionGetHandle( tr_peerConnection * connection );

/**
***
**/

void  tr_peerConnectionSetExtension( tr_peerConnection * connection,
                                     int                 lt_extensions );

int   tr_peerConnectionGetExtension( const tr_peerConnection * connection );

struct tr_torrent*
      tr_peerConnectionGetTorrent( tr_peerConnection * connection );

void  tr_peerConnectionSetTorrent( tr_peerConnection * connection,
                                                struct tr_torrent * tor );

int   tr_peerConnectionReconnect( tr_peerConnection * connection );

int   tr_peerConnectionIsIncoming( const tr_peerConnection * connection );

/**
***
**/

void  tr_peerConnectionSetPeersId( tr_peerConnection * connection,
                                   const uint8_t     * peer_id );

const uint8_t*
      tr_peerConnectionGetPeersId( const tr_peerConnection * connection );

/**
***
**/

typedef enum { READ_MORE, READ_AGAIN, READ_DONE } ReadState;
typedef ReadState (*tr_can_read_cb)(struct bufferevent*, void* user_data);
typedef void (*tr_did_write_cb)(struct bufferevent *, void *);
typedef void (*tr_net_error_cb)(struct bufferevent *, short what, void *);

void  tr_peerConnectionSetIOFuncs( tr_peerConnection  * connection,
                                   tr_can_read_cb       readcb,
                                   tr_did_write_cb      writecb,
                                   tr_net_error_cb      errcb,
                                   void               * user_data );

void  tr_peerConnectionSetIOMode ( tr_peerConnection   * connection,
                                   short                 enable_mode,
                                   short                 disable_mode );

void  tr_peerConnectionReadOrWait( tr_peerConnection * connection );

void tr_peerConnectionWrite( tr_peerConnection   * connection,
                             const void          * writeme,
                             int                   writeme_len );

void tr_peerConnectionWriteBuf( tr_peerConnection     * connection,
                                const struct evbuffer * buf );

/**
***
**/

struct tr_crypto* tr_peerConnectionGetCrypto( tr_peerConnection * connection );

typedef enum
{
    /* these match the values in MSE's crypto_select */
    PEER_ENCRYPTION_PLAINTEXT = (1<<0),
    PEER_ENCRYPTION_RC4       = (1<<1)
}
EncryptionMode;

void tr_peerConnectionSetEncryption( tr_peerConnection  * connection,
                                     int                  encryptionMode );

void tr_peerConnectionWriteBytes   ( tr_peerConnection  * conn,
                                     struct evbuffer    * outbuf,
                                     const void         * bytes,
                                     int                  byteCount );

void tr_peerConnectionWriteUint16 ( tr_peerConnection   * conn,
                                    struct evbuffer     * outbuf,
                                    uint16_t              writeme );

void tr_peerConnectionWriteUint32 ( tr_peerConnection   * conn,
                                    struct evbuffer     * outbuf,
                                    uint32_t              writeme );

void tr_peerConnectionReadBytes   ( tr_peerConnection   * conn,
                                    struct evbuffer     * inbuf,
                                    void                * bytes,
                                    int                   byteCount );

void tr_peerConnectionReadUint16  ( tr_peerConnection   * conn,
                                    struct evbuffer     * inbuf,
                                    uint16_t            * setme );

void tr_peerConnectionReadUint32  ( tr_peerConnection   * conn,
                                    struct evbuffer     * inbuf,
                                    uint32_t            * setme );


#endif
