//
//  PiecesWindowController.m
//  Transmission
//
//  Created by Livingston on 9/23/06.
//  Copyright 2006 __MyCompanyName__. All rights reserved.
//

#import "PiecesWindowController.h"

#define MAX_ACROSS 18
#define BETWEEN 1.0

#define BLANK -99

@implementation PiecesWindowController

- (id) initWithWindowNibName: (NSString *) name
{
    if ((self = [super initWithWindowNibName: name]))
    {
        fTorrent = nil;
        int numPieces = MAX_ACROSS * MAX_ACROSS;
        fPieces = malloc(numPieces);
        int i;
        for (i = 0; i < numPieces; i++)
            fPieces[i] = BLANK;
        
        fBack = [NSImage imageNamed: @"PiecesBack.tiff"];
        NSSize size = [fBack size];
        
        fWhitePiece = [NSImage imageNamed: @"BoxWhite.tiff"];
        [fWhitePiece setScalesWhenResized: YES];
        [fWhitePiece setSize: size];
        
        fGreenPiece = [NSImage imageNamed: @"BoxGreen.tiff"];
        [fGreenPiece setScalesWhenResized: YES];
        [fGreenPiece setSize: size];
        
        fBlue1Piece = [NSImage imageNamed: @"BoxBlue1.tiff"];
        [fBlue1Piece setScalesWhenResized: YES];
        [fBlue1Piece setSize: size];
        
        fBlue2Piece = [NSImage imageNamed: @"BoxBlue2.tiff"];
        [fBlue2Piece setScalesWhenResized: YES];
        [fBlue2Piece setSize: size];
        
        fBlue3Piece = [NSImage imageNamed: @"BoxBlue3.tiff"];
        [fBlue3Piece setScalesWhenResized: YES];
        [fBlue3Piece setSize: size];
        
        fExistingImage = [fBack copy];
    }
    
    return self;
}

- (void) awakeFromNib
{
    //window location and size
    NSPanel * window = (NSPanel *)[self window];
    
    [window setBecomesKeyOnlyIfNeeded: YES];
    
    [window setFrameAutosaveName: @"PiecesWindowFrame"];
    [window setFrameUsingName: @"PiecesWindowFrame"];
}

- (void) dealloc
{
    free(fPieces);
    
    if (fTorrent)
        [fTorrent release];
    [fExistingImage release];
    [super dealloc];
}

- (void) setTorrent: (Torrent *) torrent
{
    if (fTorrent)
    {
        [fTorrent release];
        
        if (!torrent)
        {
            fTorrent = nil;
            [fImageView setImage: fBack];
        }
    }
    
    if (torrent)
    {
        fTorrent = [torrent retain];
        [self updateView: YES];
    }
}

- (void) updateView: (BOOL) first
{
    if (!fTorrent)
        return;
    
    if (first)
    {
        [fExistingImage release];
        fExistingImage = [fBack copy];
    }
    
    int numPieces = MAX_ACROSS * MAX_ACROSS;
    if ([fTorrent pieceCount] < numPieces)
        numPieces = [fTorrent pieceCount];
    
    int8_t * pieces = malloc(numPieces);
    [fTorrent getAvailability: pieces size: numPieces];
    
    int i, j, piece, across;
    float width;
    NSPoint point;
    NSRect rect;
    NSImage * pieceImage;
    BOOL change = NO;
        
    for (i = 0; i < numPieces; i++)
    {
        pieceImage = nil;
    
        if (i >= numPieces)
            break;
        
        piece = pieces[i];
        if (piece < 0)
        {
            if (first || fPieces[i] != -1)
            {
                fPieces[i] = -1;
                pieceImage = fGreenPiece;
            }
        }
        else if (piece == 0)
        {
            if (first || fPieces[i] != 0)
            {
                fPieces[i] = 0;
                pieceImage = fWhitePiece;
            }
        }
        else if (piece == 1)
        {
            if (first || fPieces[i] != 1)
            {
                fPieces[i] = 1;
                pieceImage = fBlue1Piece;
            }
        }
        else if (piece == 2)
        {
            if (first || fPieces[i] != 2)
            {
                fPieces[i] = 2;
                pieceImage = fBlue2Piece;
            }
        }
        else
        {
            if (first || fPieces[i] != 3)
            {
                fPieces[i] = 3;
                pieceImage = fBlue3Piece;
            }
        }
        
        if (pieceImage)
        {
            //drawing actually will occur, so figure out values
            if (!change)
            {
                //determine how many boxes and sizes
                if (numPieces < MAX_ACROSS * MAX_ACROSS)
                {
                    across = sqrt(numPieces);
                    if (across * across < numPieces)
                        across++;
                }
                else
                    across = MAX_ACROSS;
                
                width = ([fExistingImage size].width - (float)(across + 1) * BETWEEN) / (float)across;
                rect = NSMakeRect(0, 0, width, width);
                
                [fExistingImage lockFocus];
                change = YES;
            }
            
            int numAcross = i % across, numDown = i / across;
            point = NSMakePoint((float)numAcross * (width + BETWEEN) + BETWEEN,
                                (float)(across - numDown) * (width + BETWEEN) - width);
            [pieceImage compositeToPoint: point fromRect: rect operation: NSCompositeSourceOver];
        }
    }
    
    if (change)
    {
        [fExistingImage unlockFocus];
        
        [fImageView setImage: nil];
        [fImageView setImage: fExistingImage];
    }
    
    free(pieces);
}

@end
