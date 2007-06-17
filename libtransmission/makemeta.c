/******************************************************************************
 * $Id:$
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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <stdio.h> /* FILE, snprintf, stderr */
#include <stdlib.h> /* malloc, calloc */

#include "transmission.h"
#include "internal.h" /* for tr_torrent_t */
#include "bencode.h"
#include "makemeta.h"
#include "platform.h" /* threads, locks */
#include "shared.h" /* shared lock */
#include "version.h"

/****
*****
****/

struct FileList
{
    struct FileList * next;
    char filename[MAX_PATH_LENGTH];
};

static struct FileList*
getFiles( const char        * dir,
          const char        * base,
          struct FileList   * list )
{
    int i;
    char buf[MAX_PATH_LENGTH];
    struct stat sb;
    sb.st_size = 0;

    snprintf( buf, sizeof(buf), "%s"TR_PATH_DELIMITER_STR"%s", dir, base );
    i = stat( buf, &sb );
    if( i ) {
        tr_err("makemeta couldn't stat \"%s\"; skipping. (%s)", buf, strerror(errno));
        return list;
    }

    if ( S_ISDIR( sb.st_mode ) )
    {
        DIR * odir = opendir( buf );
        struct dirent *d;
        for (d = readdir( odir ); d!=NULL; d=readdir( odir ) )
            if( strcmp( d->d_name,"." ) && strcmp( d->d_name,".." ) )
                list = getFiles( buf, d->d_name, list );
        closedir( odir );
    }
    else if( S_ISREG( sb.st_mode ) )
    {
        struct FileList * node = malloc( sizeof( struct FileList ) );
        snprintf( node->filename, sizeof( node->filename ), "%s", buf );
        node->next = list;
        list = node;
    }

    return list;
}

static void
freeFileList( struct FileList * list )
{
    while( list ) {
        struct FileList * tmp = list->next;
        free( list );
        list = tmp;
    }
}

static off_t
getFileSize ( const char * filename )
{
    struct stat sb;
    sb.st_size = 0;
    stat( filename, &sb );
    return sb.st_size;
}

#define MiB 1048576ul
#define GiB 1073741824ul

static size_t
bestPieceSize( size_t totalSize )
{
    /* almost always best to have a piee size of 512 or 256 kb.
       common practice seems to be to bump up to 1MB pieces at
       at total size of around 8GiB or so */

    if (totalSize >= (8 * GiB) )
        return MiB;

    if (totalSize >= GiB )
        return MiB / 2;

    return MiB / 4;
}

/****
*****
****/

static int pstrcmp( const void * va, const void * vb)
{
    const char * a = *(const char**) va;
    const char * b = *(const char**) vb;
    return strcmp( a, b );
}

tr_metainfo_builder_t*
tr_metaInfoBuilderCreate( tr_handle_t * handle, const char * topFile )
{
    size_t i;
    struct FileList * files;
    const struct FileList * walk;
    tr_metainfo_builder_t * ret = calloc( 1, sizeof(tr_metainfo_builder_t) );
    ret->top = tr_strdup( topFile );
    ret->handle = handle;

    if (1) {
        struct stat sb;
        stat( topFile, &sb );
        ret->isSingleFile = !S_ISDIR( sb.st_mode );
    }

    /* build a list of files containing topFile and,
       if it's a directory, all of its children */
    if (1) {
        char *dir, *base;
        char dirbuf[MAX_PATH_LENGTH];
        char basebuf[MAX_PATH_LENGTH];
        strlcpy( dirbuf, topFile, sizeof( dirbuf ) );
        strlcpy( basebuf, topFile, sizeof( basebuf ) );
        dir = dirname( dirbuf );
        base = basename( basebuf );
        files = getFiles( dir, base, NULL );
    }

    for( walk=files; walk!=NULL; walk=walk->next )
        ++ret->fileCount;
    ret->files = calloc( ret->fileCount, sizeof(char*) );
    ret->fileLengths = calloc( ret->fileCount, sizeof(size_t) );

    for( i=0, walk=files; walk!=NULL; walk=walk->next, ++i )
        ret->files[i] = tr_strdup( walk->filename );

    qsort( ret->files, ret->fileCount, sizeof(char*), pstrcmp );
    for( i=0; i<ret->fileCount; ++i ) {
        ret->fileLengths[i] = getFileSize( ret->files[i] );
        ret->totalSize += ret->fileLengths[i];
    }

    freeFileList( files );
    
    ret->pieceSize = bestPieceSize( ret->totalSize );
    ret->pieceCount = ret->totalSize / ret->pieceSize;
    if( ret->totalSize % ret->pieceSize )
        ++ret->pieceCount;

    return ret;
}

