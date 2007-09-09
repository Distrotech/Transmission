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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include <sys/types.h> /* event.h needs this */
#include <event.h>

#include "transmission.h"
#include "bencode.h"
#include "completion.h"
#include "inout.h"
#include "list.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-mgr-private.h"
#include "peer-msgs.h"
#include "ratecontrol.h"
#include "timer.h"
#include "utils.h"

/**
***
**/

#define MINUTES_TO_MSEC(N) ((N) * 60 * 1000)

/* pex attempts are made this frequently */
#define PEX_INTERVAL (MINUTES_TO_MSEC(1))

/* the most requests we'll batch up for this peer */
#define OUT_REQUESTS_MAX 6

/* when we get down to this many requests, we ask the manager for more */
#define OUT_REQUESTS_LOW 2

enum
{
    BT_CHOKE           = 0,
    BT_UNCHOKE         = 1,
    BT_INTERESTED      = 2,
    BT_NOT_INTERESTED  = 3,
    BT_HAVE            = 4,
    BT_BITFIELD        = 5,
    BT_REQUEST         = 6,
    BT_PIECE           = 7,
    BT_CANCEL          = 8,
    BT_PORT            = 9,
    BT_LTEP            = 20,

    LTEP_HANDSHAKE     = 0
};

enum
{
    AWAITING_BT_LENGTH,
    AWAITING_BT_MESSAGE,
    READING_BT_PIECE
};

struct peer_request
{
    uint32_t index;
    uint32_t offset;
    uint32_t length;
};

static int
peer_request_compare( const void * va, const void * vb )
{
    struct peer_request * a = (struct peer_request*) va;
    struct peer_request * b = (struct peer_request*) vb;
    if( a->index != b->index ) return a->index - b->index;
    if( a->offset != b->offset ) return a->offset - b->offset;
    if( a->length != b->length ) return a->length - b->length;
    return 0;
}

struct tr_peermsgs
{
    tr_peer * info;

    tr_handle * handle;
    tr_torrent * torrent;
    tr_peerIo * io;

    tr_publisher_t * publisher;

    struct evbuffer * outMessages; /* buffer of all the non-piece messages */
    struct evbuffer * outBlock;    /* the block we're currently sending */
    struct evbuffer * inBlock;     /* the block we're currently receiving */
    tr_list * peerAskedFor;
    tr_list * clientAskedFor;

    tr_timer_tag pulseTag;
    tr_timer_tag pexTag;

    unsigned int  notListening        : 1;

    struct peer_request blockToUs;

    int state;

    uint32_t incomingMessageLength;

    uint64_t gotKeepAliveTime;

    uint8_t ut_pex;
    uint16_t listeningPort;

    tr_pex * pex;
    int pexCount;
};

/**
***  EVENTS
**/

static const tr_peermsgs_event blankEvent = { 0, 0, 0, NULL };

static void
publishEvent( tr_peermsgs * peer, int eventType )
{
    tr_peermsgs_event e = blankEvent;
    e.eventType = eventType;
    tr_publisherPublish( peer->publisher, peer, &e );
}

static void
fireGotPex( tr_peermsgs * peer )
{
    publishEvent( peer, TR_PEERMSG_GOT_PEX );
}

static void
fireGotBitfield( tr_peermsgs * peer, const tr_bitfield * bitfield )
{
    tr_peermsgs_event e = blankEvent;
    e.eventType = TR_PEERMSG_GOT_BITFIELD;
    e.bitfield = bitfield;
    tr_publisherPublish( peer->publisher, peer, &e );
}

static void
fireGotHave( tr_peermsgs * peer, uint32_t pieceIndex )
{
    tr_peermsgs_event e = blankEvent;
    e.eventType = TR_PEERMSG_GOT_HAVE;
    e.pieceIndex = pieceIndex;
    tr_publisherPublish( peer->publisher, peer, &e );
}

static void
fireGotBlock( tr_peermsgs * peer, uint32_t blockIndex )
{
    tr_peermsgs_event e = blankEvent;
    e.eventType = TR_PEERMSG_GOT_BLOCK;
    e.pieceIndex = blockIndex;
    tr_publisherPublish( peer->publisher, peer, &e );
}

static void
fireGotError( tr_peermsgs * peer )
{
    publishEvent( peer, TR_PEERMSG_GOT_ERROR );
}

