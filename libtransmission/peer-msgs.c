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
    BT_LTEP            = 20
};

enum
{
    LTEP_HANDSHAKE     = 0,
    LTEP_PEX           = 1
};

enum
{
    AWAITING_BT_LENGTH,
    AWAITING_BT_MESSAGE,
    READING_BT_PIECE
};

static const char *
getStateName( int state )
{
    switch( state )
    {
        case AWAITING_BT_LENGTH: return "awaiting bt length";
        case AWAITING_BT_MESSAGE: return "awaiting bt message";
    }

    fprintf (stderr, "PeerManager::getStateName: unhandled state %d\n", state );
    abort( );
}

struct peer_request
{
    uint32_t pieceIndex;
    uint32_t offsetInPiece;
    uint32_t length;
};

static int
peer_request_compare_func( const void * va, const void * vb )
{
    struct peer_request * a = (struct peer_request*) va;
    struct peer_request * b = (struct peer_request*) vb;
    if( a->pieceIndex != b->pieceIndex )
        return a->pieceIndex - b->pieceIndex;
    if( a->offsetInPiece != b->offsetInPiece )
        return a->offsetInPiece - b->offsetInPiece;
    if( a->length != b->length )
        return a->length - b->length;
    return 0;
}

struct tr_peermsgs
{
    tr_peer * info;

    tr_handle * handle;
    tr_torrent * torrent;
    tr_peerIo * io;

    struct evbuffer * outMessages; /* buffer of all the non-piece messages */
    struct evbuffer * outBlock;    /* the block we're currently sending */
    struct evbuffer * inBlock;     /* the block we're currently receiving */
    tr_list * peerAskedFor;
    tr_list * outPieces;

    tr_timer_tag pulseTag;

    unsigned int  notListening        : 1;

    struct peer_request blockToUs;

    int state;

    uint32_t incomingMessageLength;

    uint64_t gotKeepAliveTime;

    uint16_t ut_pex;
    uint16_t listeningPort;
};

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
isInteresting( const tr_peermsgs * peer )
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
    const int i = isInteresting( peer );
    if( i != peer->info->clientIsInterested )
        sendInterest( peer, i );
}

void
tr_peerMsgsSetChoke( tr_peermsgs * peer, int choke )
{
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
            peer->ut_pex = sub->val.i;
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
        fprintf( stderr, "peer is sending us a message with %d bytes...\n", (int)len );
        peer->incomingMessageLength = len;
        peer->state = AWAITING_BT_MESSAGE;
    } return READ_AGAIN;
}

