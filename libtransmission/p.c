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
#include "peer-connection.h"
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

typedef struct
{
    tr_handle * handle;
    tr_torrent * torrent;
    tr_peerConnection * connection;
    tr_bitfield * bitfield;   /* the peer's bitfield */
    tr_bitfield * banfield;   /* bad pieces from the peer */
    tr_bitfield * blamefield; /* lists pieces that this peer helped us with */

    struct evbuffer * outMessages; /* buffer of all the non-piece messages */
    struct evbuffer * outBlock;    /* the block we're currently sending */
    struct evbuffer * inBlock;     /* the block we're currently receiving */
    tr_list * peerAskedFor;
    tr_list * outPieces;

    tr_timer_tag pulseTag;

    tr_ratecontrol * rcToUs;   /* rate of bytes from the peer to us */
    tr_ratecontrol * rcToPeer; /* rate of bytes from us to the peer */

    unsigned int  peerIsChoked        : 1;
    unsigned int  weAreChoked         : 1;
    unsigned int  peerIsInterested    : 1;
    unsigned int  weAreInterested     : 1;
    unsigned int  isPrivate           : 1;
    unsigned int  notListening        : 1;

    struct peer_request blockToUs;

    int state;

    uint32_t incomingMessageLength;

    uint64_t gotKeepAliveTime;

    float progress;

    uint16_t ut_pex;
    uint16_t listeningPort;

    /* the client name from the `v' string in LTEP's handshake dictionary */
    char * client;
}
tr_peer;

/**
***  INTEREST
**/

static int
isPieceInteresting( const tr_peer   * peer,
                    int               piece )
{
    const tr_torrent * torrent = peer->torrent;
    if( torrent->info.pieces[piece].dnd ) /* we don't want it */
        return FALSE;
    if( tr_cpPieceIsComplete( torrent->completion, piece ) ) /* we already have it */
        return FALSE;
    if( !tr_bitfieldHas( peer->bitfield, piece ) ) /* peer doesn't have it */
        return FALSE;
    if( tr_bitfieldHas( peer->banfield, piece ) ) /* peer is banned for it */
        return FALSE;
    return TRUE;
}

static int
isInteresting( const tr_peer * peer )
{
    int i;
    const tr_torrent * torrent = peer->torrent;
    const tr_bitfield * bitfield = tr_cpPieceBitfield( torrent->completion );

    if( !peer->bitfield ) /* We don't know what this peer has */
        return FALSE;

    assert( bitfield->len == peer->bitfield->len );

    for( i=0; i<torrent->info.pieceCount; ++i )
        if( isPieceInteresting( peer, i ) )
            return TRUE;

    return FALSE;
}

static void
sendInterest( tr_peer * peer, int weAreInterested )
{
    const uint32_t len = sizeof(uint8_t);
    const uint8_t bt_msgid = weAreInterested ? BT_INTERESTED : BT_NOT_INTERESTED;

    fprintf( stderr, "peer %p: enqueueing an %s message\n", peer, (weAreInterested ? "interested" : "not interested") );
    tr_peerConnectionWriteUint32( peer->connection, peer->outMessages, len );
    tr_peerConnectionWriteBytes( peer->connection, peer->outMessages, &bt_msgid, 1 );
}

static void
updateInterest( tr_peer * peer )
{
    const int i = isInteresting( peer );
    if( i != peer->weAreInterested )
        sendInterest( peer, i );
}

void
setChoke( tr_peer * peer, int choke )
{
    if( peer->peerIsChoked != !!choke )
    {
        const uint32_t len = sizeof(uint8_t);
        const uint8_t bt_msgid = choke ? BT_CHOKE : BT_UNCHOKE;

        peer->peerIsChoked = choke ? 1 : 0;
        if( peer->peerIsChoked )
        {
            tr_list_foreach( peer->peerAskedFor, tr_free );
            tr_list_free( &peer->peerAskedFor );
        }

        fprintf( stderr, "peer %p: enqueuing a %s message\n", peer, (choke ? "choke" : "unchoke") );
        tr_peerConnectionWriteUint32( peer->connection, peer->outMessages, len );
        tr_peerConnectionWriteBytes( peer->connection, peer->outMessages, &bt_msgid, 1 );
    }
}