void
tr_metaInfoBuilderFree( tr_metainfo_builder_t * builder )
{
    if( builder != NULL )
    {
        size_t i;
        for( i=0; i<builder->fileCount; ++i )
            tr_free( builder->files[i] );
        tr_free( builder->files );
        tr_free( builder->fileLengths );
        tr_free( builder->top );
        tr_free( builder->comment );
        tr_free( builder->announce );
        tr_free( builder->outputFile );
        tr_free( builder );
    }
}

/****
*****
****/

static uint8_t*
getHashInfo ( tr_metainfo_builder_t * b )
{
    size_t off = 0;
    size_t fileIndex = 0;
    uint8_t *ret = (uint8_t*) malloc ( SHA_DIGEST_LENGTH * b->pieceCount );
    uint8_t *walk = ret;
    uint8_t *buf = malloc( b->pieceSize );
    size_t totalRemain;
    FILE * fp;

    b->pieceIndex = 0;
    totalRemain = b->totalSize;
    fp = fopen( b->files[fileIndex], "rb" );
    while ( totalRemain )
    {
        uint8_t *bufptr = buf;
        const size_t thisPieceSize = MIN( b->pieceSize, totalRemain );
        size_t pieceRemain = thisPieceSize;

        assert( b->pieceIndex < b->pieceCount );

        while( pieceRemain )
        {
            const size_t n_this_pass = MIN( (b->fileLengths[fileIndex] - off), pieceRemain );
            fread( bufptr, 1, n_this_pass, fp );
            bufptr += n_this_pass;
            off += n_this_pass;
            pieceRemain -= n_this_pass;
            if( off == b->fileLengths[fileIndex] ) {
                off = 0;
                fclose( fp );
                fp = NULL;
                if( ++fileIndex < b->fileCount ) {
                    fp = fopen( b->files[fileIndex], "rb" );
                }
            }
        }

        assert( bufptr-buf == (int)thisPieceSize );
        assert( pieceRemain == 0 );
        SHA1( buf, thisPieceSize, walk );
        walk += SHA_DIGEST_LENGTH;

        if( b->abortFlag ) {
            b->failed = 1;
            break;
        }

        totalRemain -= thisPieceSize;
        ++b->pieceIndex;
    }
    assert( b->abortFlag || (walk-ret == (int)(SHA_DIGEST_LENGTH*b->pieceCount)) );
    assert( b->abortFlag || !totalRemain );
    assert( fp == NULL );

    free( buf );
    return ret;
}

static void
getFileInfo( const char * topFile,
             const char * filename,
             benc_val_t * uninitialized_length,
             benc_val_t * uninitialized_path )
{
    benc_val_t *sub;
    const char *pch, *prev;
    const size_t topLen = strlen(topFile) + 1; /* +1 for '/' */
    int n;

    /* get the file size */
    tr_bencInitInt( uninitialized_length, getFileSize(filename) );

    /* the path list */
    n = 1;
    for( pch=filename+topLen; *pch; ++pch )
        if (*pch == TR_PATH_DELIMITER)
            ++n;
    tr_bencInit( uninitialized_path, TYPE_LIST );
    tr_bencListReserve( uninitialized_path, n );
    for( prev=pch=filename+topLen; ; ++pch )
    {
        char buf[MAX_PATH_LENGTH];

        if (*pch && *pch!=TR_PATH_DELIMITER )
            continue;

        memcpy( buf, prev, pch-prev );
        buf[pch-prev] = '\0';
        /*fprintf ( stderr, "adding [%s] to the list of paths\n", buf );*/

        sub = tr_bencListAdd( uninitialized_path );
        tr_bencInitStrDup( sub, buf );

        prev = pch + 1;
        if (!*pch)
           break;
    }
}

static void
makeFilesList( benc_val_t                 * list,
               const tr_metainfo_builder_t  * builder )
{
    size_t i = 0;

    tr_bencListReserve( list, builder->fileCount );

    for( i=0; i<builder->fileCount; ++i )
    {
        benc_val_t * dict = tr_bencListAdd( list );
        benc_val_t *length, *pathVal;

        tr_bencInit( dict, TYPE_DICT );
        tr_bencDictReserve( dict, 2 );
        length = tr_bencDictAdd( dict, "length" );
        pathVal = tr_bencDictAdd( dict, "path" );
        getFileInfo( builder->top, builder->files[i], length, pathVal );
    }
}

