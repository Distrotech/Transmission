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

#ifndef TR_MAKEMETA_H
#define TR_MAKEMETA_H 1

typedef struct tr_metainfo_builder_s
{
    /**
    ***  These are set by tr_makeMetaInfo()
    ***  and cleaned up by tr_metaInfoBuilderFree()
    **/

    char * top;
    char ** files;
    size_t * fileLengths;
    size_t totalSize;
    size_t fileCount;
    size_t pieceSize;
    size_t pieceCount;
    int isSingleFile;
    tr_handle_t * handle;

    /**
    ***  These are set inside tr_makeMetaInfo()
    ***  by copying the arguments passed to it,
    ***  and cleaned up by tr_metaInfoBuilderFree()
    **/

    char * announce;
    char * comment;
    char * outputFile;
    int isPrivate;

    /**
    ***  These are set inside tr_makeMetaInfo() so the client
    ***  can poll periodically to see what the status is.
    ***  The client can also set abortFlag to nonzero to
    ***  tell tr_makeMetaInfo() to abort and clean up after itself.
    **/

    size_t pieceIndex;
    int abortFlag;
    int isDone;
    int failed; /* only meaningful if isDone is set */

    /**
    ***  This is an implementation detail.
    ***  The client should never use these fields.
    **/

    struct tr_metainfo_builder_s * nextBuilder;
}
tr_metainfo_builder_t;




tr_metainfo_builder_t*
tr_metaInfoBuilderCreate( tr_handle_t  * handle,
                          const char   * topFile );

void
tr_metaInfoBuilderFree( tr_metainfo_builder_t* );

/**
 * 'outputFile' if NULL, builder->top + ".torrent" will be used.
 *
 * This is actually done in a worker thread, not the main thread!
 * Otherwise the client's interface would lock up while this runs.
 *
 * It is the caller's responsibility to poll builder->isDone
 * from time to time!  When the worker thread sets that flag,
 * the caller must pass the builder to tr_metaInfoBuilderFree().
 */
void
tr_makeMetaInfo( tr_metainfo_builder_t  * builder,
                 const char             * outputFile,
                 const char             * announce,
                 const char             * comment,
                 int                      isPrivate );


#endif