/**
***
**/

static void
parseLtepHandshake( tr_peer * peer, int len, struct evbuffer * inbuf )
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
        tr_free( peer->client );
        fprintf( stderr, "dictionary says client is [%s]\n", sub->val.s.s );
for( i=0; i<sub->val.s.i; ++i ) fprintf( stderr, "[%c] (%d)\n", sub->val.s.s[i], (int)sub->val.s.s[i] );
        peer->client = tr_strndup( sub->val.s.s, sub->val.s.i );
        fprintf( stderr, "peer->client is now [%s]\n", peer->client );
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
parseUtPex( tr_peer * peer, int msglen, struct evbuffer * inbuf )
{
    benc_val_t val, * sub;
    uint8_t * tmp;

    if( peer->isPrivate ) /* no sharing! */
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
        tr_torrentAddCompact( peer->torrent, TR_PEER_FROM_PEX, (uint8_t*)sub->val.s.s, n );
    }

    tr_bencFree( &val );
    tr_free( tmp );
}

static void
parseLtep( tr_peer * peer, int msglen, struct evbuffer * inbuf )
{
    uint8_t ltep_msgid;

    tr_peerConnectionReadBytes( peer->connection, inbuf, &ltep_msgid, 1 );
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
readBtLength( tr_peer * peer, struct evbuffer * inbuf )
{
    uint32_t len;
    const size_t needlen = sizeof(uint32_t);

    if( EVBUFFER_LENGTH(inbuf) < needlen )
        return READ_MORE;

    tr_peerConnectionReadUint32( peer->connection, inbuf, &len );

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
readBtMessage( tr_peer * peer, struct evbuffer * inbuf )
{
    uint8_t id;
    uint32_t ui32;
    size_t msglen = peer->incomingMessageLength;

    if( EVBUFFER_LENGTH(inbuf) < msglen )
        return READ_MORE;

    tr_peerConnectionReadBytes( peer->connection, inbuf, &id, 1 );
    msglen--;
    fprintf( stderr, "got a message from the peer... "
                     "bt id number is %d, and remaining len is %d\n", (int)id, (int)msglen );

    switch( id )
    {
        case BT_CHOKE:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_CHOKE\n" );
            peer->weAreChoked = 1;
            tr_list_foreach( peer->peerAskedFor, tr_free );
            tr_list_free( &peer->peerAskedFor );
            /* FIXME: maybe choke them */
            /* FIXME: unmark anything we'd requested from them... */
            break;

        case BT_UNCHOKE:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_UNCHOKE\n" );
            peer->weAreChoked = 0;
            /* FIXME: maybe unchoke them */
            /* FIXME: maybe send them requests */
            break;

        case BT_INTERESTED:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_INTERESTED\n" );
            peer->peerIsInterested = 1;
            /* FIXME: maybe unchoke them */
            break;

        case BT_NOT_INTERESTED:
            assert( msglen == 0 );
            fprintf( stderr, "got a BT_NOT_INTERESTED\n" );
            peer->peerIsInterested = 0;
            /* FIXME: maybe choke them */
            break;

        case BT_HAVE:
            assert( msglen == 4 );
            fprintf( stderr, "got a BT_HAVE\n" );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &ui32 );
            tr_bitfieldAdd( peer->bitfield, ui32 );
            peer->progress = tr_bitfieldCountTrueBits( peer->bitfield ) / (float)peer->torrent->info.pieceCount;
            updateInterest( peer );
            break;

        case BT_BITFIELD:
            assert( msglen == peer->bitfield->len );
            fprintf( stderr, "got a BT_BITFIELD\n" );
            tr_peerConnectionReadBytes( peer->connection, inbuf, peer->bitfield->bits, msglen );
            peer->progress = tr_bitfieldCountTrueBits( peer->bitfield ) / (float)peer->torrent->info.pieceCount;
            fprintf( stderr, "peer progress is %f\n", peer->progress );
            updateInterest( peer );
            /* FIXME: maybe unchoke */
            break;

        case BT_REQUEST: {
            struct peer_request * req;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_REQUEST\n" );
            req = tr_new( struct peer_request, 1 );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &req->pieceIndex );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &req->offsetInPiece );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &req->length );
            if( !peer->peerIsChoked )
                tr_list_append( &peer->peerAskedFor, req );
            break;
        }

        case BT_CANCEL: {
            struct peer_request req;
            tr_list * node;
            assert( msglen == 12 );
            fprintf( stderr, "got a BT_CANCEL\n" );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &req.pieceIndex );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &req.offsetInPiece );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &req.length );
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
            tr_peerConnectionReadUint32( peer->connection, inbuf, &peer->blockToUs.pieceIndex );
            tr_peerConnectionReadUint32( peer->connection, inbuf, &peer->blockToUs.offsetInPiece );
            peer->blockToUs.length = msglen - 8;
            assert( peer->blockToUs.length > 0 );
            evbuffer_drain( peer->inBlock, ~0 );
            break;
        }

        case BT_PORT: {
            assert( msglen == 2 );
            fprintf( stderr, "got a BT_PORT\n" );
            tr_peerConnectionReadUint16( peer->connection, inbuf, &peer->listeningPort );
            break;
        }

        case BT_LTEP:
            fprintf( stderr, "got a BT_LTEP\n" );
            parseLtep( peer, msglen, inbuf );
            break;

        default:
            fprintf( stderr, "got an unknown BT message type: %d\n", (int)id );
            tr_peerConnectionDrain( peer->connection, inbuf, msglen );
            assert( 0 );
    }

    peer->incomingMessageLength = -1;
    peer->state = AWAITING_BT_LENGTH;
    return READ_AGAIN;
}

