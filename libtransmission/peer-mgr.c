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
#include <stdlib.h> /* qsort */
#include <stdio.h> /* printf */

#include "transmission.h"
#include "handshake.h"
#include "completion.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-mgr-private.h"
#include "peer-msgs.h"
#include "ptrarray.h"
#include "timer.h"
#include "utils.h"

#define MINUTES_TO_MSEC(N) ((N) * 60 * 1000)

/* how frequently to change which peers are choked */
#define RECHOKE_PERIOD_SECONDS (MINUTES_TO_MSEC(10))

/* how many downloaders to unchoke per-torrent.
 * http://wiki.theory.org/BitTorrentSpecification#Choking_and_Optimistic_Unchoking */
#define NUM_DOWNLOADERS_TO_UNCHOKE 4

/* across all torrents, how many peers maximum do we want connected? */
#define MAX_CONNECTED_PEERS 80

typedef struct
{
    uint8_t hash[SHA_DIGEST_LENGTH];
    tr_ptrArray * peers; /* tr_peer */
    tr_timer_tag choke_tag;
    tr_torrent * tor;
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

static int chokePulse( void * vtorrent );

static int
peerCompare( const void * va, const void * vb )
{
    const tr_peer * a = (const tr_peer *) va;
    const tr_peer * b = (const tr_peer *) vb;
    return memcmp( &a->in_addr, &b->in_addr, sizeof(struct in_addr) );
}

static int
peerCompareToAddr( const void * va, const void * vb )
{
    const tr_peer * a = (const tr_peer *) va;
    const struct in_addr * b = (const struct in_addr *) vb;
    return memcmp( &a->in_addr, b, sizeof(struct in_addr) );
}

static tr_peer*
getExistingPeer( Torrent * torrent, const struct in_addr * in_addr )
{
    return (tr_peer*) tr_ptrArrayFindSorted( torrent->peers,
                                             in_addr,
                                             peerCompareToAddr );
}

static tr_peer*
getPeer( Torrent * torrent, const struct in_addr * in_addr )
{
    tr_peer * peer = getExistingPeer( torrent, in_addr );
    if( peer == NULL )
    {
        peer = tr_new0( tr_peer, 1 );
        memcpy( &peer->in_addr, in_addr, sizeof(struct in_addr) );
        tr_ptrArrayInsertSorted( torrent->peers, peer, peerCompare );
fprintf( stderr, "getPeer: torrent %p now has %d peers\n", torrent, tr_ptrArraySize(torrent->peers) );
    }
    return peer;
}

static void
freePeer( tr_peer * peer )
{
    tr_peerMsgsFree( peer->msgs );
    tr_bitfieldFree( peer->have );
    tr_bitfieldFree( peer->blame );
    tr_bitfieldFree( peer->banned );
    tr_peerIoFree( peer->io );
    tr_free( peer->client );
    tr_free( peer );
}

static void
freeTorrent( tr_peerMgr * manager, Torrent * t )
{
    int i, size;
    tr_peer ** peers;

    assert( manager != NULL );
    assert( t != NULL );
    assert( t->peers != NULL );

    peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &size );
    tr_timerFree( &t->choke_tag );
    for( i=0; i<size; ++i )
        freePeer( peers[i] );
    tr_ptrArrayFree( t->peers );
    tr_ptrArrayRemoveSorted( manager->torrents, t, torrentCompare );
    tr_free( t );
}

/**
***
**/

tr_peerMgr*
tr_peerMgrNew( tr_handle * handle )
{
    tr_peerMgr * m = tr_new0( tr_peerMgr, 1 );
    m->handle = handle;
    m->torrents = tr_ptrArrayNew( );
    return m;
}