static void
makeInfoDict ( benc_val_t             * dict,
               tr_metainfo_builder_t  * builder )
{
    uint8_t * pch;
    benc_val_t * val;
    char base[MAX_PATH_LENGTH];

    tr_bencDictReserve( dict, 5 );

    val = tr_bencDictAdd( dict, "name" );
    strlcpy( base, builder->top, sizeof( base ) );
    tr_bencInitStrDup ( val, basename( base ) );

    val = tr_bencDictAdd( dict, "piece length" );
    tr_bencInitInt( val, builder->pieceSize );

    pch = getHashInfo( builder );
    val = tr_bencDictAdd( dict, "pieces" );
    tr_bencInitStr( val, pch, SHA_DIGEST_LENGTH * builder->pieceCount, 0 );

    if ( builder->isSingleFile )
    {
        val = tr_bencDictAdd( dict, "length" );
        tr_bencInitInt( val, builder->fileLengths[0] );
    }
    else
    {
        val = tr_bencDictAdd( dict, "files" );
        tr_bencInit( val, TYPE_LIST );
        makeFilesList( val, builder );
    }

    val = tr_bencDictAdd( dict, "private" );
    tr_bencInitInt( val, builder->isPrivate ? 1 : 0 );
}

static void tr_realMakeMetaInfo ( tr_metainfo_builder_t * builder )
{
    int n = 5;
    benc_val_t top, *val;
fprintf( stderr, "builder %p in realMakeMetaInfo\n", builder );

    tr_bencInit ( &top, TYPE_DICT );
    if ( builder->comment && *builder->comment ) ++n;
    tr_bencDictReserve( &top, n );

        val = tr_bencDictAdd( &top, "announce" );
        tr_bencInitStrDup( val, builder->announce );

        val = tr_bencDictAdd( &top, "created by" );
        tr_bencInitStrDup( val, TR_NAME " " VERSION_STRING );

        val = tr_bencDictAdd( &top, "creation date" );
        tr_bencInitInt( val, time(0) );

        val = tr_bencDictAdd( &top, "encoding" );
        tr_bencInitStrDup( val, "UTF-8" );

        if( builder->comment && *builder->comment ) {
            val = tr_bencDictAdd( &top, "comment" );
            tr_bencInitStrDup( val, builder->comment );
        }

        val = tr_bencDictAdd( &top, "info" );
        tr_bencInit( val, TYPE_DICT );
        tr_bencDictReserve( val, 666 );
        makeInfoDict( val, builder );

    /* save the file */
    if (1) {
        FILE * fp = fopen( builder->outputFile, "wb+" );
        char * pch = tr_bencSaveMalloc( &top, &n );
        fwrite( pch, n, 1, fp );
        free( pch );
        fclose( fp );
    }

    /* cleanup */
    tr_bencFree( & top );
    builder->isDone = 1;
    builder->failed = builder->abortFlag; /* FIXME: doesn't catch all failures */
}

/***
****
****  A threaded builder queue
****
***/

static tr_metainfo_builder_t * queue = NULL;

static int workerIsRunning = 0;

static tr_thread_t workerThread;

static tr_lock_t* getQueueLock( tr_handle_t * h )
{
    static tr_lock_t * lock = NULL;

    tr_sharedLock( h->shared );
    if( lock == NULL )
    {
        lock = calloc( 1, sizeof( tr_lock_t ) );
        tr_lockInit( lock );
    }
    tr_sharedUnlock( h->shared );

    return lock;
}

static void workerFunc( void * user_data )
{
    tr_handle_t * handle = (tr_handle_t *) user_data;

    for (;;)
    {
        tr_metainfo_builder_t * builder = NULL;

        /* find the next builder to process */
        tr_lock_t * lock = getQueueLock ( handle );
        tr_lockLock( lock );
        if( queue != NULL ) {
            builder = queue;
            queue = queue->nextBuilder;
        }
        tr_lockUnlock( lock );

        /* if no builders, this worker thread is done */
        if( builder == NULL )
          break;

fprintf( stderr, "worker thread got builder %p\n", builder);
        tr_realMakeMetaInfo ( builder );
    }

fprintf( stderr, "worker thread exiting\n" );
    workerIsRunning = 0;
}

void
tr_makeMetaInfo( tr_metainfo_builder_t  * builder,
                 const char             * outputFile,
                 const char             * announce,
                 const char             * comment,
                 int                      isPrivate )
{
    tr_lock_t * lock;
    builder->announce = tr_strdup( announce );
    builder->comment = tr_strdup( comment );
    builder->isPrivate = isPrivate;
    if( outputFile && *outputFile )
        builder->outputFile = tr_strdup( outputFile );
    else {
        char out[MAX_PATH_LENGTH];
        snprintf( out, sizeof(out), "%s.torrent", builder->top);
        builder->outputFile = tr_strdup( out );
    }

fprintf( stderr, "enqueuing a builder %p\n", builder);
    /* enqueue the builder */
    lock = getQueueLock ( builder->handle );
    tr_lockLock( lock );
    builder->nextBuilder = queue;
    queue = builder;
    if( !workerIsRunning ) {
        workerIsRunning = 1;
fprintf( stderr, "making new worker thread\n" );
        tr_threadCreate( &workerThread, workerFunc, builder->handle, "makeMeta" );
    }
    tr_lockUnlock( lock );
}

