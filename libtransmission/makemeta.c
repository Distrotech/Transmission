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
    char buf[MAX_PATH_LENGTH];
    struct stat sb;
    sb.st_size = 0;

    snprintf( buf, sizeof(buf), "%s"TR_PATH_DELIMITER_STR"%s", dir, base );
    stat( buf, &sb );

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
        tr_dbg( "putting absolute path in list [%s]\n", node->filename );
        node->next = list;
        list = node;
    }

    return list;
}

static int
getListSize( const struct FileList * list )
{
    int i;
    for( i=0; list!=NULL; list=list->next )
        ++i;
    return i;
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

/****
*****
****/

static off_t
getFileSize ( const char * filename )
{
    struct stat sb;
    sb.st_size = 0;
    stat( filename, &sb );
    return sb.st_size;
}

static uint8_t*
getHashInfo ( const char              * topFile,
              const struct FileList   * files,
              int                       pieceSize,
              int                     * setmeCount )
{
    int i;
    tr_torrent_t t;
    uint8_t *ret, *walk;
    const struct FileList * fwalk;
    const size_t topLen = strlen(topFile) + 1; /* +1 for '/' */

    /* build a mock tr_torrent_t that we can feed to tr_ioRecalculateHash() */  
    memset( &t, 0, sizeof( tr_torrent_t ) );
    tr_lockInit ( &t.lock );
    t.destination = (char*) topFile;
    t.info.fileCount = getListSize( files );
    t.info.files = calloc( t.info.fileCount, sizeof( tr_file_t ) );
    
    for( fwalk=files, i=0; fwalk!=NULL; fwalk=fwalk->next, ++i ) {
        tr_file_t * file = &t.info.files[i];
        file->length = getFileSize( fwalk->filename );
        strlcpy( file->name, fwalk->filename + topLen, sizeof( file->name ) );
        t.info.totalSize += file->length;
    }
    t.info.pieceSize = pieceSize;
    t.info.pieceCount = t.info.totalSize / pieceSize;
    if ( t.info.totalSize % pieceSize )
        ++t.info.pieceCount;
    t.info.pieces = calloc( t.info.pieceCount, sizeof( tr_piece_t ) );
    tr_torrentInitFilePieces( &t );

    ret = (uint8_t*) malloc ( SHA_DIGEST_LENGTH * t.info.pieceCount );
    walk = ret;
    for( i=0; i<t.info.pieceCount; ++i ) {
        tr_ioRecalculateHash( &t, i, walk );
        walk += SHA_DIGEST_LENGTH;
    }
    assert( walk-ret == SHA_DIGEST_LENGTH*t.info.pieceCount );

    *setmeCount = t.info.pieceCount;
    tr_lockClose ( &t.lock );
    free( t.info.pieces );
    free( t.info.files );
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
        fprintf ( stderr, "adding [%s] to the list of paths\n", buf );

        sub = tr_bencListAdd( uninitialized_path );
        tr_bencInitStrDup( sub, buf );

        prev = pch + 1;
        if (!*pch)
           break;
    }
}

static void
makeFilesList( benc_val_t            * list,
               const char            * topFile,
               const struct FileList * files )
{
    const struct FileList * walk;

    tr_bencListReserve( list, getListSize(files) );

    for( walk=files; walk!=NULL; walk=walk->next )
    {
        benc_val_t * dict = tr_bencListAdd( list );
        benc_val_t *length, *pathVal;

        tr_bencInit( dict, TYPE_DICT );
        tr_bencDictReserve( dict, 2 );
        length = tr_bencDictAdd( dict, "length" );
        pathVal = tr_bencDictAdd( dict, "path" );
        getFileInfo( topFile, walk->filename, length, pathVal );
    }
}

static void
makeInfoDict ( benc_val_t              * dict,
               const char              * topFile,
               const struct FileList   * files,
               int                       isPrivate )
{
    static const int pieceSize = 262144; /* 256 KiB. TODO: let user choose? */
    const int single = files->next == NULL;
    uint8_t * pch;
    int pieceCount = 0;
    benc_val_t * val;
    char base[MAX_PATH_LENGTH];

    tr_bencDictReserve( dict, 5 );

    val = tr_bencDictAdd( dict, "name" );
    strlcpy( base, topFile, sizeof( base ) );
    tr_bencInitStrDup ( val, basename( base ) );

    val = tr_bencDictAdd( dict, "piece length" );
    tr_bencInitInt( val, pieceSize );

    pch = getHashInfo( topFile, files, pieceSize, &pieceCount );
    val = tr_bencDictAdd( dict, "pieces" );
    tr_bencInitStr( val, pch, SHA_DIGEST_LENGTH * pieceCount, 0 );

    if ( single )
    {
        val = tr_bencDictAdd( dict, "length" );
        tr_bencInitInt( val, getFileSize(files->filename) );
    }
    else
    {
        val = tr_bencDictAdd( dict, "files" );
        tr_bencInit( val, TYPE_LIST );
        makeFilesList( val, topFile, files );
    }

    val = tr_bencDictAdd( dict, "private" );
    tr_bencInitInt( val, isPrivate ? 1 : 0 );
}

int
tr_makeMetaInfo( const char   * outputFile,
                 const char   * announce,
                 const char   * comment,
                 const char   * topFile,
                 int            isPrivate )
{
    int n = 5;
    benc_val_t top, *val;
    struct FileList *files;

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
        assert( files != NULL  );
    }

    tr_bencInit ( &top, TYPE_DICT );
    if ( comment && *comment ) ++n;
    tr_bencDictReserve( &top, n );

        val = tr_bencDictAdd( &top, "announce" );
        tr_bencInitStrDup( val, announce );

        val = tr_bencDictAdd( &top, "created by" );
        tr_bencInitStrDup( val, "Transmission " VERSION_STRING );

        val = tr_bencDictAdd( &top, "creation date" );
        tr_bencInitInt( val, time(0) );

        val = tr_bencDictAdd( &top, "encoding" );
        tr_bencInitStrDup( val, "UTF-8" );

        if( comment && *comment ) {
            val = tr_bencDictAdd( &top, "comment" );
            tr_bencInitStrDup( val, comment );
        }

        val = tr_bencDictAdd( &top, "info" );
        tr_bencInit( val, TYPE_DICT );
        tr_bencDictReserve( val, 666 );
        makeInfoDict( val, topFile, files, isPrivate );

    /* debugging... */
    tr_bencPrint( &top );

    /* save the file */
    if (1) {
        FILE * fp = fopen( outputFile, "wb+" );
        char * pch = tr_bencSaveMalloc( &top, &n );
        fwrite( pch, n, 1, fp );
        free( pch );
        fclose( fp );
    }

    /* cleanup */
    freeFileList( files );
    tr_bencFree( & top );
    return 0;
}
