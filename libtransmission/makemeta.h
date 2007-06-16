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

typedef struct
{
    char * top;
    char ** files;
    size_t * fileLengths;
    size_t totalSize;
    size_t fileCount;
    size_t pieceSize;
    size_t pieceCount;
    int isSingleFile;
}
meta_info_builder_t;

meta_info_builder_t*
tr_metaInfoBuilderCreate( const char * topFile );

/** 
 * Called periodically during the checksum generation.
 *
 * 'builder' is the builder passed into tr_makeMetaInfo
 * 'pieceIndex' is the current piece having a checksum generated
 * 'abortFlag' is an int pointer to set if the user wants to abort
 * 'userData' is the data passed into tr_makeMetaInfo
 */
typedef
void (*makemeta_progress_func)(const meta_info_builder_t * builder,
                               size_t                      pieceIndex,
                               int                       * abortFlag,
                               void                      * userData );

/**
 * Builds a .torrent metainfo file.
 *
 * 'outputFile' if NULL, builder->top + ".torrent" will be used.
 * 'progress_func' a client-implemented callback function (see above)
 * 'progress_func_user_data' is passed back to the user in the progress func.
 *     It can be used to pass a resource or handle from tr_makeMetaInfo's
 *     caller to progress_func, or anything else.  Pass NULL if not needed.
 */
int
tr_makeMetaInfo( const meta_info_builder_t  * builder,
                 makemeta_progress_func       progress_func,
                 void                       * progress_func_user_data,
                 const char                 * outputFile,
                 const char                 * announce,
                 const char                 * comment,
                 int                          isPrivate );


void
tr_metaInfoBuilderFree( meta_info_builder_t* );

#endif