static void
fireBlocksRunningLow( tr_peermsgs * peer )
{
    publishEvent( peer, TR_PEERMSG_BLOCKS_RUNNING_LOW );
}

/**
***  INTEREST
**/

static int
isPieceInteresting( const tr_peermsgs   * peer,
                    int                   piece )
{
    const tr_torrent * torrent = peer->torrent;
    if( torrent->info.pieces[piece].dnd ) /* we don't want it */
        return FALSE;
    if( tr_cpPieceIsComplete( torrent->completion, piece ) ) /* we already have it */
        return FALSE;
    if( !tr_bitfieldHas( peer->info->have, piece ) ) /* peer doesn't have it */
        return FALSE;
    if( tr_bitfieldHas( peer->info->banned, piece ) ) /* peer is banned for it */
        return FALSE;
    return TRUE;
}

static int
isPeerInteresting( const tr_peermsgs * peer )
{
    int i;
    const tr_torrent * torrent = peer->torrent;
    const tr_bitfield * bitfield = tr_cpPieceBitfield( torrent->completion );

    if( !peer->info->have ) /* We don't know what this peer has */
        return FALSE;

    assert( bitfield->len == peer->info->have->len );

    for( i=0; i<torrent->info.pieceCount; ++i )
        if( isPieceInteresting( peer, i ) )
            return TRUE;

    return FALSE;
}

static void
sendInterest( tr_peermsgs * peer, int weAreInterested )
{
    const uint32_t len = sizeof(uint8_t);
    const uint8_t bt_msgid = weAreInterested ? BT_INTERESTED : BT_NOT_INTERESTED;

    fprintf( stderr, "peer %p: enqueueing an %s message\n", peer, (weAreInterested ? "interested" : "not interested") );
    tr_peerIoWriteUint32( peer->io, peer->outMessages, len );
    tr_peerIoWriteBytes( peer->io, peer->outMessages, &bt_msgid, 1 );
}

static void
updateInterest( tr_peermsgs * peer )
{
    const int i = isPeerInteresting( peer );
    if( i != peer->info->clientIsInterested )
        sendInterest( peer, i );
}

void
tr_peerMsgsSetChoke( tr_peermsgs * peer, int choke )
{
    assert( peer != NULL );
    assert( peer->info != NULL );

    if( peer->info->peerIsChoked != !!choke )
    {
        const uint32_t len = sizeof(uint8_t);
        const uint8_t bt_msgid = choke ? BT_CHOKE : BT_UNCHOKE;

        peer->info->peerIsChoked = choke ? 1 : 0;
        if( peer->info )
        {
            tr_list_foreach( peer->peerAskedFor, tr_free );
            tr_list_free( &peer->peerAskedFor );
        }

        fprintf( stderr, "peer %p: enqueuing a %s message\n", peer, (choke ? "choke" : "unchoke") );
        tr_peerIoWriteUint32( peer->io, peer->outMessages, len );
        tr_peerIoWriteBytes( peer->io, peer->outMessages, &bt_msgid, 1 );
    }
}

/**
***
**/

int
tr_peerMsgsAddRequest( tr_peermsgs * peer,
                       uint32_t      index, 
                       uint32_t      offset, 
                       uint32_t      length )
{
    const uint8_t bt_msgid = BT_REQUEST;
    const uint32_t len = sizeof(uint8_t) + 3 * sizeof(uint32_t);
    struct peer_request * req;

    if( tr_list_size(peer->clientAskedFor) >= OUT_REQUESTS_MAX )
        return TR_ADDREQ_FULL;

    if( !tr_bitfieldHas( peer->info->have, index ) )
        return TR_ADDREQ_MISSING;

    /* queue the request */
    tr_peerIoWriteUint32( peer->io, peer->outMessages, len );
    tr_peerIoWriteBytes( peer->io, peer->outMessages, &bt_msgid, 1 );
    tr_peerIoWriteUint32( peer->io, peer->outMessages, index );
    tr_peerIoWriteUint32( peer->io, peer->outMessages, offset );
    tr_peerIoWriteUint32( peer->io, peer->outMessages, length );
    fprintf( stderr, "peer %p: requesting a block from piece %u, offset %u, length %u\n",
             peer, (unsigned int)index, (unsigned int)offset, (unsigned int)length );

    /* add it to our `requests sent' list */
    req = tr_new( struct peer_request, 1 );
    req->index = index;
    req->offset = offset;
    req->length = length;
    tr_list_prepend( &peer->clientAskedFor, req );
    fprintf( stderr, "added a request; peer %p's clientAskedFor.size() is now %d\n", peer, tr_list_size(peer->clientAskedFor));

    return TR_ADDREQ_OK;
}

