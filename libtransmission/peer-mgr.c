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
#include <string.h> /* memcpy, memcmp */

#include <arpa/inet.h>

#include "transmission.h"
#include "handshake.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-work.h"
#include "ptrarray.h"
#include "utils.h"

#define MAX_CONNECTED_PEERS 80

typedef struct
{
    struct in_addr in_addr;
    uint16_t port;
    tr_peerIo * io;
    int from;
}
Peer;

typedef struct
{
    uint8_t hash[SHA_DIGEST_LENGTH];
    tr_ptrArray * peers; /* Peer */
}
Torrent;

struct tr_peerMgr
{
    tr_handle * handle;
    tr_ptrArray * torrents; /* Torrent */
    int connectionCount;
};

/**
***
**/

static int
torrentCompare( const void * va, const void * vb )
{
    const Torrent * a = (const Torrent*) va;
    const Torrent * b = (const Torrent*) vb;
    return memcmp( a->hash, b->hash, SHA_DIGEST_LENGTH );
}

static int
torrentCompareToHash( const void * va, const void * vb )
{
    const Torrent * a = (const Torrent*) va;
    const uint8_t * b_hash = (const uint8_t*) vb;
    return memcmp( a->hash, b_hash, SHA_DIGEST_LENGTH );
}

static Torrent*
getExistingTorrent( tr_peerMgr * manager, const uint8_t * hash )
{
    return (Torrent*) tr_ptrArrayFindSorted( manager->torrents,
                                             hash,
                                             torrentCompareToHash );
}

static Torrent*
getTorrent( tr_peerMgr * manager, const uint8_t * hash )
{
    Torrent * val = getExistingTorrent( manager, hash );
    if( val == NULL )
    {
        val = tr_new0( Torrent, 1 );
        val->peers = tr_ptrArrayNew( );
        memcpy( val->hash, hash, SHA_DIGEST_LENGTH );
        tr_ptrArrayInsertSorted( manager->torrents, val, torrentCompare );
    }

    return val;
}

static int
peerCompare( const void * va, const void * vb )
{
    const Peer * a = (const Peer *) va;
    const Peer * b = (const Peer *) vb;
    return memcmp( &a->in_addr, &b->in_addr, sizeof(struct in_addr) );
}

static int
peerCompareToAddr( const void * va, const void * vb )
{
    const Peer * a = (const Peer *) va;
    const struct in_addr * b = (const struct in_addr *) vb;
    return memcmp( &a->in_addr, b, sizeof(struct in_addr) );
}

static Peer*
getExistingPeer( Torrent * torrent, const struct in_addr * in_addr )
{
    return (Peer*) tr_ptrArrayFindSorted( torrent->peers,
                                          in_addr,
                                          peerCompareToAddr );
}

static Peer*
getPeer( Torrent * torrent, const struct in_addr * in_addr )
{
    Peer * peer = getExistingPeer( torrent, in_addr );
    if( peer == NULL )
    {
        peer = tr_new0( Peer, 1 );
        memcpy( &peer->in_addr, in_addr, sizeof(struct in_addr) );
        tr_ptrArrayInsertSorted( torrent->peers, peer, peerCompare );
    }
    return peer;
}


/**
***
**/

tr_peerMgr*
tr_peerMgrNew( tr_handle * handle )
{
    tr_peerMgr * m = tr_new0( tr_peerMgr, 1 );
    m->handle = handle;
    return m;
}

void
tr_peerMgrFree( tr_peerMgr * manager )
{
    int it, sizet;
    Torrent ** torrents = (Torrent**) tr_ptrArrayPeek( manager->torrents, &sizet );
    for( it=0; it<sizet; ++it )
    {
        int ip, sizep;
        Torrent * t = torrents[it];
        Peer ** peers = (Peer **) tr_ptrArrayPeek( t->peers, &sizep );
        for( ip=0; ip<sizep; ++ip )
        {
            /* FIXME */
        }
    }
}

/**
***
**/

static void
myHandshakeDoneCB( tr_peerIo * io, int isConnected, void * vmanager )
{
    int ok = isConnected;
    const uint8_t * hash = tr_peerIoGetTorrentHash( io );
    tr_peerMgr * manager = (tr_peerMgr*) vmanager;
    Torrent * t = getTorrent( manager, hash );

    /* check for duplicates */
    if( ok ) {
        if ( getExistingPeer( t, tr_peerIoGetAddress(io,NULL) ) ) {
            tr_dbg( "dropping a duplicate connection... dropping." );
            ok = FALSE;
        }
    }

    /* do something with this connection */
    if( !ok ) {
        tr_peerIoFree( io );
        --manager->connectionCount;
    } else {
        uint16_t port;
        Peer * peer = getPeer( t, tr_peerIoGetAddress(io,&port) );
        peer->port = port;
        peer->io = io;
        tr_peerWorkAdd( tr_torrentFindFromHash(manager->handle,hash), io );
    }
}

void
tr_peerMgrAddIncoming( tr_peerMgr      * manager,
                       struct in_addr  * addr,
                       int               socket )
{
    ++manager->connectionCount;

    tr_handshakeAdd( tr_peerIoNewIncoming( manager->handle, addr, socket ),
                     HANDSHAKE_ENCRYPTION_PREFERRED,
                     myHandshakeDoneCB,
                     manager );
}

