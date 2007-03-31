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
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "errors.h"
#include "misc.h"
#include "transmission.h"

static void pushdir ( char *, const char *, size_t );

static char gl_myname[256];

void
setmyname( const char * argv0 )
{
    const char * name;

    name = strrchr( argv0, '/' );
    if( NULL == name || '\0' == *(++name) )
    {
        name = argv0;
    }
    strlcpy( gl_myname, name, sizeof gl_myname );
}

const char *
getmyname( void )
{
    return gl_myname;
}

void
pushdir( char * path, const char * file, size_t size )
{
    size_t off;

    off = strlen( path );
    if( 0 < off && off + 1 < size && '/' != path[off-1] )
    {
        path[off]   = '/';
        path[off+1] = '\0';
    }
    strlcat( path, file, size );
}

void
confpath( char * buf, size_t len, const char * file, enum confpathtype type )
{
    const char * typestr;

    switch( type )
    {
        case CONF_PATH_TYPE_DAEMON:
            typestr = "daemon";
            break;
        case CONF_PATH_TYPE_GTK:
            typestr = "gtk";
            break;
        default:
            assert( 0 );
            break;
    }

    strlcpy(     buf, tr_getPrefsDirectory(), len );
    pushdir(     buf, typestr,                len );
    if( NULL != file )
    {
        pushdir( buf, file,                   len );
    }
}

void
absolutify( char * buf, size_t len, const char * path )
{
    size_t off;

    if( '/' == path[0] )
    {
        strlcpy( buf, path, len );
        return;
    }

    getcwd( buf, len );
    off = strlen( buf );
    if( 0 < off && len > off + 1 && '/' != buf[off] )
    {
        buf[off] = '/';
        off++;
        buf[off] = '\0';
    }
    strlcat( buf, path, len );
}