/**
***
**/

static void
parseLtepHandshake( tr_peermsgs * peer, int len, struct evbuffer * inbuf )
{
    benc_val_t val, * sub;
    uint8_t * tmp = tr_new( uint8_t, len );
    evbuffer_remove( inbuf, tmp, len );

    if( tr_bencLoad( tmp, len, &val, NULL ) || val.type!=TYPE_DICT ) {
        fprintf( stderr, "GET  extended-handshake, couldn't get dictionary\n" );
        tr_free( tmp );
        return;
    }

    tr_bencPrint( &val );

    /* check supported messages for utorrent pex */
    sub = tr_bencDictFind( &val, "m" );
    if( tr_bencIsDict( sub ) ) {
        sub = tr_bencDictFind( sub, "ut_pex" );
        if( tr_bencIsInt( sub ) ) {
            peer->ut_pex = (uint8_t) sub->val.i;
            fprintf( stderr, "peer->ut_pex is %d\n", peer->ut_pex );
        }
    }

    /* get peer's client name */
    sub = tr_bencDictFind( &val, "v" );
    if( tr_bencIsStr( sub ) ) {
int i;
        tr_free( peer->info->client );
        fprintf( stderr, "dictionary says client is [%s]\n", sub->val.s.s );
        peer->info->client = tr_strndup( sub->val.s.s, sub->val.s.i );
for( i=0; i<sub->val.s.i; ++i ) { fprintf( stderr, "[%c] (%d)\n", sub->val.s.s[i], (int)sub->val.s.s[i] );
                                  if( (int)peer->info->client[i]==-75 ) peer->info->client[i]='u'; }
        fprintf( stderr, "peer->client is now [%s]\n", peer->info->client );
    }

    /* get peer's listening port */
    sub = tr_bencDictFind( &val, "p" );
    if( tr_bencIsInt( sub ) ) {
        peer->listeningPort = htons( (uint16_t)sub->val.i );
        fprintf( stderr, "peer->port is now %hd\n", peer->listeningPort );
    }

    tr_bencFree( &val );
    tr_free( tmp );
}

static void
parseUtPex( tr_peermsgs * peer, int msglen, struct evbuffer * inbuf )
{
    benc_val_t val, * sub;
    uint8_t * tmp;

    if( !peer->info->pexEnabled ) /* no sharing! */
        return;

    tmp = tr_new( uint8_t, msglen );
    evbuffer_remove( inbuf, tmp, msglen );

    if( tr_bencLoad( tmp, msglen, &val, NULL ) || !tr_bencIsDict( &val ) ) {
        fprintf( stderr, "GET can't read extended-pex dictionary\n" );
        tr_free( tmp );
        return;
    }

    sub = tr_bencDictFind( &val, "added" );
    if( tr_bencIsStr(sub) && ((sub->val.s.i % 6) == 0)) {
        const int n = sub->val.s.i / 6 ;
        fprintf( stderr, "got %d peers from uT pex\n", n );
        tr_peerMgrAddPeers( peer->handle->peerMgr,
                            peer->torrent->info.hash,
                            TR_PEER_FROM_PEX,
                            (uint8_t*)sub->val.s.s, n );
    }

    fireGotPex( peer );

    tr_bencFree( &val );
    tr_free( tmp );
}

static void
parseLtep( tr_peermsgs * peer, int msglen, struct evbuffer * inbuf )
{
    uint8_t ltep_msgid;

    tr_peerIoReadBytes( peer->io, inbuf, &ltep_msgid, 1 );
    msglen--;

    if( ltep_msgid == LTEP_HANDSHAKE )
    {
        fprintf( stderr, "got ltep handshake\n" );
        parseLtepHandshake( peer, msglen, inbuf );
    }
    else if( ltep_msgid == peer->ut_pex )
    {
        fprintf( stderr, "got ut pex\n" );
        parseUtPex( peer, msglen, inbuf );
    }
    else
    {
        fprintf( stderr, "skipping unknown ltep message (%d)\n", (int)ltep_msgid );
        evbuffer_drain( inbuf, msglen );
    }
}

