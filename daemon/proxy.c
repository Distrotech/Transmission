/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2007 Joshua Elsasser
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

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <event.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "errors.h"
#include "misc.h"
#include "transmission.h"

#define TIMEOUT                 ( 60 )

static void                 usage    ( const char *, ... );
static enum confpathtype    readargs ( int, char ** );
static int                  makesock ( enum confpathtype );
static struct bufferevent * setupev  ( struct event_base *, int,
                                       void ( * )( struct bufferevent *, short,
                                                   void * ), void * );
static void                 noop     ( struct bufferevent *, void * );
static void                 relay    ( struct bufferevent *, void * );
static void                 outerr   ( struct bufferevent *, short, void * );
static void                 inerr    ( struct bufferevent *, short, void * );
static void                 sockerr  ( struct bufferevent *, short, void * );

int
main( int argc, char ** argv )
{
    struct event_base  * base;
    enum confpathtype    type;
    int                  sockfd;
    struct bufferevent * outev, * inev, * sockev;

    setmyname( argv[0] );
    type = readargs( argc, argv );
    base = event_init();

    sockfd = makesock( type );
    if( 0 > sockfd )
    {
        return EXIT_FAILURE;
    }

    outev  = setupev( base, STDOUT_FILENO, outerr,  NULL );
    sockev = setupev( base, sockfd,        sockerr, outev );
    inev   = setupev( base, STDIN_FILENO,  inerr,   sockev );

    if( NULL == outev || NULL == inev || NULL == sockev )
    {
        return EXIT_FAILURE;
    }

    bufferevent_disable( outev,  EV_READ );
    bufferevent_enable(  outev,  EV_WRITE );
    bufferevent_enable(  inev,   EV_READ );
    bufferevent_disable( inev,   EV_WRITE );
    bufferevent_enable(  sockev, EV_READ );
    bufferevent_enable(  sockev, EV_WRITE );

    event_base_dispatch( base );

    return EXIT_FAILURE;
}

void
usage( const char * msg, ... )
{
    va_list ap;

    if( NULL != msg )
    {
        printf( "%s: ", getmyname() );
        va_start( ap, msg );
        vprintf( msg, ap );
        va_end( ap );
        printf( "\n" );
    }

    printf(
  "usage: %s [options] [files]...\n"
  "\n"
  "Transmission %s (r%d) http://transmission.m0k.org/\n"
  "A free, lightweight BitTorrent client with a simple, intuitive interface.\n"
  "\n"
  "  -h --help                 Display this message and exit\n"
  "  -t --type daemon          Use the daemon frontend, transmission-daemon\n"
  "  -t --type gtk             Use the GTK+ frontend, transmission-gtk\n",
            getmyname(), VERSION_STRING, VERSION_REVISION );
    exit( EXIT_SUCCESS );
}

enum confpathtype
readargs( int argc, char ** argv )
{
    char optstr[] = "ht:";
    struct option longopts[] =
    {
        { "help",               no_argument,       NULL, 'h' },
        { "type",               required_argument, NULL, 't' },
        { NULL, 0, NULL, 0 }
    };
    enum confpathtype type;
    int opt;

    type = CONF_PATH_TYPE_DAEMON;

    while( 0 <= ( opt = getopt_long( argc, argv, optstr, longopts, NULL ) ) )
    {
        switch( opt )
        {
            case 't':
                if( 0 == strcasecmp( "daemon", optarg ) )
                {
                    type = CONF_PATH_TYPE_DAEMON;
                }
                else if( 0 == strcasecmp( "gtk", optarg ) )
                {
                    type = CONF_PATH_TYPE_GTK;
                }
                else
                {
                    usage( "invalid type: %s", optarg );
                }
                break;
            default:
                usage( NULL );
                break;
        }
    }

    return type;
}

int
makesock( enum confpathtype type )
{
    struct sockaddr_un sun;
    int                fd;

    bzero( &sun, sizeof sun );
    sun.sun_family = AF_LOCAL;
    confpath( sun.sun_path, sizeof sun.sun_path, CONF_FILE_SOCKET, type );

    fd = socket( AF_UNIX, SOCK_STREAM, 0 );
    if( 0 > fd )
    {
        errnomsg( "failed to create socket" );
        return -1;
    }

    if( 0 > connect( fd, ( struct sockaddr * )&sun, SUN_LEN( &sun ) ) )
    {
        errnomsg( "failed to connect to socket file: %s", sun.sun_path );
        close( fd );
        return -1;
    }

    return fd;
}

struct bufferevent *
setupev( struct event_base * base, int fd,
         void ( * efunc )( struct bufferevent *, short, void * ), void * arg )
{
    struct bufferevent * ev;

    ev = bufferevent_new( fd, relay, noop, efunc, arg );
    if( NULL == ev )
    {
        mallocmsg( -1 );
        return NULL;
    }

    bufferevent_base_set( base, ev );
    bufferevent_settimeout( ev, TIMEOUT, TIMEOUT );

    return ev;
}

void
noop( struct bufferevent * ev UNUSED, void * arg UNUSED )
{
    /* libevent prior to 1.2 couldn't handle a NULL write callback */
}

void
relay( struct bufferevent * in, void * arg )
{
    struct bufferevent * out = arg;

    if( NULL == arg )
    {
        /* this shouldn't happen, but let's drain the buffer anyway */
        evbuffer_drain( EVBUFFER_INPUT( in ),
                        EVBUFFER_LENGTH( EVBUFFER_INPUT( in ) ) );
    }
    else
    {
        bufferevent_write_buffer( out, EVBUFFER_INPUT( in ) );
    }
}

void
outerr( struct bufferevent * ev UNUSED, short what, void * arg UNUSED )
{
    if( EVBUFFER_TIMEOUT & what )
    {
        errmsg( "timed out writing to stdout" );
    }
    else if( EVBUFFER_WRITE & what )
    {
        errmsg( "write error on stdout" );
    }
    else if( EVBUFFER_ERROR & what )
    {
        errmsg( "error on client stdout" );
    }
    else
    {
        errmsg( "unknown error on stdout connection: 0x%x", what );
    }

    exit( EXIT_FAILURE );
}

void
inerr( struct bufferevent * ev UNUSED, short what, void * arg UNUSED )
{
    if( EVBUFFER_EOF & what )
    {
        exit( EXIT_SUCCESS );
    }
    else if( EVBUFFER_TIMEOUT & what )
    {
        errmsg( "timed out reading from stdin" );
    }
    else if( EVBUFFER_READ & what )
    {
        errmsg( "read error on stdin" );
    }
    else if( EVBUFFER_ERROR & what )
    {
        errmsg( "error on stdin" );
    }
    else
    {
        errmsg( "unknown error on stdin: 0x%x", what );
    }

    exit( EXIT_FAILURE );
}

void
sockerr( struct bufferevent * ev UNUSED, short what, void * arg UNUSED )
{
    if( EVBUFFER_EOF & what )
    {
        errmsg( "server closed connection" );
    }
    else if( EVBUFFER_TIMEOUT & what )
    {
        errmsg( "server connection timed out" );
    }
    else if( EVBUFFER_READ & what )
    {
        errmsg( "read error on server connection" );
    }
    else if( EVBUFFER_WRITE & what )
    {
        errmsg( "write error on server connection" );
    }
    else if( EVBUFFER_ERROR & what )
    {
        errmsg( "error on server connection" );
    }
    else
    {
        errmsg( "unknown error on server connection: 0x%x", what );
    }

    exit( EXIT_FAILURE );
}