static int
readBtMessage( tr_peermsgs * peer, struct evbuffer * inbuf )
{
    uint8_t id;
    uint32_t ui32;
    size_t msglen = peer->incomingMessageLength;

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
            updateInterest( peer );
            break;

        case BT_BITFIELD:
            assert( msglen == peer->info->have->len );
            fprintf( stderr, "got a BT_BITFIELD\n" );
            tr_peerIoReadBytes( peer->io, inbuf, peer->info->have->bits, msglen );
            peer->info->progress = tr_bitfieldCountTrueBits( peer->info->have ) / (float)peer->torrent->info.pieceCount;
            fprintf( stderr, "peer progress is %f\n", peer->info->progress );
            updateInterest( peer );
            /* FIXME: maybe unchoke */
            break;

        case BT_REQUEST: {
            struct peer_request * req;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_REQUEST\n" );
            req = tr_new( struct peer_request, 1 );
            tr_peerIoReadUint32( peer->io, inbuf, &req->pieceIndex );
            tr_peerIoReadUint32( peer->io, inbuf, &req->offsetInPiece );
            tr_peerIoReadUint32( peer->io, inbuf, &req->length );
            if( !peer->info->peerIsChoked )
                tr_list_append( &peer->peerAskedFor, req );
            break;
        }

        case BT_CANCEL: {
            struct peer_request req;
            tr_list * node;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_CANCEL\n" );
            tr_peerIoReadUint32( peer->io, inbuf, &req.pieceIndex );
            tr_peerIoReadUint32( peer->io, inbuf, &req.offsetInPiece );
            tr_peerIoReadUint32( peer->io, inbuf, &req.length );
            node = tr_list_find( peer->peerAskedFor, &req, peer_request_compare_func );
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
            tr_peerIoReadUint32( peer->io, inbuf, &peer->blockToUs.pieceIndex );
            tr_peerIoReadUint32( peer->io, inbuf, &peer->blockToUs.offsetInPiece );
            peer->blockToUs.length = msglen - 8;
            assert( peer->blockToUs.length > 0 );
            evbuffer_drain( peer->inBlock, ~0 );
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
gotBlock( tr_peermsgs * peer, int pieceIndex, int offset, struct evbuffer * inbuf )
{
    tr_torrent * tor = peer->torrent;
    const size_t len = EVBUFFER_LENGTH( inbuf );
    const int block = _tr_block( tor, pieceIndex, offset );

    /* sanity clause */
    if( tr_cpBlockIsComplete( tor->completion, block ) ) {
        tr_dbg( "have this block already..." );
        return;
    }
    if( (int)len != tr_torBlockCountBytes( tor, block ) ) {
        tr_dbg( "block is the wrong length..." );
        return;
    }

    /* write to disk */
    if( tr_ioWrite( tor, pieceIndex, offset, len, EVBUFFER_DATA( inbuf )))
        return;

    /* make a note that this peer helped us with this piece */
    if( !peer->info->blame )
         peer->info->blame = tr_bitfieldNew( tor->info.pieceCount );
    tr_bitfieldAdd( peer->info->blame, pieceIndex );

    tr_cpBlockAdd( tor->completion, block );

    tor->downloadedCur += len;
    tr_rcTransferred( tor->download, len );
    tr_rcTransferred( tor->handle->download, len );

//    broadcastCancel( tor, index, begin, len - 8 );
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

        if( !peer->blockToUs.length )
        {
            gotBlock( peer, peer->blockToUs.pieceIndex,
                            peer->blockToUs.offsetInPiece,
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
    fprintf( stderr, "peer %p got a canRead; state is [%s]\n", peer, getStateName(peer->state) );

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
            const size_t outlen = MIN( len, 2048 );
            tr_peerIoWrite( peer->io, EVBUFFER_DATA(peer->outBlock), outlen );
            evbuffer_drain( peer->outBlock, outlen );
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
        tr_ioRead( peer->torrent, req->pieceIndex, req->offsetInPiece, req->length, tmp );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, msglen );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, &msgid, 1 );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, req->pieceIndex );
        tr_peerIoWriteUint32( peer->io, peer->outBlock, req->offsetInPiece );
        tr_peerIoWriteBytes ( peer->io, peer->outBlock, tmp, req->length );
        tr_free( tmp );
    }

    return TRUE; /* loop forever */
}

static void
didWrite( struct bufferevent * evin UNUSED, void * vpeer )
{
    tr_peermsgs * peer = (tr_peermsgs *) vpeer;
    fprintf( stderr, "peer %p got a didWrite...\n", peer );
    pulse( vpeer );
}

static void
gotError( struct bufferevent * evbuf UNUSED, short what, void * vpeer )
{
    tr_peermsgs * peer = (tr_peermsgs *) vpeer;
    fprintf( stderr, "peer %p got an error in %d\n", peer, (int)what );
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

tr_peermsgs*
tr_peerMsgsNew( struct tr_torrent * torrent, struct tr_peer * info )
{
    tr_peermsgs * peer;

    assert( info != NULL );
    assert( info->io != NULL );

    peer = tr_new0( tr_peermsgs, 1 );
    peer->info = info;
    peer->handle = torrent->handle;
    peer->torrent = torrent;
    peer->io = info->io;
    peer->info->clientIsChoked = 1;
    peer->info->peerIsChoked = 1;
    peer->info->clientIsInterested = 0;
    peer->info->peerIsInterested = 0;
    peer->info->have = tr_bitfieldNew( torrent->info.pieceCount );
    peer->pulseTag = tr_timerNew( peer->handle, pulse, peer, NULL, 200 );
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
        tr_timerFree( &p->pulseTag );
        evbuffer_free( p->outMessages );
        evbuffer_free( p->outBlock );
        evbuffer_free( p->inBlock );
        tr_free( p );
    }
}