static int
readBtLength( tr_peermsgs * peer, struct evbuffer * inbuf )
{
    uint32_t len;
    const size_t needlen = sizeof(uint32_t);

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    tr_peerIoReadUint32( peer->io, inbuf, &len );

    if( len == 0 ) { /* peer sent us a keepalive message */
        fprintf( stderr, "peer sent us a keepalive message...\n" );
        peer->gotKeepAliveTime = tr_date( );
    } else {
        fprintf( stderr, "peer is sending us a message with %"PRIu64" bytes...\n", (uint64_t)len );
        peer->incomingMessageLength = len;
        fprintf( stderr, "peer is sending us a message with %"PRIu64" bytes...\n", (uint64_t)peer->incomingMessageLength );
        peer->state = AWAITING_BT_MESSAGE;
    } return READ_AGAIN;
}

static int
readBtMessage( tr_peermsgs * peer, struct evbuffer * inbuf )
{
    uint8_t id;
    uint32_t ui32;
    uint32_t msglen = peer->incomingMessageLength;

    if( EVBUFFER_LENGTH(inbuf) < msglen )
        return READ_MORE;

    tr_peerIoReadBytes( peer->io, inbuf, &id, 1 );
    msglen--;
    fprintf( stderr, "got a message from the peer... "
                     "bt id number is %d, and remaining len is %d\n", (int)id, (int)msglen );

    switch( id )
    {
        case BT_CHOKE:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_CHOKE\n" );
            peer->info->clientIsChoked = 1;
            tr_list_foreach( peer->peerAskedFor, tr_free );
            tr_list_free( &peer->peerAskedFor );
            /* FIXME: maybe choke them */
            /* FIXME: unmark anything we'd requested from them... */
            break;

        case BT_UNCHOKE:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_UNCHOKE\n" );
            peer->info->clientIsChoked = 0;
            /* FIXME: maybe unchoke them */
            /* FIXME: maybe send them requests */
            break;

        case BT_INTERESTED:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_INTERESTED\n" );
            peer->info->peerIsInterested = 1;
            /* FIXME: maybe unchoke them */
            break;

        case BT_NOT_INTERESTED:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_NOT_INTERESTED\n" );
            peer->info->peerIsInterested = 0;
            /* FIXME: maybe choke them */
            break;

        case BT_HAVE:
            assert( msglen == 4 );
            fprintf( stderr, "got a BT_HAVE\n" );
            tr_peerIoReadUint32( peer->io, inbuf, &ui32 );
            tr_bitfieldAdd( peer->info->have, ui32 );
            peer->info->progress = tr_bitfieldCountTrueBits( peer->info->have ) / (float)peer->torrent->info.pieceCount;
            fireGotHave( peer, ui32 );
            updateInterest( peer );
            break;

        case BT_BITFIELD:
            assert( msglen == peer->info->have->len );
            fprintf( stderr, "got a BT_BITFIELD\n" );
            tr_peerIoReadBytes( peer->io, inbuf, peer->info->have->bits, msglen );
            peer->info->progress = tr_bitfieldCountTrueBits( peer->info->have ) / (float)peer->torrent->info.pieceCount;
            fprintf( stderr, "peer progress is %f\n", peer->info->progress );
            fireGotBitfield( peer, peer->info->have );
            updateInterest( peer );
            /* FIXME: maybe unchoke */
            break;

        case BT_REQUEST: {
            struct peer_request * req;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_REQUEST\n" );
            req = tr_new( struct peer_request, 1 );
            tr_peerIoReadUint32( peer->io, inbuf, &req->index );
            tr_peerIoReadUint32( peer->io, inbuf, &req->offset );
            tr_peerIoReadUint32( peer->io, inbuf, &req->length );
            if( !peer->info->peerIsChoked )
                tr_list_prepend( &peer->peerAskedFor, req );
            break;
        }

        case BT_CANCEL: {
            struct peer_request req;
            tr_list * node;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_CANCEL\n" );
            tr_peerIoReadUint32( peer->io, inbuf, &req.index );
            tr_peerIoReadUint32( peer->io, inbuf, &req.offset );
            tr_peerIoReadUint32( peer->io, inbuf, &req.length );
            node = tr_list_find( peer->peerAskedFor, &req, peer_request_compare );
            if( node != NULL ) {
                fprintf( stderr, "found the req that peer is cancelling... cancelled.\n" );
                tr_list_remove_data( &peer->peerAskedFor, node->data );
            }
            break;
        }

        case BT_PIECE: {
            fprintf( stderr, "got a BT_PIECE\n" );
            assert( peer->blockToUs.length == 0 );
            peer->state = READING_BT_PIECE;
            tr_peerIoReadUint32( peer->io, inbuf, &peer->blockToUs.index );
            tr_peerIoReadUint32( peer->io, inbuf, &peer->blockToUs.offset );
            peer->blockToUs.length = msglen - 8;
fprintf( stderr, "left to read is [%"PRIu64"]\n", (uint64_t)peer->blockToUs.length );
            assert( peer->blockToUs.length > 0 );
            evbuffer_drain( peer->inBlock, ~0 );
            return READ_AGAIN;
            break;
        }

        case BT_PORT: {
            assert( msglen == 2 );
            fprintf( stderr, "got a BT_PORT\n" );
            tr_peerIoReadUint16( peer->io, inbuf, &peer->listeningPort );
            break;
        }

        case BT_LTEP:
            fprintf( stderr, "got a BT_LTEP\n" );
            parseLtep( peer, msglen, inbuf );
            break;

        default:
            fprintf( stderr, "got an unknown BT message type: %d\n", (int)id );
            tr_peerIoDrain( peer->io, inbuf, msglen );
            assert( 0 );
    }

    peer->incomingMessageLength = -1;
    peer->state = AWAITING_BT_LENGTH;
    return READ_AGAIN;
}