void
tr_peerMgrAddPeers( tr_peerMgr    * manager,
                    const uint8_t * torrentHash,
                    int             from,
                    const uint8_t * peerCompact,
                    int             peerCount )
{
    int i;
    const uint8_t * walk = peerCompact;
    Torrent * t = getTorrent( manager, torrentHash );

    for( i=0; i<peerCount; ++i )
    {
        Peer * peer;
        struct in_addr addr;
        uint16_t port;

        memcpy( &addr, walk, 4 ); walk += 4;
        memcpy( &port, walk, 2 ); walk += 2;

        peer = getPeer( t, &addr );
        peer->port = port;
        peer->from = from;
    }
}

/**
***
**/

int
tr_peerMgrIsAcceptingConnections( const tr_peerMgr * manager )
{
    return manager->connectionCount < MAX_CONNECTED_PEERS;
}

void
tr_peerMgrSetBlame( tr_peerMgr     * manager UNUSED,
                    const uint8_t  * torrentHash UNUSED,
                    int              pieceIndex UNUSED,
                    int              success UNUSED )
{
    assert( 0 );
}

int
tr_peerMgrGetPeers( tr_peerMgr      * manager UNUSED,
                    const uint8_t   * torrentHash UNUSED,
                    uint8_t        ** setme_compact  UNUSED)
{
    assert( 0 );
    return 0;
}


void
tr_peerMgrStartTorrent( tr_peerMgr     * manager UNUSED,
                        const uint8_t  * torrentHash UNUSED)
{
    assert( 0 );
}

void
tr_peerMgrStopTorrent( tr_peerMgr     * manager UNUSED,
                       const uint8_t  * torrentHash UNUSED )
{
    assert( 0 );
}

void
tr_peerMgrTorrentAvailability( const tr_peerMgr * manager UNUSED,
                               const uint8_t    * torrentHash UNUSED,
                               int8_t           * tab UNUSED,
                               int                tabCount  UNUSED)
{
#if 0
    int i, j, piece;
    float interval;

    tr_torrentReaderLock( tor );

    interval = (float)tor->info.pieceCount / (float)size;
    for( i = 0; i < size; i++ )
    {
        piece = i * interval;

        if( tr_cpPieceIsComplete( tor->completion, piece ) )
        {
            tab[i] = -1;
            continue;
        }

        tab[i] = 0;
        for( j = 0; j < tor->peerCount; j++ )
        {
            if( tr_peerHasPiece( tor->peers[j], piece ) )
            {
                (tab[i])++;
            }
        }
    }

    tr_torrentReaderUnlock( tor );
#endif
    assert( 0 && "FIXME" );
}


void
tr_peerMgrTorrentStats( const tr_peerMgr * manager,
                        const uint8_t    * torrentHash,
                        int              * setmePeersTotal UNUSED,
                        int              * setmePeersConnected UNUSED,
                        int              * setmePeersSendingToUs UNUSED,
                        int              * setmePeersGettingFromUs  UNUSED)
{
    int i, size;
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    const Peer ** peers = (const Peer **) tr_ptrArrayPeek( t->peers, &size );

    *setmePeersTotal          = size;
    *setmePeersConnected      = 0;
    *setmePeersSendingToUs    = 0;
    *setmePeersGettingFromUs  = 0;

    for( i=0; i<size; ++i )
    {
        const Peer * peer = peers[i];
        if( peer->io == NULL )
            continue;
        ++*setmePeersConnected;
#warning FIXME
#if 0
        ++*setmePeersFrom[peer->from];
        if( tr_peerDownloadRate( peer ) > 0.01 )
            ++s->peersSendingToUs;
        if( tr_peerUploadRate( peer ) > 0.01 )
            ++s->peersGettingFromUs;
#endif
    }
}

struct tr_peer_stat *
tr_peerMgrPeerStats( const tr_peerMgr  * manager,
                     const uint8_t     * torrentHash,
                     int               * setmeCount UNUSED )
{
    int i, size;
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    const Peer ** peers = (const Peer **) tr_ptrArrayPeek( t->peers, &size );
    tr_peer_stat * ret;

    ret = tr_new0( tr_peer_stat, size );

    for( i=0; i<size; ++i )
    {
        const Peer * peer = peers[i];
        const int live = peer->io != NULL;
        tr_peer_stat * stat = ret + i;

        tr_netNtop( &peer->in_addr, stat->addr, sizeof(stat->addr) );
        stat->port = peer->port;
        stat->from = peer->from;
        stat->isConnected = live;
        stat->uploadToRate     = tr_peerIoGetRateToPeer( peer->io );
        stat->downloadFromRate = tr_peerIoGetRateToClient( peer->io );
        stat->isDownloading    =  stat->uploadToRate > 0.01;
        stat->isUploading      =  stat->downloadFromRate > 0.01;

#warning FIXME
        //stat->progress = tr_peerProgress( peer );
        //stat->client = tr_peerClient( peer );
    }

    *setmeCount = size;
    return ret;
}

void
tr_peerMgrDisablePex( tr_peerMgr    * manager UNUSED,
                      const uint8_t * torrentHash UNUSED,
                      int             disable UNUSED)
{
#warning FIXME
#if 0
    tr_torrentWriterLock( tor );

    if( ! ( TR_FLAG_PRIVATE & tor->info.flags ) )
    {
        if( tor->pexDisabled != disable )
        {
            int i;
            tor->pexDisabled = disable;
            for( i=0; i<tor->peerCount; ++i )
                tr_peerSetPrivate( tor->peers[i], disable );
        }
    }
#endif
    assert( 0 );

}
