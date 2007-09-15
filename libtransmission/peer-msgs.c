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
#include "trevent.h"
#include "utils.h"

/**
***
**/

#define PEX_INTERVAL (60 * 1000)

#define PEER_PULSE_INTERVAL (50)

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
    time_t time_requested;
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

    tr_timer * pulseTimer;
    tr_timer * pexTimer;

    unsigned int notListening    : 1;
    unsigned int peerSupportsPex : 1;

    struct peer_request blockToUs; /* the block currntly being sent to us */

    time_t gotKeepAliveTime;
    time_t clientSentPexAt;

    uint8_t state;

    uint8_t ut_pex_id;
    uint16_t listeningPort;

    uint16_t pexCount;

    uint32_t incomingMessageLength;

    tr_pex * pex;
};

/**
***  EVENTS
**/

static const tr_peermsgs_event blankEvent = { 0, 0, 0, 0, NULL };

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
    e.eventType = TR_PEERMSG_PEER_BITFIELD;
    e.bitfield = bitfield;
    tr_publisherPublish( peer->publisher, peer, &e );
}

static void
fireHave( tr_peermsgs * msgs, int isClient, uint32_t pieceIndex )
{
    tr_peermsgs_event e = blankEvent;
    e.eventType = isClient ? TR_PEERMSG_CLIENT_HAVE : TR_PEERMSG_PEER_HAVE;
    e.pieceIndex = pieceIndex;
    tr_publisherPublish( msgs->publisher, msgs, &e );
}

static void
fireClientHave( tr_peermsgs * msgs, uint32_t pieceIndex )
{
    fireHave( msgs, TRUE, pieceIndex );
}

static void
firePeerHave( tr_peermsgs * msgs, uint32_t pieceIndex )
{
    fireHave( msgs, FALSE, pieceIndex );
}

static void
fireGotBlock( tr_peermsgs * peer, uint32_t pieceIndex, uint32_t offset, uint32_t length )
{
    tr_peermsgs_event e = blankEvent;
    e.eventType = TR_PEERMSG_CLIENT_BLOCK;
    e.pieceIndex = pieceIndex;
    e.offset = offset;
    e.length = length;
    tr_publisherPublish( peer->publisher, peer, &e );
}

static void
fireGotError( tr_peermsgs * peer )
{
    publishEvent( peer, TR_PEERMSG_GOT_ERROR );
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
isPeerInteresting( const tr_peermsgs * msgs )
{
    const int clientIsSeed = tr_cpGetStatus( msgs->torrent->completion ) != TR_CP_INCOMPLETE;
    const int peerIsSeed = msgs->info->progress >= 1.0;

    if( peerIsSeed )
    {
        return !clientIsSeed;
    }
    else if( clientIsSeed )
    {
        return !peerIsSeed;
    }
    else /* we're both leeches... */
    {
        int i;
        const tr_torrent * torrent = msgs->torrent;
        const tr_bitfield * bitfield = tr_cpPieceBitfield( torrent->completion );

        if( !msgs->info->have ) /* We don't know what this peer has... what should this be? */
            return TRUE;

        assert( bitfield->len == msgs->info->have->len );
        for( i=0; i<torrent->info.pieceCount; ++i )
            if( isPieceInteresting( msgs, i ) )
                return TRUE;

        return FALSE;
    }
}

static void
sendInterest( tr_peermsgs * msgs, int weAreInterested )
{
    const uint32_t len = sizeof(uint8_t);
    const uint8_t bt_msgid = weAreInterested ? BT_INTERESTED : BT_NOT_INTERESTED;

    fprintf( stderr, "peer %p: sending an %s message\n", msgs, (weAreInterested ? "INTERESTED" : "NOT_INTERESTED") );
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, len );
    tr_peerIoWriteBytes( msgs->io, msgs->outMessages, &bt_msgid, 1 );
}

static void
updateInterest( tr_peermsgs * msgs )
{
    const int i = isPeerInteresting( msgs );
    if( i != msgs->info->clientIsInterested )
        sendInterest( msgs, i );
}

