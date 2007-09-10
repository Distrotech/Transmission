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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/queue.h> /* for evhttp */
#include <sys/types.h> /* for evhttp */

#include <event.h>
#include <evdns.h>
#include <evhttp.h>

#include "transmission.h"
#include "list.h"
#include "platform.h"
#include "utils.h"

/* #define DEBUG */
#ifdef DEBUG
#include <stdio.h>
#undef tr_dbg
#define tr_dbg( a, b... ) fprintf(stderr, a "\n", ##b )
#endif

/***
****
***/

typedef struct tr_event_handle
{
    tr_lock * lock;
    tr_handle_t * h;
    tr_thread * thread;
    tr_list * commands;
    struct event_base * base;
    struct event pulse;
    struct timeval pulseInterval;
    uint8_t die;
}
tr_event_handle;

#ifdef DEBUG
static int reads = 0;
static int writes = 0;
#endif

enum mode
{
   TR_EV_EVENT_DEL,
   TR_EV_EVENT_ADD,
   TR_EV_EVHTTP_MAKE_REQUEST,
   TR_EV_BUFFEREVENT_SET,
   TR_EV_BUFFEREVENT_WRITE,
   TR_EV_BUFFEREVENT_FREE
};

struct tr_event_command
{
    int mode;

    struct event * event;
    struct timeval interval;

    struct evhttp_connection * evcon;
    struct evhttp_request * req;
    enum evhttp_cmd_type evtype;
    char * uri;

    struct bufferevent * bufev;
    short enable;
    short disable;
    char * buf;
    size_t buflen;
};

static void
pumpList( int i UNUSED, short s UNUSED, void * veh )
{
    tr_event_handle * eh = veh;

    for( ;; )
    {
        struct tr_event_command * cmd;

        /* get the next command */
        tr_lockLock( eh->lock );
        cmd = tr_list_pop_front( &eh->commands );
        tr_lockUnlock( eh->lock );
        if( cmd == NULL )
            break;

        /* process the command */
        switch( cmd->mode )
        {
            case TR_EV_EVENT_DEL:
                event_del( cmd->event );
                tr_free( cmd->event );
                break;

            case TR_EV_EVENT_ADD:
                event_add( cmd->event, &cmd->interval );
                break;

            case TR_EV_EVHTTP_MAKE_REQUEST:
                evhttp_make_request( cmd->evcon, cmd->req, cmd->evtype, cmd->uri );
                tr_free( cmd->uri );
                break;

           case TR_EV_BUFFEREVENT_SET:
                bufferevent_enable( cmd->bufev, cmd->enable );
                bufferevent_disable( cmd->bufev, cmd->disable );
                break;

            case TR_EV_BUFFEREVENT_WRITE:
                bufferevent_write( cmd->bufev, cmd->buf, cmd->buflen );
                tr_free( cmd->buf );
                break;

            case TR_EV_BUFFEREVENT_FREE:
                bufferevent_free( cmd->bufev );
                break;

            default:
                assert( 0 && "unhandled command type!" );
        }

        /* cleanup */
        tr_free( cmd );
    }

    if( !eh->die )
        timeout_add( &eh->pulse, &eh->pulseInterval );
}

static void
logFunc( int severity, const char * message )
{
    switch( severity )
    {
        case _EVENT_LOG_DEBUG: 
            tr_dbg( "%s", message );
            break;
        case _EVENT_LOG_ERR:
            tr_err( "%s", message );
            break;
        default:
            tr_inf( "%s", message );
            break;
    }
}

static void
libeventThreadFunc( void * veh )
{
    tr_event_handle * eh = (tr_event_handle *) veh;
    tr_dbg( "Starting libevent thread" );

    eh->base = event_init( );
    event_set_log_callback( logFunc );
    evdns_init( );
    timeout_set( &eh->pulse, pumpList, veh );
    timeout_add( &eh->pulse, &eh->pulseInterval );

    event_dispatch( );

    evdns_shutdown( FALSE );
    tr_lockFree( eh->lock );
    event_base_free( eh->base );
    tr_free( eh );

    tr_dbg( "Closing libevent thread" );
}

void
tr_eventInit( tr_handle_t * handle )
{
    tr_event_handle * eh;

    eh = tr_new0( tr_event_handle, 1 );
    eh->lock = tr_lockNew( );
    eh->h = handle;
    eh->pulseInterval = timevalMsec( 20 );
    eh->thread = tr_threadNew( libeventThreadFunc, eh, "libeventThreadFunc" );

    handle->events = eh;
}

