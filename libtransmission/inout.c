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

#include <stdlib.h> /* for calloc */
#include "transmission.h"

struct tr_io_s
{
    tr_torrent_t * tor;

    tr_bitfield_t * uncheckedPieces;
};

#include "fastresume.h"

/****
*****  Low-level IO functions
****/

enum { TR_IO_READ, TR_IO_WRITE };

static int
readOrWriteBytes ( const tr_io_t    * io,
                   int                ioMode,
                   int                fileIndex,
                   size_t             fileOffset,
                   void             * buf,
                   size_t             buflen )
{
    const tr_info_t * info = &io->tor->info;
    const tr_file_t * file = &info->files[fileIndex];
    int fd, ret;
    typedef size_t (* iofunc) ( int, void *, size_t );
    iofunc func = ioMode == TR_IO_READ ? (iofunc)read : (iofunc)write;

    assert ( 0<=fileIndex && fileIndex<info->fileCount );
    assert ( !file->length || (fileOffset < file->length));
    assert ( fileOffset + buflen <= file->length );

    if( !file->length )
        return 0;
    else if ((fd = tr_fdFileOpen ( io->tor->destination, file->name, TRUE )) < 0)
        ret = fd;
    else if( lseek( fd, fileOffset, SEEK_SET ) == ((off_t)-1) )
        ret = TR_ERROR_IO_OTHER;
    else if( func( fd, buf, buflen ) != buflen )
        ret = tr_ioErrorFromErrno ();
    else
        ret = TR_OK;
 
    if( fd >= 0 )
        tr_fdFileRelease( fd );

    return ret;
}

static void
findFileLocation ( const tr_io_t * io,
                   int pieceIndex, size_t pieceOffset,
                   int * fileIndex, size_t * fileOffset )
{
    const tr_torrent_t * tor = io->tor;
    const tr_info_t * info = &tor->info;

    int i;
    uint64_t piecePos = ((uint64_t)pieceIndex * info->pieceSize) + pieceOffset;

    assert ( 0<=pieceIndex && pieceIndex < info->pieceCount );
    assert ( pieceOffset < (size_t)tr_pieceSize(pieceIndex) );
    assert ( piecePos < info->totalSize );

    for ( i=0; info->files[i].length<=piecePos; ++i )
      piecePos -= info->files[i].length;

    *fileIndex = i;
    *fileOffset = piecePos;

    assert ( 0<=*fileIndex && *fileIndex<info->fileCount );
    assert ( *fileOffset < info->files[i].length );
}

static int
ensureMinimumFileSize ( const tr_io_t   * io,
                        int               fileIndex,
                        size_t            minSize ) /* in bytes */
{
    int fd;
    int ret;
    struct stat sb;
    const tr_torrent_t * tor = io->tor;
    const tr_file_t * file = &tor->info.files[fileIndex];

    assert ( 0<=fileIndex && fileIndex<tor->info.fileCount );
    assert ( minSize <= file->length );

    fd = tr_fdFileOpen( tor->destination, file->name, TRUE );
    if( fd < 0 ) /* bad fd */
        ret = fd;
    else if (fstat (fd, &sb) ) /* how big is the file? */
        ret = tr_ioErrorFromErrno ();
    else if ((size_t)sb.st_size >= minSize) /* already big enough */
        ret = TR_OK;
    else if (!ftruncate( fd, minSize )) /* grow it */
        ret = TR_OK;
    else /* couldn't grow it */
        ret = tr_ioErrorFromErrno ();

    if( fd >= 0 )
        tr_fdFileRelease( fd );

    return ret;
}

static int
readOrWritePiece ( const tr_io_t   * io,
                   int               ioMode,
                   int               pieceIndex,
                   size_t            pieceOffset,
                   uint8_t         * buf,
                   size_t            buflen )
{
    int ret = 0;
    int fileIndex;
    size_t fileOffset;
    tr_torrent_t * tor = io->tor;
    const tr_info_t * info = &tor->info;

    assert( io != NULL );
    assert( 0<=pieceIndex && pieceIndex<io->tor->info.pieceCount );
    assert( buflen <= (size_t) tr_pieceSize( pieceIndex ) );

    /* Release the torrent lock so the UI can still update itself if
       this blocks for a while */
    tr_lockUnlock( &tor->lock );

    findFileLocation ( io, pieceIndex, pieceOffset, &fileIndex, &fileOffset );

    while( buflen && !ret )
    {
        const tr_file_t * file = &info->files[fileIndex];
        const size_t bytesThisPass = MIN( buflen, file->length - fileOffset );

        if( ioMode == TR_IO_WRITE )
            ret = ensureMinimumFileSize( io, fileIndex,
                                         fileOffset + bytesThisPass );
        if( !ret )
            ret = readOrWriteBytes( io, ioMode,
                                    fileIndex, fileOffset, buf, bytesThisPass );
        buf += bytesThisPass;
        buflen -= bytesThisPass;
        fileIndex++;
        fileOffset = 0;
    }

    tr_lockLock( &tor->lock );

    return ret;
}