void
tr_peerMsgsSetChoke( tr_peermsgs * msgs, int choke )
{
    assert( msgs != NULL );
    assert( msgs->info != NULL );
    assert( choke==0 || choke==1 );

    if( msgs->info->peerIsChoked != choke )
    {
        const uint32_t len = sizeof(uint8_t);
        const uint8_t bt_msgid = choke ? BT_CHOKE : BT_UNCHOKE;

        msgs->info->peerIsChoked = choke ? 1 : 0;
        if( msgs->info )
        {
            tr_list_foreach( msgs->peerAskedFor, tr_free );
            tr_list_free( &msgs->peerAskedFor );
        }

        fprintf( stderr, "peer %p: sending a %s message\n", msgs, (choke ? "CHOKE" : "UNCHOKE") );
        tr_peerIoWriteUint32( msgs->io, msgs->outMessages, len );
        tr_peerIoWriteBytes( msgs->io, msgs->outMessages, &bt_msgid, 1 );
    }
}

/**
***
**/

void
tr_peerMsgsCancel( tr_peermsgs * msgs,
                   uint32_t      pieceIndex,
                   uint32_t      offset,
                   uint32_t      length )
{
    tr_list * node;
    struct peer_request tmp;

    assert( msgs != NULL );
    assert( length > 0 );

    tmp.index = pieceIndex;
    tmp.offset = offset;
    tmp.length = length;

    node = tr_list_find( msgs->clientAskedFor, &tmp, peer_request_compare );
    if( node != NULL )
    {
        /* cancel the request */
        const uint8_t bt_msgid = BT_CANCEL;
        const uint32_t len = sizeof(uint8_t) + 3 * sizeof(uint32_t);
        tr_peerIoWriteUint32( msgs->io, msgs->outMessages, len );
        tr_peerIoWriteBytes( msgs->io, msgs->outMessages, &bt_msgid, 1 );
        tr_peerIoWriteUint32( msgs->io, msgs->outMessages, pieceIndex );
        tr_peerIoWriteUint32( msgs->io, msgs->outMessages, offset );
        tr_peerIoWriteUint32( msgs->io, msgs->outMessages, length );

        /* remove it from our "requested" list */
        tr_list_remove_data( &msgs->peerAskedFor, node->data );
    }
}

/**
***
**/

void
tr_peerMsgsHave( tr_peermsgs * msgs,
                 uint32_t      pieceIndex )
{
    const uint8_t bt_msgid = BT_HAVE;
    const uint32_t len = sizeof(uint8_t) + sizeof(uint32_t);
    fprintf( stderr, "peer %p: telling them we HAVE piece #%d\n", msgs, pieceIndex );
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, len );
    tr_peerIoWriteBytes( msgs->io, msgs->outMessages, &bt_msgid, 1 );
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, pieceIndex );

    updateInterest( msgs );
}

/**
***
**/

int
tr_peerMsgsAddRequest( tr_peermsgs * msgs,
                       uint32_t      index, 
                       uint32_t      offset, 
                       uint32_t      length )
{
    const uint8_t bt_msgid = BT_REQUEST;
    const uint32_t len = sizeof(uint8_t) + 3 * sizeof(uint32_t);
    struct peer_request * req;
    int maxSize;

    if( msgs->info->clientIsChoked )
        return TR_ADDREQ_CLIENT_CHOKED;

    if( !tr_bitfieldHas( msgs->info->have, index ) )
        return TR_ADDREQ_MISSING;

    maxSize = MIN( 2 + (int)(tr_peerIoGetRateToClient(msgs->io)/10), 100 );
    if( tr_list_size( msgs->clientAskedFor) >= maxSize )
        return TR_ADDREQ_FULL;
    fprintf( stderr, "w00t peer %p has a max request queue size of %d\n", msgs, maxSize );

    /* queue the request */
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, len );
    tr_peerIoWriteBytes( msgs->io, msgs->outMessages, &bt_msgid, 1 );
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, index );
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, offset );
    tr_peerIoWriteUint32( msgs->io, msgs->outMessages, length );

    /* add it to our `requests sent' list */
    req = tr_new( struct peer_request, 1 );
    req->index = index;
    req->offset = offset;
    req->length = length;
    req->time_requested = time( NULL );
    tr_list_append( &msgs->clientAskedFor, req );

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
            peer->peerSupportsPex = 1;
            peer->ut_pex_id = (uint8_t) sub->val.i;
            fprintf( stderr, "peer->ut_pex is %d\n", (int)peer->ut_pex_id );
        }
    }