void
tr_eventClose( tr_handle_t * handle )
{
    tr_event_handle * eh = handle->events;
    eh->die = TRUE;
    event_base_loopexit( eh->base, NULL );
}

/**
***
**/

static void
pushList( struct tr_event_handle * eh, struct tr_event_command * command )
{
    tr_lockLock( eh->lock );
    tr_list_append( &eh->commands, command );
    tr_lockUnlock( eh->lock );
}

void
tr_event_add( tr_handle_t    * handle,
              struct event   * event,
              struct timeval * interval )
{
    if( tr_amInThread( handle->events->thread ) )
        event_add( event, interval );
    else {
        struct tr_event_command * cmd = tr_new0( struct tr_event_command, 1 );
        cmd->mode = TR_EV_EVENT_ADD;
        cmd->event = event;
        cmd->interval = *interval;
        pushList( handle->events, cmd );
    }
}

void
tr_event_del( tr_handle_t    * handle,
              struct event   * event )
{
    if( tr_amInThread( handle->events->thread ) ) {
        event_del( event );
        tr_free( event );
    } else {
        struct tr_event_command * cmd = tr_new0( struct tr_event_command, 1 );
        cmd->mode = TR_EV_EVENT_DEL;
        cmd->event = event;
        pushList( handle->events, cmd );
    }
}

void
tr_evhttp_make_request (tr_handle_t               * handle,
                        struct evhttp_connection  * evcon,
                        struct evhttp_request     * req,
                        enum   evhttp_cmd_type      type,
                        char                      * uri)
{
    if( tr_amInThread( handle->events->thread ) ) {
        evhttp_make_request( evcon, req, type, uri );
        tr_free( uri );
    } else {
        struct tr_event_command * cmd = tr_new0( struct tr_event_command, 1 );
        cmd->mode = TR_EV_EVHTTP_MAKE_REQUEST;
        cmd->evcon = evcon;
        cmd->req = req;
        cmd->evtype = type;
        cmd->uri = uri;
        pushList( handle->events, cmd );
    }
}

void
tr_bufferevent_write( tr_handle_t           * handle,
                      struct bufferevent    * bufev,
                      const void            * buf,
                      size_t                  buflen )
{
    if( tr_amInThread( handle->events->thread ) )
        bufferevent_write( bufev, (void*)buf, buflen );
    else {
        struct tr_event_command * cmd = tr_new0( struct tr_event_command, 1 );
        cmd->mode = TR_EV_BUFFEREVENT_WRITE;
        cmd->bufev = bufev;
        cmd->buf = tr_strndup( buf, buflen );
        cmd->buflen = buflen;
        pushList( handle->events, cmd );
    }
}

void
tr_setBufferEventMode( struct tr_handle   * handle,
                       struct bufferevent * bufev,
                       short                mode_enable,
                       short                mode_disable )
{
    if( tr_amInThread( handle->events->thread ) ) {
        bufferevent_enable( bufev, mode_enable );
        bufferevent_disable( bufev, mode_disable );
    } else {
        struct tr_event_command * cmd = tr_new0( struct tr_event_command, 1 );
        cmd->mode = TR_EV_BUFFEREVENT_SET;
        cmd->bufev = bufev;
        cmd->enable = mode_enable;
        cmd->disable = mode_disable;
        pushList( handle->events, cmd );
    }
}

static int
compareFunc( const void * va, const void * vb )
{
    const struct tr_event_command * a = va;
    const struct bufferevent * b = vb;
    return a->bufev == b ? 0 : 1;
}

void
tr_bufferevent_free( struct tr_handle   * handle,
                     struct bufferevent * bufev )
{
    void * v;
    tr_event_handle * eh = handle->events;

    /* purge pending commands from the list */
    tr_lockLock( eh->lock );
    while(( v = tr_list_remove( &eh->commands, bufev, compareFunc ) )) {
        fprintf( stderr, "---> I AM PURGING A QUEUED COMMAND BECAUSE ITS BUFEV IS GOING AWAY <--\n" );
        tr_free( v );
    }
    tr_lockUnlock( eh->lock );

    if( tr_amInThread( handle->events->thread ) )
        bufferevent_free( bufev );
    else {
        struct tr_event_command * cmd = tr_new0( struct tr_event_command, 1 );
        cmd->mode = TR_EV_BUFFEREVENT_FREE;
        cmd->bufev = bufev;
        pushList( handle->events, cmd );
    }
}
