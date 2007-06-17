/*
 * This file Copyright (C) 2007 Charles Kerr <charles@rebelbase.com>
 *
 * This file is available under the
 * Attribution-Noncommercial-Share Alike 3.0 License.
 * Overview:  http://creativecommons.org/licenses/by-nc-sa/3.0/
 * Full text: http://creativecommons.org/licenses/by-nc-sa/3.0/legalcode
 *
 * Loosely speaking, you're free to use and adapt this work
 * provided that it's for noncommercial uses and that you share your
 * changes under a compatible license.
 */

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