#if 0
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
#endif

    /* get peer's listening port */
    sub = tr_bencDictFind( &val, "p" );
    if( tr_bencIsInt( sub ) ) {
        peer->listeningPort = htons( (uint16_t)sub->val.i );
        fprintf( stderr, "peer->port is now %hu\n", peer->listeningPort );
    }

    tr_bencFree( &val );
    tr_free( tmp );
}

static void
parseUtPex( tr_peermsgs * msgs, int msglen, struct evbuffer * inbuf )
{
    benc_val_t val, * sub;
    uint8_t * tmp;

    if( msgs->torrent->pexDisabled ) /* no sharing! */
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
        tr_peerMgrAddPeers( msgs->handle->peerMgr,
                            msgs->torrent->info.hash,
                            TR_PEER_FROM_PEX,
                            (uint8_t*)sub->val.s.s, n );
    }

    fireGotPex( msgs );

    tr_bencFree( &val );
    tr_free( tmp );
}

static void
sendPex( tr_peermsgs * msgs );

static void
parseLtep( tr_peermsgs * msgs, int msglen, struct evbuffer * inbuf )
{
    uint8_t ltep_msgid;

    tr_peerIoReadBytes( msgs->io, inbuf, &ltep_msgid, 1 );
    msglen--;

    if( ltep_msgid == LTEP_HANDSHAKE )
    {
        fprintf( stderr, "got ltep handshake\n" );
        parseLtepHandshake( msgs, msglen, inbuf );
    }
    else if( ltep_msgid == msgs->ut_pex_id )
    {
        fprintf( stderr, "got ut pex\n" );
        msgs->peerSupportsPex = 1;
        parseUtPex( msgs, msglen, inbuf );
    }
    else
    {
        fprintf( stderr, "skipping unknown ltep message (%d)\n", (int)ltep_msgid );
        evbuffer_drain( inbuf, msglen );
    }
}

static int
readBtLength( tr_peermsgs * msgs, struct evbuffer * inbuf )
{
    uint32_t len;
    const size_t needlen = sizeof(uint32_t);

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    tr_peerIoReadUint32( msgs->io, inbuf, &len );

    if( len == 0 ) { /* peer sent us a keepalive message */
        fprintf( stderr, "peer %p sent us a keepalive message...\n", msgs );
        msgs->gotKeepAliveTime = time( NULL );
    } else {
        fprintf( stderr, "peer %p is sending us a message with %"PRIu64" bytes...\n", msgs, (uint64_t)len );
        msgs->incomingMessageLength = len;
        msgs->state = AWAITING_BT_MESSAGE;
    } return READ_AGAIN;
}