void
tr_peerMgrFree( tr_peerMgr * manager )
{
    while( !tr_ptrArrayEmpty( manager->torrents ) )
        freeTorrent( manager, (Torrent*)tr_ptrArrayNth( manager->torrents,0) );
    tr_ptrArrayFree( manager->torrents );
    tr_free( manager );
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
    Torrent * t = getExistingTorrent( manager, hash );
    uint16_t port;
    const struct in_addr * in_addr;

    fprintf( stderr, "peer-mgr: torrent [%s] finished a handshake; isConnected is %d\n", t->tor->info.name, isConnected );

    assert( io != NULL );

    in_addr = tr_peerIoGetAddress( io, &port );

    /* if we couldn't connect or were snubbed,
     * the peer's probably not worth remembering. */
    if( !ok ) {
        tr_peer * peer = getExistingPeer( t, in_addr );
        fprintf( stderr, "peer-mgr: torrent [%s] got a bad one, and you know what? fuck them.\n", t->tor->info.name );
        if( peer ) {
            tr_ptrArrayRemoveSorted( t->peers, peer, peerCompare );
            freePeer( peer );
        } else  {
            tr_peerIoFree( io );
        }
        --manager->connectionCount;
        return;
    }

#if 0
    /* ONLY DO THIS TEST FOR INCOMING CONNECTIONS */
    /* check for duplicates */
    if( getExistingPeer( t, in_addr ) ) {
        tr_dbg( "dropping a duplicate connection... dropping." );
        tr_peerIoFree( io );
        return;
    }
#endif

    if( 1 ) {
        tr_peer * peer = getPeer( t, in_addr );
        peer->port = port;
        peer->msgs = tr_peerMsgsNew( t->tor, peer );
        chokePulse( t );
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

static void
maybeConnect( tr_peerMgr * manager, Torrent * t, tr_peer * peer )
{
    if( tr_peerMgrIsAcceptingConnections( manager ) )
    {
        fprintf( stderr, "peer-mgr: torrent [%s] is handshaking with a new peer %08x:%04x\n",
                 t->tor->info.name,
                 (uint32_t) peer->in_addr.s_addr, peer->port );

        peer->io = tr_peerIoNewOutgoing( manager->handle, &peer->in_addr, peer->port, t->hash );

        tr_handshakeAdd( peer->io, HANDSHAKE_ENCRYPTION_PREFERRED,
                         myHandshakeDoneCB, manager );
    }
}

void
tr_peerMgrAddPex( tr_peerMgr     * manager,
                  const uint8_t  * torrentHash,
                  int              from,
                  const tr_pex   * pex,
                  int              pexCount )
{
    int i;
    const tr_pex * walk = pex;
    Torrent * t = getExistingTorrent( manager, torrentHash );
    for( i=0; i<pexCount; ++i )
    {
        tr_peer * peer = getPeer( t, &walk->in_addr );
        peer->port = walk->port;
        peer->from = from;
        maybeConnect( manager, t, peer );
    }
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
    Torrent * t = getExistingTorrent( manager, torrentHash );
    for( i=0; i<peerCount; ++i )
    {
        tr_peer * peer;
        struct in_addr addr;
        uint16_t port;
        memcpy( &addr, walk, 4 ); walk += 4;
        memcpy( &port, walk, 2 ); walk += 2;
        peer = getPeer( t, &addr );
        peer->port = port;
        peer->from = from;
        maybeConnect( manager, t, peer );
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
tr_pexCompare( const void * va, const void * vb )
{
    const tr_pex * a = (const tr_pex *) va;
    const tr_pex * b = (const tr_pex *) vb;
    int i = memcmp( &a->in_addr, &b->in_addr, sizeof(struct in_addr) );
    if( i ) return i;
    if( a->port < b->port ) return -1;
    if( a->port > b->port ) return 1;
    return 0;
}

int tr_pexCompare( const void * a, const void * b );


int
tr_peerMgrGetPeers( tr_peerMgr      * manager,
                    const uint8_t   * torrentHash,
                    tr_pex         ** setme_pex )
{
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    int i, peerCount;
    const tr_peer ** peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
    tr_pex * pex = tr_new( tr_pex, peerCount );
    tr_pex * walk = pex;

    for( i=0; i<peerCount; ++i, ++walk )
    {
        walk->in_addr = peers[i]->in_addr;
        walk->port = peers[i]->port;
        walk->flags = '\0'; /* FIXME */
    }

    assert( ( walk - pex ) == peerCount );
    qsort( pex, peerCount, sizeof(tr_pex), tr_pexCompare );
    *setme_pex = pex;
    return peerCount;
}

void
tr_peerMgrStartTorrent( tr_peerMgr     * manager UNUSED,
                        const uint8_t  * torrentHash UNUSED)
{
    //fprintf( stderr, "FIXME\n" );
}

void
tr_peerMgrStopTorrent( tr_peerMgr     * manager UNUSED,
                       const uint8_t  * torrentHash UNUSED )
{
    //fprintf( stderr, "FIXME\n" );
}

void
tr_peerMgrAddTorrent( tr_peerMgr * manager,
                      tr_torrent * tor )
{
    Torrent * t;

    assert( tor != NULL );
    assert( getExistingTorrent( manager, tor->info.hash ) == NULL );

    t = tr_new0( Torrent, 1 );
    t->tor = tor;
    t->peers = tr_ptrArrayNew( );
    t->choke_tag = tr_timerNew( manager->handle,
                                chokePulse, t, NULL, 
                                RECHOKE_PERIOD_SECONDS );
    memcpy( t->hash, tor->info.hash, SHA_DIGEST_LENGTH );
    tr_ptrArrayInsertSorted( manager->torrents, t, torrentCompare );
}

void
tr_peerMgrRemoveTorrent( tr_peerMgr     * manager,
                         const uint8_t  * torrentHash )
{
    Torrent * t = getExistingTorrent( manager, torrentHash );
    if( t != NULL ) {
        tr_peerMgrStopTorrent( manager, torrentHash );
        freeTorrent( manager, t );
    }
}

void
tr_peerMgrTorrentAvailability( const tr_peerMgr * manager,
                               const uint8_t    * torrentHash,
                               int8_t           * tab,
                               int                tabCount )
{
    int i;
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    const tr_torrent * tor = t->tor;
    const float interval = tor->info.pieceCount / (float)tabCount;

    for( i=0; i<tabCount; ++i )
    {
        const int piece = i * interval;

        if( tor == NULL )
            tab[i] = 0;
        else if( tr_cpPieceIsComplete( tor->completion, piece ) )
            tab[i] = -1;
        else {
            int j, peerCount;
            const tr_peer ** peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
            for( j=0; j<peerCount; ++j )
                if( tr_bitfieldHas( peers[j]->have, i ) )
                    ++tab[i];
        }
    }
}


void
tr_peerMgrTorrentStats( const tr_peerMgr * manager,
                        const uint8_t    * torrentHash,
                        int              * setmePeersTotal,
                        int              * setmePeersConnected,
                        int              * setmePeersSendingToUs,
                        int              * setmePeersGettingFromUs,
                        int              * setmePeersFrom )
{
    int i, size;
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    const tr_peer ** peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &size );

    *setmePeersTotal          = size;
    *setmePeersConnected      = 0;
    *setmePeersSendingToUs    = 0;
    *setmePeersGettingFromUs  = 0;

    for( i=0; i<size; ++i )
    {
        const tr_peer * peer = peers[i];

        if( peer->io == NULL ) /* not connected */
            continue;

        ++*setmePeersConnected;

        ++setmePeersFrom[peer->from];

        if( tr_peerIoGetRateToPeer( peer->io ) > 0.01 )
            ++*setmePeersGettingFromUs;

        if( tr_peerIoGetRateToClient( peer->io ) > 0.01 )
            ++*setmePeersSendingToUs;
    }
}

struct tr_peer_stat *
tr_peerMgrPeerStats( const tr_peerMgr  * manager,
                     const uint8_t     * torrentHash,
                     int               * setmeCount UNUSED )
{
    int i, size;
    const Torrent * t = getExistingTorrent( (tr_peerMgr*)manager, torrentHash );
    const tr_peer ** peers = (const tr_peer **) tr_ptrArrayPeek( t->peers, &size );
    tr_peer_stat * ret;

    ret = tr_new0( tr_peer_stat, size );

    for( i=0; i<size; ++i )
    {
        const tr_peer * peer = peers[i];
        const int live = peer->io != NULL;
        tr_peer_stat * stat = ret + i;

        tr_netNtop( &peer->in_addr, stat->addr, sizeof(stat->addr) );
        stat->port             = peer->port;
        stat->from             = peer->from;
        stat->client           = peer->client;
        stat->progress         = peer->progress;
        stat->isConnected      = live;
        stat->uploadToRate     = tr_peerIoGetRateToPeer( peer->io );
        stat->downloadFromRate = tr_peerIoGetRateToClient( peer->io );
        stat->isDownloading    = stat->uploadToRate > 0.01;
        stat->isUploading      = stat->downloadFromRate > 0.01;
    }

    *setmeCount = size;
    return ret;
}

void
tr_peerMgrDisablePex( tr_peerMgr    * manager,
                      const uint8_t * torrentHash,
                      int             disable)
{
    Torrent * t = getExistingTorrent( manager, torrentHash );
    tr_torrent * tor = t->tor;

    if( ( tor->pexDisabled != disable ) && ! ( TR_FLAG_PRIVATE & tor->info.flags ) )
    {
        int i, size;
        tr_peer ** peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &size );
        for( i=0; i<size; ++i ) {
            peers[i]->pexEnabled = disable ? 0 : 1;
            peers[i]->lastPexTime = 0;
        }

        tor->pexDisabled = disable;
    }
}

/**
***
**/

typedef struct
{
    tr_peer * peer;
    float rate;
    int isInterested;
}
ChokeData;

static int
compareChokeByRate( const void * va, const void * vb )
{
    const ChokeData * a = ( const ChokeData * ) va;
    const ChokeData * b = ( const ChokeData * ) vb;
    if( a->rate > b->rate ) return -1;
    if( a->rate < b->rate ) return 1;
    return 0;
}

static int
compareChokeByDownloader( const void * va, const void * vb )
{
    const ChokeData * a = ( const ChokeData * ) va;
    const ChokeData * b = ( const ChokeData * ) vb;

    /* primary key: interest */
    if(  a->isInterested && !b->isInterested ) return -1;
    if( !a->isInterested &&  b->isInterested ) return 1;

    /* second key: rate */
    return compareChokeByRate( va, vb );
}

static int
chokePulse( void * vtorrent )
{
    Torrent * t = (Torrent *) vtorrent;
    int i, size, unchoked;
    const int done = tr_cpGetStatus( t->tor->completion ) != TR_CP_INCOMPLETE;
    tr_peer ** peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &size );
    float bestDownloaderRate;
    ChokeData * data;

fprintf( stderr, "rechoking torrent %p, with %d peers\n", t, size );

    if( size < 1 )
        return TRUE;

    data = tr_new( ChokeData, size );
    for( i=0; i<size; ++i ) {
        data[i].peer = peers[i];
        data[i].isInterested = peers[i]->peerIsInterested;
        data[i].rate = done ? tr_peerIoGetRateToPeer( peers[i]->io )
                            : tr_peerIoGetRateToClient( peers[i]->io );
    }

    /* find the best downloaders and unchoke them */
    qsort( data, size, sizeof(ChokeData), compareChokeByDownloader );
    bestDownloaderRate = data[0].rate;
    for( i=unchoked=0; i<size && unchoked<NUM_DOWNLOADERS_TO_UNCHOKE; ++i ) {
        if( data[i].peer->msgs != NULL ) {
            tr_peerMsgsSetChoke( data[i].peer->msgs, FALSE );
            ++unchoked;
        }
    }
    memmove( data, data+i, sizeof(ChokeData)*(size-i) );
    size -= i;

    /* of those remaining, unchoke those that are faster than the downloaders */
    qsort( data, size, sizeof(ChokeData), compareChokeByRate );
    for( i=0; i<size && data[i].rate >= bestDownloaderRate; ++i )
        tr_peerMsgsSetChoke( data[i].peer->msgs, FALSE );
    memmove( data, data+i, sizeof(ChokeData)*(size-i) );
    size -= i;

    /* of those remaining, optimistically unchoke one; choke the rest */
    if( size > 0 ) {
        const int optimistic = tr_rand( size );
        for( i=0; i<size; ++i )
            tr_peerMsgsSetChoke( data[i].peer->msgs, i!=optimistic );
    }

    /* cleanup */
    tr_free( data );
    return TRUE;
}