static int
canDownload( const tr_peermsgs * peer )
{
    tr_torrent * tor = peer->torrent;

#if 0
    /* FIXME: was swift worth it?  did anyone notice a difference? */
    if( SWIFT_ENABLED && !isSeeding && (peer->credit<0) )
        return FALSE;
#endif

    if( tor->downloadLimitMode == TR_SPEEDLIMIT_GLOBAL )
        return !tor->handle->useDownloadLimit || tr_rcCanTransfer( tor->handle->download );

    if( tor->downloadLimitMode == TR_SPEEDLIMIT_SINGLE )
        return tr_rcCanTransfer( tor->download );

    return TRUE;
}

static void
gotBlock( tr_peermsgs * peer, int index, int offset, struct evbuffer * inbuf )
{
    tr_torrent * tor = peer->torrent;
    const size_t length = EVBUFFER_LENGTH( inbuf );
    const int block = _tr_block( tor, index, offset );
    struct peer_request key, *req;

    /* sanity clause */
    if( tr_cpBlockIsComplete( tor->completion, block ) ) {
fprintf( stderr, "have this block already...\n" );
        tr_dbg( "have this block already..." );
        return;
    }
    if( (int)length != tr_torBlockCountBytes( tor, block ) ) {
fprintf( stderr, "block is the wrong length... expected %d and got %d\n", (int)length, (int)tr_torBlockCountBytes(tor,block) );
        tr_dbg( "block is the wrong length..." );
        return;
    }

    /* remove it from our `we asked for this' list */
    key.index = index;
    key.offset = offset;
    key.length = length;
    req = (struct peer_request*) tr_list_remove( &peer->clientAskedFor, &key,
                                                 peer_request_compare );
    if( req == NULL ) {
fprintf( stderr, "we didn't ask for this message...\n" );
        tr_dbg( "we didn't ask the peer for this message..." );
        return;
    }
    tr_free( req );
    fprintf( stderr, "peer %p now has %d block requests in its outbox\n", peer, tr_list_size(peer->clientAskedFor));

    {
        uint64_t block = index;
        block *= tor->info.pieceSize;
        block += offset;
        block /= tor->blockSize;
        fireGotBlock( peer, (uint32_t)block );
    }

    /* write to disk */
    if( tr_ioWrite( tor, index, offset, length, EVBUFFER_DATA( inbuf )))
        return;

    /* make a note that this peer helped us with this piece */
    if( !peer->info->blame )
         peer->info->blame = tr_bitfieldNew( tor->info.pieceCount );
    tr_bitfieldAdd( peer->info->blame, index );

    tr_cpBlockAdd( tor->completion, block );

    tor->downloadedCur += length;
    tr_rcTransferred( tor->download, length );
    tr_rcTransferred( tor->handle->download, length );
}