static int
readBtMessage( tr_peermsgs * msgs, struct evbuffer * inbuf )
{
    uint8_t id;
    uint32_t ui32;
    uint32_t msglen = msgs->incomingMessageLength;

    if( EVBUFFER_LENGTH(inbuf) < msglen )
        return READ_MORE;

    tr_peerIoReadBytes( msgs->io, inbuf, &id, 1 );
    msglen--;
    fprintf( stderr, "peer %p sent us a message... "
                     "bt id number is %d, and remaining len is %d\n", msgs, (int)id, (int)msglen );

    switch( id )
    {
        case BT_CHOKE:
            assert( msglen == 0 );
            fprintf( stderr, "peer-msgs %p sent us a BT_CHOKE\n", msgs );
            msgs->info->clientIsChoked = 1;
            tr_list_foreach( msgs->peerAskedFor, tr_free );
            tr_list_free( &msgs->peerAskedFor );
            /* FIXME: maybe choke them */
            /* FIXME: unmark anything we'd requested from them... */
            break;

        case BT_UNCHOKE:
            assert( msglen == 0 );
            fprintf( stderr, "peer-msgs %p sent us a BT_UNCHOKE\n", msgs );
            msgs->info->clientIsChoked = 0;
            /* FIXME: maybe unchoke them */
            /* FIXME: maybe send them requests */
            break;

        case BT_INTERESTED:
            assert( msglen == 0 );
            fprintf( stderr, "peer-msgs %p sent us a BT_INTERESTED\n", msgs );
            msgs->info->peerIsInterested = 1;
            /* FIXME: maybe unchoke them */
            break;

        case BT_NOT_INTERESTED:
            assert( msglen == 0 );
            fprintf( stderr, "peer-msgs %p sent us a BT_NOT_INTERESTED\n", msgs );
            msgs->info->peerIsInterested = 0;
            /* FIXME: maybe choke them */
            break;

        case BT_HAVE:
            assert( msglen == 4 );
            fprintf( stderr, "peer-msgs %p sent us a BT_HAVE\n", msgs );
            tr_peerIoReadUint32( msgs->io, inbuf, &ui32 );
            tr_bitfieldAdd( msgs->info->have, ui32 );
            msgs->info->progress = tr_bitfieldCountTrueBits( msgs->info->have ) / (float)msgs->torrent->info.pieceCount;
            fprintf( stderr, "after the HAVE message, peer progress is %f\n", msgs->info->progress );
            updateInterest( msgs );
            firePeerHave( msgs, ui32 );
            break;

        case BT_BITFIELD:
            assert( msglen == msgs->info->have->len );
            fprintf( stderr, "peer-msgs %p sent us a BT_BITFIELD\n", msgs );
            tr_peerIoReadBytes( msgs->io, inbuf, msgs->info->have->bits, msglen );
            msgs->info->progress = tr_bitfieldCountTrueBits( msgs->info->have ) / (float)msgs->torrent->info.pieceCount;
            fprintf( stderr, "after the BITFIELD peer progress is %f\n", msgs->info->progress );
            fireGotBitfield( msgs, msgs->info->have );
            updateInterest( msgs );
            /* FIXME: maybe unchoke */
            break;

        case BT_REQUEST: {
            struct peer_request * req;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_REQUEST\n" );
            req = tr_new( struct peer_request, 1 );
            tr_peerIoReadUint32( msgs->io, inbuf, &req->index );
            tr_peerIoReadUint32( msgs->io, inbuf, &req->offset );
            tr_peerIoReadUint32( msgs->io, inbuf, &req->length );
            if( !msgs->info->peerIsChoked )
                tr_list_append( &msgs->peerAskedFor, req );
            break;
        }

        case BT_CANCEL: {
            struct peer_request req;
            tr_list * node;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_CANCEL\n" );
            tr_peerIoReadUint32( msgs->io, inbuf, &req.index );
            tr_peerIoReadUint32( msgs->io, inbuf, &req.offset );
            tr_peerIoReadUint32( msgs->io, inbuf, &req.length );
            node = tr_list_find( msgs->peerAskedFor, &req, peer_request_compare );
            if( node != NULL ) {
                fprintf( stderr, "found the req that peer is cancelling... cancelled.\n" );
                tr_list_remove_data( &msgs->peerAskedFor, node->data );
            }
            break;
        }

        case BT_PIECE: {
            fprintf( stderr, "peer-msgs %p sent us a BT_PIECE\n", msgs );
            assert( msgs->blockToUs.length == 0 );
            msgs->state = READING_BT_PIECE;
            tr_peerIoReadUint32( msgs->io, inbuf, &msgs->blockToUs.index );
            tr_peerIoReadUint32( msgs->io, inbuf, &msgs->blockToUs.offset );
            msgs->blockToUs.length = msglen - 8;
            assert( msgs->blockToUs.length > 0 );
            assert( EVBUFFER_LENGTH(msgs->inBlock) == 0 );
            //evbuffer_drain( msgs->inBlock, ~0 );
            return READ_AGAIN;
            break;
        }

        case BT_PORT: {
            assert( msglen == 2 );
            fprintf( stderr, "peer-msgs %p sent us a BT_PORT\n", msgs );
            tr_peerIoReadUint16( msgs->io, inbuf, &msgs->listeningPort );
            break;
        }

        case BT_LTEP:
            fprintf( stderr, "peer-msgs %p sent us a BT_LTEP\n", msgs );
            parseLtep( msgs, msglen, inbuf );
            break;

        default:
            fprintf( stderr, "peer-msgs %p sent us an UNKNOWN: %d\n", msgs, (int)id );
            tr_peerIoDrain( msgs->io, inbuf, msglen );
            assert( 0 );
    }

    msgs->incomingMessageLength = -1;
    msgs->state = AWAITING_BT_LENGTH;
    return READ_AGAIN;
}

