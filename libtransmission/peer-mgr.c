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

/**
***
**/

struct tr_block
{
    unsigned int have          : 1;
    unsigned int dnd           : 1;
    unsigned int low_priority  : 1;
    unsigned int high_priority : 1;
    uint8_t requestCount;
    uint8_t scarcity;
    uint32_t block;
};

#define MAX_SCARCITY UINT8_MAX
#define MAX_REQ_COUNT UINT8_MAX

static void
incrementReqCount( struct tr_block * block )
{
    assert( block != NULL );

    if( block->requestCount < MAX_REQ_COUNT )
        block->requestCount++;
}

static void
incrementScarcity( struct tr_block * block )
{
    assert( block != NULL );

    if( block->scarcity < MAX_SCARCITY )
        block->scarcity++;
}

static int
compareBlockByIndex( const void * va, const void * vb )
{
    const struct tr_block * a = (const struct tr_block *) va;
    const struct tr_block * b = (const struct tr_block *) vb;
    return tr_compareUint32( a->block, b->block );
}

static int
compareBlockByInterest( const void * va, const void * vb )
{
    const struct tr_block * a = (const struct tr_block *) va;
    const struct tr_block * b = (const struct tr_block *) vb;
    int i;

    if( a->dnd != b->dnd )
        return a->dnd ? 1 : -1;

    if( a->have != b->have )
        return a->have ? 1 : -1;

    if(( i = tr_compareUint8( a->requestCount, b->requestCount )))
        return i;

    if( a->high_priority != b->high_priority )
        return a->high_priority ? -1 : 1;

    if( a->low_priority != b->low_priority )
        return a->low_priority ? 1 : -1;

    if(( i = tr_compareUint16( a->scarcity, b->scarcity )))
        return i;

    if(( i = tr_compareUint32( a->block, b->block )))
        return i;

    return 0;
}

/**
***
**/

typedef struct
{
    uint8_t hash[SHA_DIGEST_LENGTH];
    tr_ptrArray * peers; /* tr_peer */
    tr_timer_tag choke_tag;
    tr_timer_tag refill_tag;
    tr_torrent * tor;

    struct tr_block * blocks;
    uint32_t blockCount;

    struct tr_peerMgr * manager;
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
    tr_timerFree( &t->refill_tag );
    for( i=0; i<size; ++i )
        freePeer( peers[i] );
    tr_ptrArrayFree( t->peers );
    tr_ptrArrayRemoveSorted( manager->torrents, t, torrentCompare );
    tr_free( t->blocks );
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

static tr_peer**
getConnectedPeers( Torrent * t, int * setmeCount )
{
    int i, peerCount, connectionCount;
    tr_peer **peers = (tr_peer **) tr_ptrArrayPeek( t->peers, &peerCount );
    tr_peer **ret = tr_new( tr_peer*, peerCount );

    for( i=connectionCount=0; i<peerCount; ++i )
        if( peers[i]->msgs != NULL )
            ret[connectionCount++] = peers[i];

    *setmeCount = connectionCount;
    return ret;
}

static int
refillPulse( void * vtorrent )
{
    uint32_t i;
    int size;
    Torrent * t = (Torrent *) vtorrent;
    tr_peer ** peers = getConnectedPeers( t, &size );
fprintf( stderr, "in refill pulse for [%s]... sorting blocks by interest...", t->tor->info.name );

    /* sort the blocks by interest */
    qsort( t->blocks, t->blockCount, sizeof(struct tr_block), compareBlockByInterest );
fprintf( stderr, " .done.\n" );

    /* walk through all the most interesting blocks */
    for( i=0; i<t->blockCount; ++i )
    {
        const uint32_t b = t->blocks[i].block;
        const uint32_t index = tr_torBlockPiece( t->tor, b );
        const uint32_t begin = ( b * t->tor->blockSize )-( index * t->tor->info.pieceSize );
        const uint32_t length = tr_torBlockCountBytes( t->tor, (int)b );
        int j;

        if( t->blocks[i].have || t->blocks[i].dnd )
            continue;

        if( !size ) { /* all peers full */
            fprintf( stderr, "all peers full...\n" );
            break;
        }

        /* find a peer who can ask for this block */
        for( j=0; j<size; )
        {
            const int val = tr_peerMsgsAddRequest( peers[j]->msgs, index, begin, length );
//fprintf( stderr, " block %"PRIu64", peer %"PRIu64, (uint64_t)i,  (uint64_t)j );
            if( val == TR_ADDREQ_FULL ) {
fprintf( stderr, "peer %d of %d is full\n", (int)j, size );
                peers[j] = peers[--size];
            }
            else if( val == TR_ADDREQ_MISSING ) {
//fprintf( stderr, "peer doesn't have it\n" );
                ++j;
            }
            else if( val == TR_ADDREQ_OK ) {
fprintf( stderr, "peer %d took the request for block %d\n", j, i );
                incrementReqCount( &t->blocks[i] );
                j = size;
            }
        }
    }

    /* put the blocks back by index */
    qsort( t->blocks, t->blockCount, sizeof(struct tr_block), compareBlockByIndex );

    /* cleanup */
    tr_free( peers );

    /* let the timer expire */
    t->refill_tag = NULL;
    return FALSE;
}

static void
ensureRefillTag( Torrent * t )
{
    if( t->refill_tag == NULL )
        t->refill_tag = tr_timerNew( t->manager->handle,
                                     refillPulse, t, NULL, 5000 );
}

static void
msgsCallbackFunc( void * source UNUSED, void * vevent, void * vt )
{
    Torrent * t = (Torrent *) vt;
    const tr_peermsgs_event * e = (const tr_peermsgs_event *) vevent;

    switch( e->eventType )
    {
        case TR_PEERMSG_GOT_BITFIELD: {
            const uint32_t begin = 0;
            const uint32_t end = begin + t->blockCount;
            uint32_t i;
            for( i=begin; i<end; ++i ) {
                if( !tr_bitfieldHas( e->bitfield, i ) )
                    continue;
                assert( t->blocks[i].block == i );
                incrementScarcity( &t->blocks[i] );
            }
            break;
        }

        case TR_PEERMSG_GOT_HAVE: {
            const uint32_t begin = tr_torPieceFirstBlock( t->tor, e->pieceIndex );
            const uint32_t end = begin + tr_torPieceCountBlocks( t->tor, (int)e->pieceIndex );
            uint32_t i;
            for( i=begin; i<end; ++i ) {
                assert( t->blocks[i].block == i );
                incrementScarcity( &t->blocks[i] );
            }
            break;
        }

        case TR_PEERMSG_GOT_BLOCK: {
            uint32_t i = e->blockIndex;
            assert( t->blocks[i].block == i );
            t->blocks[i].have = 1;
            break;
        }

        case TR_PEERMSG_GOT_PEX:
            /* FIXME */
            break;

        case TR_PEERMSG_GOT_ERROR:
            /* FIXME */
            break;

        case TR_PEERMSG_BLOCKS_RUNNING_LOW:
            ensureRefillTag( t );
            break;

        default:
            assert(0);
    }
}

static void
myHandshakeDoneCB( tr_peerIo * io, int isConnected, void * vmanager )
{
    int ok = isConnected;
    uint16_t port;
    const struct in_addr * in_addr;
    tr_peerMgr * manager = (tr_peerMgr*) vmanager;
    const uint8_t * hash = NULL;
    Torrent * t;

    assert( io != NULL );

    in_addr = tr_peerIoGetAddress( io, &port );

    if( !tr_peerIoHasTorrentHash( io ) ) /* incoming connection gone wrong? */
    {
        tr_peerIoFree( io );
        --manager->connectionCount;
        return;
    }

    hash = tr_peerIoGetTorrentHash( io );
    t = getExistingTorrent( manager, hash );
    if( t == NULL )
    {
        tr_peerIoFree( io );
        --manager->connectionCount;
        return;
    }

    fprintf( stderr, "peer-mgr: torrent [%s] finished a handshake; isConnected is %d\n", t->tor->info.name, isConnected );

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
        peer->io = io;
        peer->msgs = tr_peerMsgsNew( t->tor, peer );
        peer->msgsTag = tr_peerMsgsSubscribe( peer->msgs, msgsCallbackFunc, t );
        chokePulse( t );
    }
}