static ReadState
readBtPiece( tr_peermsgs * peer, struct evbuffer * inbuf )
{
    assert( peer->blockToUs.length > 0 );

    if( !canDownload( peer ) )
    {
        peer->notListening = 1;
        tr_peerIoSetIOMode ( peer->io, 0, EV_READ );
        return READ_DONE;
    }
    else
    {
        /* inbuf ->  inBlock */
        const uint32_t len = MIN( EVBUFFER_LENGTH(inbuf), peer->blockToUs.length );
        uint8_t * tmp = tr_new( uint8_t, len );
        tr_peerIoReadBytes( peer->io, inbuf, tmp, len );
        evbuffer_add( peer->inBlock, tmp, len );
        tr_free( tmp );
        peer->blockToUs.length -= len;
fprintf( stderr, "got %"PRIu64"; left to read is [%"PRIu64"]\n", (uint64_t)len, (uint64_t)peer->blockToUs.length );


        if( !peer->blockToUs.length )
        {
fprintf( stderr, "w00t\n" );
            gotBlock( peer, peer->blockToUs.index,
                            peer->blockToUs.offset,
                            peer->inBlock );
            evbuffer_drain( peer->outBlock, ~0 );
            peer->state = AWAITING_BT_LENGTH;
        }

        return READ_AGAIN;
    }
}

static ReadState
canRead( struct bufferevent * evin, void * vpeer )
{
    ReadState ret;
    tr_peermsgs * peer = (tr_peermsgs *) vpeer;
    struct evbuffer * inbuf = EVBUFFER_INPUT ( evin );

    switch( peer->state )
    {
        case AWAITING_BT_LENGTH:  ret = readBtLength  ( peer, inbuf ); break;
        case AWAITING_BT_MESSAGE: ret = readBtMessage ( peer, inbuf ); break;
        case READING_BT_PIECE:    ret = readBtPiece   ( peer, inbuf ); break;
        default: assert( 0 );
    }
    return ret;
}

/**
***
**/

static int
canUpload( const tr_peermsgs * peer )
{
    const tr_torrent * tor = peer->torrent;

    if( tor->uploadLimitMode == TR_SPEEDLIMIT_GLOBAL )
        return !tor->handle->useUploadLimit || tr_rcCanTransfer( tor->handle->upload );

    if( tor->uploadLimitMode == TR_SPEEDLIMIT_SINGLE )
        return tr_rcCanTransfer( tor->upload );

    return TRUE;
}

static int
pulse( void * vpeer )
{
    tr_peermsgs * peer = (tr_peermsgs *) vpeer;
    size_t len;

    /* if we froze out a downloaded block because of speed limits,
       start listening to the peer again */
    if( peer->notListening )
    {
        fprintf( stderr, "peer %p thawing out...\n", peer );
        peer->notListening = 0;
        tr_peerIoSetIOMode ( peer->io, EV_READ, 0 );
    }

    if(( len = EVBUFFER_LENGTH( peer->outBlock ) ))
    {
        if( canUpload( peer ) )
        {
            const size_t outlen = MIN( len, 4096 );
            tr_peerIoWrite( peer->io, EVBUFFER_DATA(peer->outBlock), outlen );
            evbuffer_drain( peer->outBlock, outlen );

            peer->torrent->uploadedCur += outlen;
            tr_rcTransferred( peer->torrent->upload, outlen );
            tr_rcTransferred( peer->handle->upload, outlen );
        }
    }
    else if(( len = EVBUFFER_LENGTH( peer->outMessages ) ))
    {
        fprintf( stderr, "peer %p pulse is writing %d bytes worth of messages...\n", peer, (int)len );
        tr_peerIoWriteBuf( peer->io, peer->outMessages );
        evbuffer_drain( peer->outMessages, ~0 );
    }
    else if(( peer->peerAskedFor ))
    {
        struct peer_request * req = (struct peer_request*) peer->peerAskedFor->data;
        uint8_t * tmp = tr_new( uint8_t, req->length );
        const uint8_t msgid = BT_PIECE;
        const uint32_t msglen = sizeof(uint8_t) + sizeof(uint32_t)*2 + req->length;
fprintf( stderr, "peer %p starting to upload a block...\n", peer );
        tr_ioRead( peer->torrent, req->index, req->offset, req->length, tmp );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, msglen );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, &msgid, 1 );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, req->index );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, req->offset );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, tmp, req->length );
        tr_free( tmp );
    }

    if( tr_list_size(peer->clientAskedFor) <= OUT_REQUESTS_LOW )
        fireBlocksRunningLow( peer );

    return TRUE; /* loop forever */
}