static void
clientGotBytes( tr_peermsgs * msgs, uint32_t byteCount )
{
    tr_torrent * tor = msgs->torrent;
    tor->downloadedCur += byteCount;
    tr_rcTransferred( tor->download, byteCount );
    tr_rcTransferred( tor->handle->download, byteCount );
}

static void
peerGotBytes( tr_peermsgs * msgs, uint32_t byteCount )
{
    tr_torrent * tor = msgs->torrent;
    tor->uploadedCur += byteCount;
    tr_rcTransferred( tor->upload, byteCount );
    tr_rcTransferred( tor->upload, byteCount );
}

static int
canDownload( const tr_peermsgs * msgs UNUSED )
{
#if 0
    tr_torrent * tor = msgs->torrent;

    if( tor->downloadLimitMode == TR_SPEEDLIMIT_GLOBAL )
        return !tor->handle->useDownloadLimit || tr_rcCanTransfer( tor->handle->download );

    if( tor->downloadLimitMode == TR_SPEEDLIMIT_SINGLE )
        return tr_rcCanTransfer( tor->download );
#endif

    return TRUE;
}

static void
reassignBytesToCorrupt( tr_peermsgs * msgs, uint32_t byteCount )
{
    tr_torrent * tor = msgs->torrent;

    /* increment the `corrupt' field */
    tor->corruptCur += byteCount;

    /* decrement the `downloaded' field */
    if( tor->downloadedCur >= byteCount )
        tor->downloadedCur -= byteCount;
    else
        tor->downloadedCur = 0;
}


static void
gotBadPiece( tr_peermsgs * msgs, uint32_t pieceIndex )
{
    const uint32_t byteCount = tr_torPieceCountBytes( msgs->torrent, (int)pieceIndex );
    reassignBytesToCorrupt( msgs, byteCount );
}

static void
gotUnwantedBlock( tr_peermsgs * msgs, uint32_t index UNUSED, uint32_t offset UNUSED, uint32_t length )
{
    reassignBytesToCorrupt( msgs, length );
}

static void
addUsToBlamefield( tr_peermsgs * msgs, uint32_t index )
{
    if( !msgs->info->blame )
         msgs->info->blame = tr_bitfieldNew( msgs->torrent->info.pieceCount );
    tr_bitfieldAdd( msgs->info->blame, index );
}

static void
gotBlock( tr_peermsgs      * msgs,
          struct evbuffer  * inbuf,
          uint32_t           index,
          uint32_t           offset,
          uint32_t           length )
{
    tr_torrent * tor = msgs->torrent;
    const int block = _tr_block( tor, index, offset );
    struct peer_request key, *req;

    /**
    *** Remove the block from our `we asked for this' list
    **/

    key.index = index;
    key.offset = offset;
    key.length = length;
    req = (struct peer_request*) tr_list_remove( &msgs->clientAskedFor, &key,
                                                 peer_request_compare );
    fprintf( stderr, "w00t got a block from %p. turnaround time for this block was %d seconds\n",
                     msgs, (int)(time(NULL) - req->time_requested) );
    if( req == NULL ) {
        gotUnwantedBlock( msgs, index, offset, length );
        fprintf( stderr, "we didn't ask for this message...\n" );
        tr_dbg( "we didn't ask the peer for this message..." );
        return;
    }
    tr_free( req );
    fprintf( stderr, "peer %p now has %d block requests in its outbox\n",
             msgs, tr_list_size(msgs->clientAskedFor));

    /**
    *** Error checks
    **/

    if( tr_cpBlockIsComplete( tor->completion, block ) ) {
        fprintf( stderr, "have this block already...\n" );
        tr_dbg( "have this block already..." );
        gotUnwantedBlock( msgs, index, offset, length );
        return;
    }

    if( (int)length != tr_torBlockCountBytes( tor, block ) ) {
        fprintf( stderr, "block is the wrong length..." );
        tr_dbg( "block is the wrong length..." );
        gotUnwantedBlock( msgs, index, offset, length );
        return;
    }

    /**
    ***  Write the block
    **/

    if( tr_ioWrite( tor, index, offset, length, EVBUFFER_DATA( inbuf ))) {
        return;
    }

    tr_cpBlockAdd( tor->completion, block );

    addUsToBlamefield( msgs, index );

    fireGotBlock( msgs, index, offset, length );

    /**
    ***  Handle if this was the last block in the piece
    **/

    if( tr_cpPieceIsComplete( tor->completion, index ) )
    {
        if( !tr_ioHash( tor, index ) )
        {
            gotBadPiece( msgs, index );
            return;
        }

        fireClientHave( msgs, index );
    }
}


