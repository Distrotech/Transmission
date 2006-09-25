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
        
        //determine relevant values
        fNumPieces = MAX_ACROSS * MAX_ACROSS;
        if ([fTorrent pieceCount] < fNumPieces)
        {
            fNumPieces = [fTorrent pieceCount];
            
            fAcross = sqrt(fNumPieces);
            if (fAcross * fAcross < fNumPieces)
                fAcross++;
        }
        else
            fAcross = MAX_ACROSS;
        
        fWidth = ([fExistingImage size].width - (float)(fAcross + 1) * BETWEEN) / (float)fAcross;
        
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
    
    int8_t * pieces = malloc(fNumPieces);
    [fTorrent getAvailability: pieces size: fNumPieces];
    
    int i, j, piece;
    NSPoint point;
    NSRect rect = NSMakeRect(0, 0, fWidth, fWidth);
    NSImage * pieceImage;
    BOOL change = NO;
        
    for (i = 0; i < fNumPieces; i++)
    {
        pieceImage = nil;
        
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
                [fExistingImage lockFocus];
                change = YES;
            }
            
            int numAcross = i % fAcross, numDown = i / fAcross;
            point = NSMakePoint((float)numAcross * (fWidth + BETWEEN) + BETWEEN,
                                (float)(fAcross - numDown) * (fWidth + BETWEEN) - fWidth);
            
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
