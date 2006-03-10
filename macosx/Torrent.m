/******************************************************************************
 * Copyright (c) 2005-2006 Transmission authors and contributors
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

#import "Torrent.h"

@implementation Torrent

- (id) initWithPath: (NSString *) path lib: (tr_handle_t *) lib
{
    fLib = lib;

    int error;
    fHandle = tr_torrentInit( fLib, [path UTF8String], &error );
    if( !fHandle )
    {
        [self release];
        return nil;
    }

    fInfo = tr_torrentInfo( fHandle );
    NSString * fileType = ( fInfo->fileCount > 1 ) ?
        NSFileTypeForHFSTypeCode('fldr') : [[self name] pathExtension];
    fIcon = [[NSWorkspace sharedWorkspace] iconForFileType: fileType];

    [self update];
    return self;
}

- (void) dealloc
{
    if( fHandle )
    {
        tr_torrentClose( fLib, fHandle );
    }
    [super dealloc];
}

- (void) setFolder: (NSString *) path
{
    tr_torrentSetFolder( fHandle, [path UTF8String] );
}

- (NSString *) getFolder
{
    return [NSString stringWithUTF8String: tr_torrentGetFolder( fHandle )];
}

- (void) update
{
    fStat = tr_torrentStat( fHandle );

    fStatusString = @"";
    fInfoString   = @"";

    switch( fStat->status )
    {
        case TR_STATUS_PAUSE:
            fStatusString = [NSString stringWithFormat:
                @"Paused (%.2f %%)", 100 * fStat->progress];
            break;

        case TR_STATUS_CHECK:
            fStatusString = [NSString stringWithFormat: 
                @"Checking existing files (%.2f %%)", 100 * fStat->progress];
            break;

        case TR_STATUS_DOWNLOAD:
            if( fStat->eta < 0 )
            {
                fStatusString = [NSString stringWithFormat:
                    @"Finishing in --:--:-- (%.2f %%)", 100 * fStat->progress];
            }
            else
            {
                fStatusString = [NSString stringWithFormat:
                    @"Finishing in %02d:%02d:%02d (%.2f %%)",
                    fStat->eta / 3600, ( fStat->eta / 60 ) % 60,
                    fStat->eta % 60, 100 * fStat->progress];
            }
            fInfoString = [NSString stringWithFormat:
                @"Downloading from %d of %d peer%s",
                fStat->peersUploading, fStat->peersTotal,
                ( fStat->peersTotal == 1 ) ? "" : "s"];
            break;

        case TR_STATUS_SEED:
            fStatusString  = [NSString stringWithFormat:
                @"Seeding, uploading to %d of %d peer%s",
                fStat->peersDownloading, fStat->peersTotal,
                ( fStat->peersTotal == 1 ) ? "" : "s"];
            break;

        case TR_STATUS_STOPPING:
            fStatusString  = @"Stopping...";
            break;
    }

#if 0
    if( ( stat->status & ( TR_STATUS_DOWNLOAD | TR_STATUS_SEED ) ) &&
        ( stat->status & TR_TRACKER_ERROR ) )
    {
        fPeersString = [NSString stringWithFormat: @"%@%@",
            @"Error: ", [NSString stringWithUTF8String: stat->error]];
    }
#endif

    tr_torrentAvailability( fHandle, fPieces, 120 );
}

- (void) start
{
    if( fStat->status & TR_STATUS_INACTIVE )
    {
        tr_torrentStart( fHandle );
    }
}

- (void) stop
{
    if( fStat->status & TR_STATUS_ACTIVE )
    {
        tr_torrentStop( fHandle );
    }
}

- (void) sleep
{
    if( fStat->status & TR_STATUS_ACTIVE )
    {
        [self stop];
        fResumeOnWake = YES;
    }
    else
    {
        fResumeOnWake = NO;
    }
}

- (void) wakeUp
{
    if( fResumeOnWake )
    {
        [self start];
    }
}

- (NSImage *) icon
{
    return fIcon;
}

- (NSString *) path
{
    return [NSString stringWithUTF8String: fInfo->torrent];
}

- (NSString *) name
{
    return [NSString stringWithUTF8String: fInfo->name];
}

- (uint64_t) size
{
    return fInfo->totalSize;
}

- (BOOL) isActive
{
    return ( fStat->status & TR_STATUS_ACTIVE );
}

- (BOOL) isPaused
{
    return ( fStat->status == TR_STATUS_PAUSE );
}

- (BOOL) justFinished
{
    return tr_getFinished( fHandle );
}

- (NSString *) statusString
{
    return fStatusString;
}

- (NSString *) infoString
{
    return fInfoString;
}

@end