static void
didWrite( struct bufferevent * evin UNUSED, void * vpeer )
{
    pulse( (tr_peermsgs *) vpeer );
}

static void
gotError( struct bufferevent * evbuf UNUSED, short what UNUSED, void * vpeer )
{
    fireGotError( (tr_peermsgs*)vpeer );
}

static void
sendBitfield( tr_peermsgs * peer )
{
    const tr_bitfield * bitfield = tr_cpPieceBitfield( peer->torrent->completion );
    const uint32_t len = sizeof(uint8_t) + bitfield->len;
    const uint8_t bt_msgid = BT_BITFIELD;

    fprintf( stderr, "peer %p: enqueueing a bitfield message\n", peer );
    tr_peerIoWriteUint32( peer->io, peer->outMessages, len );
    tr_peerIoWriteBytes( peer->io, peer->outMessages, &bt_msgid, 1 );
    tr_peerIoWriteBytes( peer->io, peer->outMessages, bitfield->bits, bitfield->len );
}

/**
***
**/

#define MAX_DIFFS 50

typedef struct
{
    tr_pex * added;
    tr_pex * dropped;
    tr_pex * elements;
    int addedCount;
    int droppedCount;
    int elementCount;
    int diffCount;
}
PexDiffs;

static void pexAddedCb( void * vpex, void * userData )
{
    PexDiffs * diffs = (PexDiffs *) userData;
    tr_pex * pex = (tr_pex *) vpex;
    if( diffs->diffCount < MAX_DIFFS )
    {
        diffs->diffCount++;
        diffs->added[diffs->addedCount++] = *pex;
        diffs->elements[diffs->elementCount++] = *pex;
    }
}

static void pexRemovedCb( void * vpex, void * userData )
{
    PexDiffs * diffs = (PexDiffs *) userData;
    tr_pex * pex = (tr_pex *) vpex;
    if( diffs->diffCount < MAX_DIFFS )
    {
        diffs->diffCount++;
        diffs->dropped[diffs->droppedCount++] = *pex;
    }
}

static void pexElementCb( void * vpex, void * userData )
{
    PexDiffs * diffs = (PexDiffs *) userData;
    tr_pex * pex = (tr_pex *) vpex;
    if( diffs->diffCount < MAX_DIFFS )
    {
        diffs->diffCount++;
        diffs->elements[diffs->elementCount++] = *pex;
    }
}