static ReadState
readBtPiece( tr_peermsgs * msgs, struct evbuffer * inbuf )
{
    assert( msgs->blockToUs.length > 0 );

    if( !canDownload( msgs ) )
    {
        msgs->notListening = 1;
        tr_peerIoSetIOMode ( msgs->io, 0, EV_READ );
        return READ_DONE;
    }
    else
    {
        /* inbuf ->  inBlock */
        const uint32_t len = MIN( EVBUFFER_LENGTH(inbuf), msgs->blockToUs.length );
        uint8_t * tmp = tr_new( uint8_t, len );
        tr_peerIoReadBytes( msgs->io, inbuf, tmp, len );
        evbuffer_add( msgs->inBlock, tmp, len );
        tr_free( tmp );
        msgs->blockToUs.length -= len;
        clientGotBytes( msgs, len );
        msgs->info->peerSentDataAt = time( NULL );
fprintf( stderr, "got %"PRIu64"; left to read is [%"PRIu64"]\n", (uint64_t)len, (uint64_t)msgs->blockToUs.length );

        if( !msgs->blockToUs.length )
        {
fprintf( stderr, "w00t -- index %u, offset %u\n", msgs->blockToUs.index, msgs->blockToUs.offset );
            gotBlock( msgs, msgs->inBlock,
                            msgs->blockToUs.index,
                            msgs->blockToUs.offset,
                            EVBUFFER_LENGTH( msgs->inBlock ) );
            evbuffer_drain( msgs->inBlock, ~0 );
            msgs->state = AWAITING_BT_LENGTH;
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
pulse( void * vmsgs )
{
    tr_peermsgs * msgs = (tr_peermsgs *) vmsgs;
    size_t len;

    /* if we froze out a downloaded block because of speed limits,
       start listening to the peer again */
#if 0
    if( msgs->notListening )
    {
        fprintf( stderr, "msgs %p thawing out...\n", msgs );
        msgs->notListening = 0;
        tr_peerIoSetIOMode ( msgs->io, EV_READ, 0 );
    }
#endif

    if(( len = EVBUFFER_LENGTH( msgs->outBlock ) ))
    {
        while ( len && canUpload( msgs ) )
        {
            const size_t outlen = MIN( len, 1024 );
            tr_peerIoWrite( msgs->io, EVBUFFER_DATA(msgs->outBlock), outlen );
            evbuffer_drain( msgs->outBlock, outlen );
            peerGotBytes( msgs, outlen );
            len -= outlen;
        }
    }
    else if(( len = EVBUFFER_LENGTH( msgs->outMessages ) ))
    {
        tr_peerIoWriteBuf( msgs->io, msgs->outMessages );
    }
    else if(( msgs->peerAskedFor ))
    {
        struct peer_request * req = (struct peer_request*) msgs->peerAskedFor->data;
        uint8_t * tmp = tr_new( uint8_t, req->length );
        const uint8_t msgid = BT_PIECE;
        const uint32_t msglen = sizeof(uint8_t) + sizeof(uint32_t)*2 + req->length;
        tr_ioRead( msgs->torrent, req->index, req->offset, req->length, tmp );
        tr_peerIoWriteUint32( msgs->io, msgs->outBlock, msglen );
        tr_peerIoWriteBytes ( msgs->io, msgs->outBlock, &msgid, 1 );
        tr_peerIoWriteUint32( msgs->io, msgs->outBlock, req->index );
        tr_peerIoWriteUint32( msgs->io, msgs->outBlock, req->offset );
        tr_peerIoWriteBytes ( msgs->io, msgs->outBlock, tmp, req->length );
        tr_free( tmp );
    }

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

static void
pexAddedCb( void * vpex, void * userData )
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

static void
pexRemovedCb( void * vpex, void * userData )
{
    PexDiffs * diffs = (PexDiffs *) userData;
    tr_pex * pex = (tr_pex *) vpex;
    if( diffs->diffCount < MAX_DIFFS )
    {
        diffs->diffCount++;
        diffs->dropped[diffs->droppedCount++] = *pex;
    }
}

static void
pexElementCb( void * vpex, void * userData )
{
    PexDiffs * diffs = (PexDiffs *) userData;
    tr_pex * pex = (tr_pex *) vpex;
    if( diffs->diffCount < MAX_DIFFS )
    {
        diffs->diffCount++;
        diffs->elements[diffs->elementCount++] = *pex;
    }
}

static void
sendPex( tr_peermsgs * msgs )
{
    if( msgs->peerSupportsPex && !msgs->torrent->pexDisabled )
    {
        int i;
        tr_pex * newPex = NULL;
        const int newCount = tr_peerMgrGetPeers( msgs->handle->peerMgr, msgs->torrent->info.hash, &newPex );
        PexDiffs diffs;
        benc_val_t val, *added, *dropped, *flags;
        uint8_t *tmp, *walk;
        char * benc;
        int bencLen;
        const uint8_t bt_msgid = BT_LTEP;
        const uint8_t ltep_msgid = msgs->ut_pex_id;

        /* build the diffs */
        diffs.added = tr_new( tr_pex, newCount );
        diffs.addedCount = 0;
        diffs.dropped = tr_new( tr_pex, msgs->pexCount );
        diffs.droppedCount = 0;
        diffs.elements = tr_new( tr_pex, newCount + msgs->pexCount );
        diffs.elementCount = 0;
        diffs.diffCount = 0;
        tr_set_compare( msgs->pex, msgs->pexCount,
                        newPex, newCount,
                        tr_pexCompare, sizeof(tr_pex),
                        pexRemovedCb, pexAddedCb, pexElementCb, &diffs );
        fprintf( stderr, "pex: old peer count %d, new peer count %d, added %d, removed %d\n", msgs->pexCount, newCount, diffs.addedCount, diffs.droppedCount );

        /* update peer */
        tr_free( msgs->pex );
        msgs->pex = diffs.elements;
        msgs->pexCount = diffs.elementCount;

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
        for( i=0; i<diffs.addedCount; ++i ) {
            fprintf( stderr, "PEX -->> -->> flag is %d\n", (int)diffs.added[i].flags );
            *walk++ = diffs.added[i].flags;
        }
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
        tr_peerIoWriteUint32( msgs->io, msgs->outMessages, 1 + 1 + bencLen );
        tr_peerIoWriteBytes ( msgs->io, msgs->outMessages, &bt_msgid, 1 );
        tr_peerIoWriteBytes ( msgs->io, msgs->outMessages, &ltep_msgid, 1 );
        tr_peerIoWriteBytes ( msgs->io, msgs->outMessages, benc, bencLen );

        /* cleanup */
        tr_free( benc );
        tr_bencFree( &val );
        tr_free( diffs.added );
        tr_free( diffs.dropped );
        tr_free( newPex );

        msgs->clientSentPexAt = time( NULL );
    }
}

static int
pexPulse( void * vpeer )
{
    sendPex( vpeer );
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
    peer->pulseTimer = tr_timerNew( peer->handle, pulse, peer, PEER_PULSE_INTERVAL );
    peer->pexTimer = tr_timerNew( peer->handle, pexPulse, peer, PEX_INTERVAL );
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
        tr_timerFree( &p->pulseTimer );
        tr_timerFree( &p->pexTimer );
        tr_publisherFree( &p->publisher );
        tr_list_foreach( p->clientAskedFor, tr_free );
        tr_list_free( &p->clientAskedFor );
        tr_list_foreach( p->peerAskedFor, tr_free );
        tr_list_free( &p->peerAskedFor );
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
