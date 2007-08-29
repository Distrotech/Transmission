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
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <event.h>
#include "transmission.h"
#include "crypto.h"
#include "net.h"
#include "peer-connection.h"
#include "trevent.h"
#include "utils.h"

/**
***
**/

struct tr_peerConnection
{
    struct tr_handle * handle;
    struct tr_torrent * torrent;

    struct in_addr in_addr;
    int port;
    int socket;
    int extensions;
    struct bufferevent * bufev;
    uint8_t peerId[20];

    unsigned int isEncrypted : 1;
    unsigned int isIncoming : 1;
    unsigned int peerIdIsSet : 1;

    tr_can_read_cb     canRead;
    tr_did_write_cb    didWrite;
    tr_net_error_cb    gotError;
    void             * userData;

    tr_crypto * crypto;
};

/**
***
**/

static void
didWriteWrapper( struct bufferevent * e, void * userData )
{
    tr_peerConnection * c = (tr_peerConnection *) userData;
    assert( c->didWrite != NULL );
    (*c->didWrite)( e, c->userData );
}

static void
canReadWrapper( struct bufferevent * e, void * userData )
{
    tr_peerConnection * c = (tr_peerConnection *) userData;

    assert( c->canRead != NULL );

    for( ;; ) {
        const int ret = (*c->canRead)( e, c->userData );
        switch( ret ) {
            case READ_DONE: return;
            case READ_AGAIN: continue;
            case READ_MORE: tr_peerConnectionSetIOMode( c, EV_READ ); return;
        }
    }
}

static void
gotErrorWrapper( struct bufferevent * e, short what, void * userData )
{
    tr_peerConnection * c = (tr_peerConnection *) userData;
    assert( c->gotError != NULL );
    (*c->gotError)( e, what, c->userData );
}

/**
***
**/

static tr_peerConnection*
tr_peerConnectionNew( struct tr_handle  * handle,
                      struct in_addr    * in_addr,
                      struct tr_torrent * torrent,
                      int                 isIncoming,
                      int                 socket )
{
    tr_peerConnection * c;
    c = tr_new0( tr_peerConnection, 1 );
    c->torrent = torrent;
    c->crypto = tr_cryptoNew( torrent ? torrent->info.hash : NULL, isIncoming );
    c->handle = handle;
    c->in_addr = *in_addr;
    c->socket = socket;
    c->bufev = bufferevent_new( c->socket,
                                canReadWrapper,
                                didWriteWrapper,
                                gotErrorWrapper,
                                c );
    return c;
}

tr_peerConnection*
tr_peerConnectionNewIncoming( struct tr_handle  * handle,
                              struct in_addr    * in_addr,
                              int                 socket )
{
    tr_peerConnection * c =
        tr_peerConnectionNew( handle, in_addr, NULL, 1, socket );
    c->port = -1;
    return c;
}

tr_peerConnection*
tr_peerConnectionNewOutgoing( struct tr_handle  * handle,
                              struct in_addr    * in_addr,
                              int                 port,
                              struct tr_torrent * torrent )
{
    tr_peerConnection * c;

    assert( handle != NULL );
    assert( in_addr != NULL );
    assert( port >= 0 );
    assert( torrent != NULL );

    c = tr_peerConnectionNew( handle, in_addr, torrent, 0,
                              tr_netOpenTCP( in_addr, port, 0 ) );
    c->port = port;
    return c;
}

void
tr_peerConnectionFree( tr_peerConnection * c )
{
    bufferevent_free( c->bufev );
    tr_netClose( c->socket );
    tr_cryptoFree( c->crypto );
    tr_free( c );
}

void 
tr_peerConnectionSetIOFuncs( tr_peerConnection  * connection,
                             tr_can_read_cb       readcb,
                             tr_did_write_cb      writecb,
                             tr_net_error_cb      errcb,
                             void               * userData )
{
    connection->canRead = readcb;
    connection->didWrite = writecb;
    connection->gotError = errcb;
    connection->userData = userData;
}

void
tr_peerConnectionSetIOMode( tr_peerConnection * c, short mode )
{
    tr_setBufferEventMode( c->handle, c->bufev, mode );
}

void
tr_peerConnectionReadOrWait( tr_peerConnection * c )
{
    if( EVBUFFER_LENGTH( c->bufev->input ) )
        canReadWrapper( c->bufev, c );
    else
        tr_peerConnectionSetIOMode( c, EV_READ );
}

int
tr_peerConnectionIsIncoming( const tr_peerConnection * c )
{
    return c->isIncoming;
}

int
tr_peerConnectionReconnect( tr_peerConnection * connection )
{
    assert( !tr_peerConnectionIsIncoming( connection ) );

    if( connection->socket >= 0 )
        tr_netClose( connection->socket );

    connection->socket = tr_netOpenTCP( &connection->in_addr,
                                        connection->port, 0 );

    return connection->socket >= 0 ? 0 : -1;
}


tr_crypto* 
tr_peerConnectionGetCrypto( tr_peerConnection * c )
{
    return c->crypto;
}

/**
***
**/

void
tr_peerConnectionWrite( tr_peerConnection   * connection,
                        const void          * writeme,
                        int                   writeme_len )
{
    tr_bufferevent_write( connection->handle,
                          connection->bufev,
                          writeme,
                          writeme_len );
}

void
tr_peerConnectionWriteBuf  ( tr_peerConnection   * connection,
                             struct evbuffer     * buf )
{
    tr_peerConnectionWrite( connection,
                            EVBUFFER_DATA(buf),
                            EVBUFFER_LENGTH(buf) );
}

/**
***
**/

void
tr_peerConnectionSetTorrent( tr_peerConnection  * connection,
                             struct tr_torrent  * torrent )
{
    connection->torrent = torrent;

    tr_cryptoSetTorrentHash( connection->crypto, torrent->info.hash );
}

struct tr_torrent*
tr_peerConnectionGetTorrent( tr_peerConnection * connection )
{
    return connection->torrent;
}

/**
***
**/

void
tr_peerConnectionSetPeersId( tr_peerConnection * connection,
                             const uint8_t     * peer_id )
{
    assert( connection != NULL );

    if(( connection->peerIdIsSet = peer_id != NULL ))
        memcpy( connection->peerId, peer_id, 20 );
    else
        memset( connection->peerId, 0, 20 );
}

const uint8_t* 
tr_peerConnectionGetPeersId( const tr_peerConnection * connection )
{
    assert( connection != NULL );
    assert( connection->peerIdIsSet );

    return connection->peerId;
}

/**
***
**/

void
tr_peerConnectionSetExtension( tr_peerConnection * connection,
                               int                 extensions )
{
    assert( connection != NULL );
    assert( ( extensions == LT_EXTENSIONS_NONE )
         || ( extensions == LT_EXTENSIONS_LTEP )
         || ( extensions == LT_EXTENSIONS_AZMP ) );

    connection->extensions = extensions;
}

int
tr_peerConnectionGetExtension( const tr_peerConnection * connection )
{
    assert( connection != NULL );

    return connection->extensions;
}