static int
pexPulse( void * vpeer )
{
    tr_peermsgs * peer = (tr_peermsgs *) vpeer;

    if( peer->info->pexEnabled )
    {
        int i;
        tr_pex * newPex = NULL;
        const int newCount = tr_peerMgrGetPeers( peer->handle->peerMgr, peer->torrent->info.hash, &newPex );
        PexDiffs diffs;
        benc_val_t val, *added, *dropped, *flags;
        uint8_t *tmp, *walk;
        char * benc;
        int bencLen;
        const uint8_t bt_msgid = BT_LTEP;
        const uint8_t ltep_msgid = peer->ut_pex;

        /* build the diffs */
        diffs.added = tr_new( tr_pex, newCount );
        diffs.addedCount = 0;
        diffs.dropped = tr_new( tr_pex, peer->pexCount );
        diffs.droppedCount = 0;
        diffs.elements = tr_new( tr_pex, newCount + peer->pexCount );
        diffs.elementCount = 0;
        diffs.diffCount = 0;
        tr_set_compare( peer->pex, peer->pexCount,
                        newPex, newCount,
                        tr_pexCompare, sizeof(tr_pex),
                        pexRemovedCb, pexAddedCb, pexElementCb, &diffs );
        fprintf( stderr, "pex: old peer count %d, new peer count %d, added %d, removed %d\n", peer->pexCount, newCount, diffs.addedCount, diffs.droppedCount );

        /* update peer */
        tr_free( peer->pex );
        peer->pex = diffs.elements;
        peer->pexCount = diffs.elementCount;

       
        /* build the pex payload */
        tr_bencInit( &val, TYPE_DICT );
        tr_bencDictReserve( &val, 3 );

        /* "added" */
        added = tr_bencDictAdd( &val, "added" );
        tmp = walk = tr_new( uint8_t, diffs.addedCount * 6 );
        for( i=0; i<diffs.addedCount; ++i ) {
            memcpy( walk, &diffs.added[i].in_addr, 4 ); walk += 4;
            memcpy( walk, &diffs.added[i].port, 2 ); walk += 2;
        }
        assert( ( walk - tmp ) == diffs.addedCount * 6 );
        tr_bencInitStr( added, tmp, walk-tmp, FALSE );

        /* "added.f" */
        flags = tr_bencDictAdd( &val, "added.f" );
        tmp = walk = tr_new( uint8_t, diffs.addedCount );
        for( i=0; i<diffs.addedCount; ++i )
            *walk++ = diffs.added[i].flags;
        assert( ( walk - tmp ) == diffs.addedCount );
        tr_bencInitStr( flags, tmp, walk-tmp, FALSE );

        /* "dropped" */
        dropped = tr_bencDictAdd( &val, "dropped" );
        tmp = walk = tr_new( uint8_t, diffs.droppedCount * 6 );
        for( i=0; i<diffs.droppedCount; ++i ) {
            memcpy( walk, &diffs.dropped[i].in_addr, 4 ); walk += 4;
            memcpy( walk, &diffs.dropped[i].port, 2 ); walk += 2;
        }
        assert( ( walk - tmp ) == diffs.droppedCount * 6 );
        tr_bencInitStr( dropped, tmp, walk-tmp, FALSE );

        /* write the pex message */
        benc = tr_bencSaveMalloc( &val, &bencLen );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, 1 + 1 + bencLen );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, &bt_msgid, 1 );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, &ltep_msgid, 1 );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, benc, bencLen );

        /* cleanup */
        tr_free( benc );
        tr_bencFree( &val );
        tr_free( diffs.added );
        tr_free( diffs.dropped );
        tr_free( newPex );
    }

    return TRUE;
}

/**
***
**/

tr_peermsgs*
tr_peerMsgsNew( struct tr_torrent * torrent, struct tr_peer * info )
{
    tr_peermsgs * peer;

    assert( info != NULL );
    assert( info->io != NULL );

    peer = tr_new0( tr_peermsgs, 1 );
    peer->publisher = tr_publisherNew( );
    peer->info = info;
    peer->handle = torrent->handle;
    peer->torrent = torrent;
    peer->io = info->io;
    peer->info->clientIsChoked = 1;
    peer->info->peerIsChoked = 1;
    peer->info->clientIsInterested = 0;
    peer->info->peerIsInterested = 0;
    peer->info->have = tr_bitfieldNew( torrent->info.pieceCount );
    peer->pulseTag = tr_timerNew( peer->handle, pulse, peer, NULL, 500 );
fprintf( stderr, "peer %p pulseTag %p\n", peer, peer->pulseTag );
    peer->pexTag = tr_timerNew( peer->handle, pexPulse, peer, NULL, PEX_INTERVAL );
    peer->outMessages = evbuffer_new( );
    peer->outBlock = evbuffer_new( );
    peer->inBlock = evbuffer_new( );

    tr_peerIoSetIOFuncs( peer->io, canRead, didWrite, gotError, peer );
    tr_peerIoSetIOMode( peer->io, EV_READ|EV_WRITE, 0 );

    sendBitfield( peer );

    return peer;
}

void
tr_peerMsgsFree( tr_peermsgs* p )
{
    if( p != NULL )
    {
fprintf( stderr, "peer %p destroying its pulse tag\n", p );
        tr_publisherFree( &p->publisher );
        tr_timerFree( &p->pulseTag );
        tr_timerFree( &p->pexTag );
        evbuffer_free( p->outMessages );
        evbuffer_free( p->outBlock );
        evbuffer_free( p->inBlock );
        tr_free( p );
    }
}

tr_publisher_tag
tr_peerMsgsSubscribe( tr_peermsgs       * peer,
                      tr_delivery_func    func,
                      void              * userData )
{
    return tr_publisherSubscribe( peer->publisher, func, userData );
}

void
tr_peerMsgsUnsubscribe( tr_peermsgs       * peer,
                        tr_publisher_tag    tag )
{
    tr_publisherUnsubscribe( peer->publisher, tag );
}