int
tr_ioRead( tr_io_t * io, int pieceIndex, int begin, int len, uint8_t * buf )
{
    return readOrWritePiece ( io, TR_IO_READ, pieceIndex, begin, buf, len );
}

int
tr_ioWrite( tr_io_t * io, int pieceIndex, int begin, int len, uint8_t * buf )
{
    return readOrWritePiece ( io, TR_IO_WRITE, pieceIndex, begin, buf, len );
}

/****
*****
****/

static int
checkPiece ( tr_io_t * io, int pieceIndex )
{
    int n;
    int ret;
    uint8_t * buf;
    const tr_torrent_t * tor;
    const tr_info_t * info;

    assert( io != NULL );
    assert( io->tor != NULL );
    assert( 0<=pieceIndex && pieceIndex<io->tor->info.pieceCount );

    tor = io->tor;
    info = &tor->info;
    n = tr_pieceSize( pieceIndex );

    buf = malloc( n );
    ret = tr_ioRead( io, pieceIndex, 0, n, buf );
    if( !ret )
    {
        uint8_t hash[SHA_DIGEST_LENGTH];
        SHA1( buf, n, hash );
        ret = memcmp( hash, info->pieces[pieceIndex].hash, SHA_DIGEST_LENGTH );
    }
    free( buf );

    tr_dbg ("torrent [%s] piece %d hash check: %s",
            tor->info.name, pieceIndex, (ret?"FAILED":"OK"));
    return ret;
}

static void
checkFiles( tr_io_t * io )
{
    int i;
    const tr_torrent_t * tor = io->tor;

    tr_bitfieldClear( io->uncheckedPieces );

    if ( fastResumeLoad( io->tor, io->uncheckedPieces ) )
        tr_bitfieldAddRange( io->uncheckedPieces, 0, tor->info.pieceCount-1 );

    for( i=0; i<tor->info.pieceCount; ++i ) 
    {
        if( tor->status & TR_STATUS_STOPPING )
            break;

        if( !tr_bitfieldHas( io->uncheckedPieces, i ) )
            continue;

        tr_inf ( "Checking piece %d because it's not in fast-resume", i );

        if( !checkPiece( io, i ) )
           tr_cpPieceAdd( tor->completion, i );
        else
           tr_cpPieceRem( tor->completion, i );

        tr_bitfieldRem( io->uncheckedPieces, i );
    }
}

/****
*****  Life Cycle
****/

tr_io_t*
tr_ioInit( tr_torrent_t * tor )
{
    tr_io_t * io = calloc( 1, sizeof( tr_io_t ) );
    io->uncheckedPieces = tr_bitfieldNew( tor->info.pieceCount );
    io->tor = tor;
    checkFiles( io );
    return io;
}

void
tr_ioSync( tr_io_t * io )
{
    int i;
    const tr_info_t * info = &io->tor->info;

    for( i=0; i<info->fileCount; ++i )
        tr_fdFileClose( io->tor->destination, info->files[i].name );

    if( tr_bitfieldIsEmpty( io->uncheckedPieces ) )
        fastResumeSave( io->tor );
}

void
tr_ioClose( tr_io_t * io )
{
    tr_ioSync( io );

    tr_bitfieldFree( io->uncheckedPieces );
    free( io );
}


/* try to load the fast resume file */
void
tr_ioLoadResume( tr_torrent_t * tor )
{
    tr_io_t * io = calloc( 1, sizeof( tr_io_t ) );
    io->uncheckedPieces = tr_bitfieldNew( tor->info.pieceCount );
    io->tor = tor;
    fastResumeLoad( tor, io->uncheckedPieces );
    tor->ioLoaded = 1;
    tr_bitfieldFree( io->uncheckedPieces );
    free( io );
}

void tr_ioRemoveResume( tr_torrent_t * tor )
{
    if( !tor->io )
        fastResumeRemove( tor );
}

int
tr_ioHash( tr_io_t * io, int pieceIndex )
{
    int i;

    tr_torrent_t * tor = io->tor;
    const int success = !checkPiece( io, pieceIndex );
    if( success )
    {
        tr_inf( "Piece %d hash OK", pieceIndex );
        tr_cpPieceAdd( tor->completion, pieceIndex );
    }
    else
    {
        tr_err( "Piece %d hash FAILED", pieceIndex );
        tr_cpPieceRem( tor->completion, pieceIndex );
    }

    /* Assign blame or credit to peers */
    for( i = 0; i < tor->peerCount; ++i )
        tr_peerBlame( tor->peers[i], pieceIndex, success );

    return 0;
}
