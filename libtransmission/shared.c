/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2005-2007 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>

#include "transmission.h"
#include "handshake.h"
#include "natpmp.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "platform.h"
#include "shared.h"
#include "upnp.h"
#include "utils.h"

/* Maximum number of peers that we keep in our local list */
/* This is an arbitrary number, but it seems to work well */
#define MAX_PEER_COUNT 128

struct tr_shared
{
    tr_handle  * h;
    volatile int   die;
    tr_thread_t  * thread;
    tr_lock_t    * lock;

    /* Incoming connections */
    int          publicPort;
    int          bindPort;
    int          bindSocket;

    /* NAT-PMP/UPnP */
    tr_natpmp_t  * natpmp;
    tr_upnp_t    * upnp;
};

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static void SharedLoop( void * );
static void SetPublicPort( tr_shared *, int );
static void AcceptPeers( tr_shared * );


/***********************************************************************
 * tr_sharedInit
 ***********************************************************************
 *
 **********************************************************************/
tr_shared * tr_sharedInit( tr_handle * h )
{
    tr_shared * s = calloc( 1, sizeof( tr_shared ) );

    s->h          = h;
    s->lock       = tr_lockNew( );
    s->publicPort = -1;
    s->bindPort   = -1;
    s->bindSocket = -1;
    s->natpmp     = tr_natpmpInit();
    s->upnp       = tr_upnpInit();
    s->die        = 0;
    s->thread     = tr_threadNew( SharedLoop, s, "shared" );

    return s;
}

/***********************************************************************
 * tr_sharedClose
 ***********************************************************************
 *
 **********************************************************************/
void tr_sharedClose( tr_shared * s )
{
    /* Stop the thread */
    s->die = 1;
    tr_threadJoin( s->thread );

    if( s->bindSocket > -1 )
        tr_netClose( s->bindSocket );
    tr_lockFree( s->lock );
    tr_natpmpClose( s->natpmp );
    tr_upnpClose( s->upnp );
    free( s );
}

/***********************************************************************
 * tr_sharedLock, tr_sharedUnlock
 ***********************************************************************
 *
 **********************************************************************/
void tr_sharedLock( tr_shared * s )
{
    tr_lockLock( s->lock );
}
void tr_sharedUnlock( tr_shared * s )
{
    tr_lockUnlock( s->lock );
}

/***********************************************************************
 * tr_sharedSetPort
 ***********************************************************************
 *
 **********************************************************************/
void tr_sharedSetPort( tr_shared * s, int port )
{
#ifdef BEOS_NETSERVER
    /* BeOS net_server seems to be unable to set incoming connections
     * to non-blocking. Too bad. */
    return;
#endif

    tr_sharedLock( s );

    if( port == s->bindPort )
    {
        tr_sharedUnlock( s );
        return;
    }
    s->bindPort = port;

    /* Close the previous accept socket, if any */
    if( s->bindSocket > -1 )
    {
        tr_netClose( s->bindSocket );
    }

    /* Create the new one */
    /* XXX should handle failure here in a better way */
    s->bindSocket = tr_netBindTCP( port );
    if( 0 > s->bindSocket )
    {
        /* Notify the trackers */
        SetPublicPort( s, 0 );
        /* Remove the forwarding for the old port */
        tr_natpmpRemoveForwarding( s->natpmp );
        tr_upnpRemoveForwarding( s->upnp );
    }
    else
    {
        tr_inf( "Bound listening port %d", port );
        listen( s->bindSocket, 5 );
        if( port != s->publicPort )
        {
            /* Notify the trackers */
            SetPublicPort( s, port );
        }
        /* Forward the new port */
        tr_natpmpForwardPort( s->natpmp, port );
        tr_upnpForwardPort( s->upnp, port );
    }

    tr_sharedUnlock( s );
}

/***********************************************************************
 * tr_sharedGetPublicPort
 ***********************************************************************
 *
 **********************************************************************/
int tr_sharedGetPublicPort( tr_shared * s )
{
    return s->publicPort;
}

/***********************************************************************
 * tr_sharedTraversalEnable, tr_sharedTraversalStatus
 ***********************************************************************
 *
 **********************************************************************/
void tr_sharedTraversalEnable( tr_shared * s, int enable )
{
    if( enable )
    {
        tr_natpmpStart( s->natpmp );
        tr_upnpStart( s->upnp );
    }
    else
    {
        tr_natpmpStop( s->natpmp );
        tr_upnpStop( s->upnp );
    }
}

int tr_sharedTraversalStatus( tr_shared * s )
{
    int statuses[] = {
        TR_NAT_TRAVERSAL_MAPPED,
        TR_NAT_TRAVERSAL_MAPPING,
        TR_NAT_TRAVERSAL_UNMAPPING,
        TR_NAT_TRAVERSAL_ERROR,
        TR_NAT_TRAVERSAL_NOTFOUND,
        TR_NAT_TRAVERSAL_DISABLED,
        -1,
    };
    int natpmp, upnp, ii;

    natpmp = tr_natpmpStatus( s->natpmp );
    upnp = tr_upnpStatus( s->upnp );

    for( ii = 0; 0 <= statuses[ii]; ii++ )
    {
        if( statuses[ii] == natpmp || statuses[ii] == upnp )
        {
            return statuses[ii];
        }
    }

    assert( 0 );

    return TR_NAT_TRAVERSAL_ERROR;

}


/***********************************************************************
 * Local functions
 **********************************************************************/

/***********************************************************************
 * SharedLoop
 **********************************************************************/
static void SharedLoop( void * _s )
{
    tr_shared * s = _s;
    uint64_t      date1, date2;
    int           newPort;

    tr_sharedLock( s );

    while( !s->die )
    {
        date1 = tr_date();

        /* NAT-PMP and UPnP pulses */
        newPort = -1;
        tr_natpmpPulse( s->natpmp, &newPort );
        if( 0 < newPort && newPort != s->publicPort )
        {
            SetPublicPort( s, newPort );
        }
        tr_upnpPulse( s->upnp );

        /* Handle incoming connections */
        AcceptPeers( s );

        /* Wait up to 20 ms */
        date2 = tr_date();
        if( date2 < date1 + 20 )
        {
            tr_sharedUnlock( s );
            tr_wait( date1 + 20 - date2 );
            tr_sharedLock( s );
        }
    }

    tr_sharedUnlock( s );
}

/***********************************************************************
 * SetPublicPort
 **********************************************************************/
static void SetPublicPort( tr_shared * s, int port )
{
    tr_handle * h = s->h;
    tr_torrent * tor;

    s->publicPort = port;

    for( tor = h->torrentList; tor; tor = tor->next )
        tr_torrentChangeMyPort( tor, port );
}

/***********************************************************************
 * AcceptPeers
 ***********************************************************************
 * Check incoming connections and add the peers to our local list
 **********************************************************************/

static void
AcceptPeers( tr_shared * s )
{
    for( ;; )
    {
        int socket;
        struct in_addr addr;

        if( s->bindSocket < 0 || !tr_peerMgrIsAcceptingConnections( s->h->peerMgr ) )
            break;

        socket = tr_netAccept( s->bindSocket, &addr, NULL );
        if( socket < 0 )
            break;

        tr_peerMgrAddIncoming( s->h->peerMgr, &addr, socket );
    }
}