static int
canDownload( const tr_peer * peer )
{
    tr_torrent * tor = peer->torrent;

#if 0
    /* FIXME: was swift worth it?  did anyone notice a difference? :) */
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
gotBlock( tr_peer * peer, int pieceIndex, int offset, struct evbuffer * inbuf )
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
    if( !peer->blamefield )
         peer->blamefield = tr_bitfieldNew( tor->info.pieceCount );
    tr_bitfieldAdd( peer->blamefield, pieceIndex );

    tr_cpBlockAdd( tor->completion, block );

    tor->downloadedCur += len;
    tr_rcTransferred( peer->rcToUs, len );
    tr_rcTransferred( tor->download, len );
    tr_rcTransferred( tor->handle->download, len );

//    broadcastCancel( tor, index, begin, len - 8 );
}


static ReadState
readBtPiece( tr_peer * peer, struct evbuffer * inbuf )
{
    assert( peer->blockToUs.length > 0 );

    if( !canDownload( peer ) )
    {
        peer->notListening = 1;
        tr_peerConnectionSetIOMode ( peer->connection, 0, EV_READ );
        return READ_DONE;
    }
    else
    {
        /* inbuf ->  inBlock */
        const uint32_t len = MIN( EVBUFFER_LENGTH(inbuf), peer->blockToUs.length );
        uint8_t * tmp = tr_new( uint8_t, len );
        tr_peerConnectionReadBytes( peer->connection, inbuf, tmp, len );
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
    tr_peer * peer = (tr_peer *) vpeer;
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
canUpload( const tr_peer * peer )
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
    tr_peer * peer = (tr_peer *) vpeer;
    size_t len;

    /* if we froze out a downloaded block because of speed limits,
       start listening to the peer again */
    if( peer->notListening )
    {
        fprintf( stderr, "peer %p thawing out...\n", peer );
        peer->notListening = 0;
        tr_peerConnectionSetIOMode ( peer->connection, EV_READ, 0 );
    }

    if(( len = EVBUFFER_LENGTH( peer->outBlock ) ))
    {
        if( canUpload( peer ) )
        {
            const size_t outlen = MIN( len, 2048 );
            tr_peerConnectionWrite( peer->connection, EVBUFFER_DATA(peer->outBlock), outlen );
            evbuffer_drain( peer->outBlock, outlen );
        }
    }
    else if(( len = EVBUFFER_LENGTH( peer->outMessages ) ))
    {
        fprintf( stderr, "peer %p pulse is writing %d bytes worth of messages...\n", peer, (int)len );
        tr_peerConnectionWriteBuf( peer->connection, peer->outMessages );
        evbuffer_drain( peer->outMessages, ~0 );
    }
    else if(( peer->peerAskedFor ))
    {
        struct peer_request * req = (struct peer_request*) peer->peerAskedFor->data;
        uint8_t * tmp = tr_new( uint8_t, req->length );
        const uint8_t msgid = BT_PIECE;
        const uint32_t msglen = sizeof(uint8_t) + sizeof(uint32_t)*2 + req->length;
        tr_ioRead( peer->torrent, req->pieceIndex, req->offsetInPiece, req->length, tmp );
        tr_peerConnectionWriteUint32( peer->connection, peer->outBlock, msglen );
        tr_peerConnectionWriteBytes ( peer->connection, peer->outBlock, &msgid, 1 );
        tr_peerConnectionWriteUint32( peer->connection, peer->outBlock, req->pieceIndex );
        tr_peerConnectionWriteUint32( peer->connection, peer->outBlock, req->offsetInPiece );
        tr_peerConnectionWriteBytes ( peer->connection, peer->outBlock, tmp, req->length );
        tr_free( tmp );
    }

    return TRUE; /* loop forever */
}

static void
didWrite( struct bufferevent * evin UNUSED, void * vpeer )
{
    tr_peer * peer = (tr_peer *) vpeer;
    fprintf( stderr, "peer %p got a didWrite...\n", peer );
    pulse( vpeer );
}

static void
gotError( struct bufferevent * evbuf UNUSED, short what, void * vpeer )
{
    tr_peer * peer = (tr_peer *) vpeer;
    fprintf( stderr, "peer %p got an error in %d\n", peer, (int)what );
}

static void
sendBitfield( tr_peer * peer )
{
    const tr_bitfield * bitfield = tr_cpPieceBitfield( peer->torrent->completion );
    const uint32_t len = sizeof(uint8_t) + bitfield->len;
    const uint8_t bt_msgid = BT_BITFIELD;

    fprintf( stderr, "peer %p: enqueueing a bitfield message\n", peer );
    tr_peerConnectionWriteUint32( peer->connection, peer->outMessages, len );
    tr_peerConnectionWriteBytes( peer->connection, peer->outMessages, &bt_msgid, 1 );
    tr_peerConnectionWriteBytes( peer->connection, peer->outMessages, bitfield->bits, bitfield->len );
}

void
tr_peerManagerAdd( struct tr_torrent        * torrent,
                   struct tr_peerConnection * connection )
{
    tr_peer * peer;

    peer = tr_new0( tr_peer, 1 );
    peer->handle = torrent->handle;
    peer->torrent = torrent;
    peer->connection = connection;
    peer->weAreChoked = 1;
    peer->peerIsChoked = 1;
    peer->weAreInterested = 0;
    peer->peerIsInterested = 0;
    peer->pulseTag = tr_timerNew( peer->handle, pulse, peer, NULL, 200 );
    peer->bitfield = tr_bitfieldNew( torrent->info.pieceCount );
    peer->outMessages = evbuffer_new( );
    peer->outBlock = evbuffer_new( );
    peer->inBlock = evbuffer_new( );

    tr_peerConnectionSetIOFuncs( connection, canRead, didWrite, gotError, peer );
    tr_peerConnectionSetIOMode( connection, EV_READ|EV_WRITE, 0 );

    sendBitfield( peer );
}

void
tr_peerGetInfo( const tr_peer * peer, tr_peer_stat * setme )
{
    setme->client = peer->client;
    setme->isConnected = TRUE;
    setme->progress = peer->progress;
    setme->downloadFromRate = tr_rcRate( peer->rcToUs );
    setme->downloadFromRate = tr_rcRate( peer->rcToPeer );

    //char    addr[INET_ADDRSTRLEN];
    //int     from;
    //int     port;
    //int     isDownloading;
    //int     isUploading;

}