void
tr_peerMgrAddIncoming( tr_peerMgr      * manager,
                       struct in_addr  * addr,
                       int               socket )
{
    ++manager->connectionCount;

fprintf( stderr, "peer-mgr: new INCOMING CONNECTION...\n" );
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

        ++manager->connectionCount;

        peer->io = tr_peerIoNewOutgoing( manager->handle,
                                         &peer->in_addr,
                                         peer->port,
                                         t->hash );

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
tr_peerMgrUpdateCompletion( tr_peerMgr     * manager,
                            const uint8_t  * torrentHash )
{
    uint32_t i;
    Torrent * t = getExistingTorrent( manager, torrentHash );

    for( i=0; i<t->blockCount; ++i ) {
        assert( t->blocks[i].block == i );
        t->blocks[i].have = tr_cpBlockIsComplete( t->tor->completion, i ) ? 1 : 0;
    }
}

void
tr_peerMgrAddTorrent( tr_peerMgr * manager,
                      tr_torrent * tor )
{
    Torrent * t;
    uint32_t i;

    assert( tor != NULL );
    assert( getExistingTorrent( manager, tor->info.hash ) == NULL );

    t = tr_new0( Torrent, 1 );
    t->manager = manager;
    t->tor = tor;
    t->peers = tr_ptrArrayNew( );
    t->choke_tag = tr_timerNew( manager->handle,
                                chokePulse, t, NULL, 
                                RECHOKE_PERIOD_SECONDS );

    t->blockCount = tor->blockCount;
    t->blocks = tr_new( struct tr_block, t->blockCount );
    for( i=0; i<t->blockCount; ++i ) {
        const int index = tr_torBlockPiece( tor, i );
        t->blocks[i].have = tr_cpBlockIsComplete( t->tor->completion, i ) ? 1 : 0;
if( tr_cpBlockIsComplete( t->tor->completion, i ) ) fprintf( stderr, "have block %d\n", (int)i );
        t->blocks[i].dnd = tor->info.pieces[index].dnd;
        t->blocks[i].low_priority = tor->info.pieces[index].priority == TR_PRI_LOW;
        t->blocks[i].high_priority = tor->info.pieces[index].priority == TR_PRI_HIGH;
        t->blocks[i].requestCount = 0;
        t->blocks[i].scarcity = 0;
        t->blocks[i].block = i;
    }

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
    float bestDownloaderRate;
    ChokeData * data;
    tr_peer ** peers = getConnectedPeers( t, &size );

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
        tr_peerMsgsSetChoke( data[i].peer->msgs, FALSE );
        ++unchoked;
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
    tr_free( peers );
    return TRUE;
}
