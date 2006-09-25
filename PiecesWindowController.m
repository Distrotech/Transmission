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
    
    int numPieces = MAX_ACROSS * MAX_ACROSS, across;
    if ([fTorrent pieceCount] < numPieces)
    {
        numPieces = [fTorrent pieceCount];
        
        //determine how many boxes
        across = sqrt(numPieces);
        if (across * across < numPieces)
            across++;
    }
    else
        across = MAX_ACROSS;
    
    float width = ([fExistingImage size].width - (float)(across + 1) * BETWEEN) / (float)across;
    
    int8_t * pieces = malloc(numPieces);
    [fTorrent getAvailability: pieces size: numPieces];
    
    int i, j, piece, index = -1;
    NSPoint point;
    NSRect rect = NSMakeRect(0, 0, width, width);
    NSImage * pieceImage;
    BOOL change = NO;
    
    for (i = 0; i < across; i++)
        for (j = 0; j < across; j++)
        {
            pieceImage = nil;
        
            index++;
            if (index >= numPieces)
                break;
            
            piece = pieces[index];
            if (piece < 0)
            {
                if (first || fPieces[index] != -1)
                {
                    fPieces[index] = -1;
                    pieceImage = fGreenPiece;
                }
            }
            else if (piece == 0)
            {
                if (first || fPieces[index] != 0)
                {
                    fPieces[index] = 0;
                    pieceImage = fWhitePiece;
                }
            }
            else if (piece == 1)
            {
                if (first || fPieces[index] != 1)
                {
                    fPieces[index] = 1;
                    pieceImage = fBlue1Piece;
                }
            }
            else if (piece == 2)
            {
                if (first || fPieces[index] != 2)
                {
                    fPieces[index] = 2;
                    pieceImage = fBlue2Piece;
                }
            }
            else
            {
                if (first || fPieces[index] != 3)
                {
                    fPieces[index] = 3;
                    pieceImage = fBlue3Piece;
                }
            }
            
            if (pieceImage)
            {
                if (!change)
                {
                    [fExistingImage lockFocus];
                    change = YES;
                }
                
                point = NSMakePoint((float)j * (width + BETWEEN) + BETWEEN, (float)(across - i) * (width + BETWEEN) - width);
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
