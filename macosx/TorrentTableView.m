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

#import "TorrentTableView.h"
#import "Controller.h"
#import "Torrent.h"

@implementation TorrentTableView

- (void) setTorrents: (NSArray *) torrents
{
    fTorrents = torrents;
}

- (void) pauseOrResume: (int) row
{
#if 0
    if( fStat[row].status & TR_STATUS_PAUSE )
    {
        [fController resumeTorrentWithIndex: row];
    }
    else if( fStat[row].status & ( TR_STATUS_CHECK |
              TR_STATUS_DOWNLOAD | TR_STATUS_SEED ) )
    {
        [fController stopTorrentWithIndex: row];
    }                                                                   
#endif
}

- (void) mouseDown: (NSEvent *) e
{
    fClickPoint = [self convertPoint: [e locationInWindow] fromView: NULL];
    [self display];
}

- (NSRect) pauseRectForRow: (int) row
{
    int col;
    NSRect cellRect, rect;

    col      = [self columnWithIdentifier: @"Torrent"];
    cellRect = [self frameOfCellAtColumn: col row: row];
    rect     = NSMakeRect( cellRect.origin.x + cellRect.size.width - 39,
                           cellRect.origin.y + 3, 14, 14 );

    return rect;
}

- (NSRect) revealRectForRow: (int) row
{
    int col;
    NSRect cellRect, rect;

    col      = [self columnWithIdentifier: @"Torrent"];
    cellRect = [self frameOfCellAtColumn: col row: row];
    rect     = NSMakeRect( cellRect.origin.x + cellRect.size.width - 20,
                           cellRect.origin.y + 3, 14, 14 );

    return rect;
}

- (BOOL) pointInPauseRect: (NSPoint) point
{
    return NSPointInRect( point, [self pauseRectForRow:
                                    [self rowAtPoint: point]] );
}

- (BOOL) pointInRevealRect: (NSPoint) point
{
    return NSPointInRect( point, [self revealRectForRow:
                                    [self rowAtPoint: point]] );
}

- (void) mouseUp: (NSEvent *) e
{
    NSPoint point;
    int row, col;
    Torrent * torrent;

    point = [self convertPoint: [e locationInWindow] fromView: NULL];
    row   = [self rowAtPoint: point];
    col   = [self columnAtPoint: point];

    if( row < 0 )
    {
        [self deselectAll: NULL];
    }
    else if( [self pointInPauseRect: point] )
    {
        [self pauseOrResume: row];
    }
    else if( [self pointInRevealRect: point] )
    {
        torrent = [fTorrents objectAtIndex: row];
        [torrent reveal];
        [self display];
    }
    else if( row >= 0 && ( [e modifierFlags] & NSAlternateKeyMask ) )
    {
        [fController advancedChanged: self];
    }
    else
    {
        [self selectRowIndexes: [NSIndexSet indexSetWithIndex: row]
            byExtendingSelection: NO];
    }

    fClickPoint = NSMakePoint( 0, 0 );
}

- (NSMenu *) menuForEvent: (NSEvent *) e
{
    NSPoint point;
    int row;

    point = [self convertPoint: [e locationInWindow] fromView: NULL];
    row = [self rowAtPoint: point];
    
    [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];

    return row >= 0 ? fContextRow : fContextNoRow;
}

- (void) drawRect: (NSRect) r
{
    int i;
    NSRect rect;
    NSPoint point;
    NSImage * image;

    [super drawRect: r];

    for( i = 0; i < [self numberOfRows]; i++ )
    {
        rect  = [self pauseRectForRow: i];
        image = NULL;
#if 0
        if( fStat[i].status & TR_STATUS_PAUSE )
        {
#endif
            image = NSPointInRect( fClickPoint, rect ) ?
                [NSImage imageNamed: @"ResumeOn.png"] :
                [NSImage imageNamed: @"ResumeOff.png"];
#if 0
        }
        else if( fStat[i].status &
                 ( TR_STATUS_CHECK | TR_STATUS_DOWNLOAD | TR_STATUS_SEED ) )
        {
            image = NSPointInRect( fClickPoint, rect ) ?
                [NSImage imageNamed: @"PauseOn.png"] :
                [NSImage imageNamed: @"PauseOff.png"];
        }
#endif
        if( image )
        {
            [image setFlipped: YES];
            [image drawAtPoint: rect.origin fromRect:
                NSMakeRect( 0, 0, 14, 14 ) operation:
                NSCompositeSourceOver fraction: 1.0];
        }

        rect  = [self revealRectForRow: i];
        image = NSPointInRect( fClickPoint, rect ) ?
            [NSImage imageNamed: @"RevealOn.png"] :
            [NSImage imageNamed: @"RevealOff.png"];
        [image setFlipped: YES];
        [image drawAtPoint: rect.origin fromRect:
            NSMakeRect( 0, 0, 14, 14 ) operation:
            NSCompositeSourceOver fraction: 1.0];
    }
}

@end
