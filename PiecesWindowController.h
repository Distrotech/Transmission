//
//  PiecesWindowController.h
//  Transmission
//
//  Created by Livingston on 9/23/06.
//  Copyright 2006 __MyCompanyName__. All rights reserved.
//

#import <Cocoa/Cocoa.h>
#import "Torrent.h"

@interface PiecesWindowController : NSWindowController
{
    int8_t * fPieces;
    
    NSImage * fExistingImage, * fBack, * fWhitePiece, * fGreenPiece,
            * fBlue1Piece, * fBlue2Piece, * fBlue3Piece;
    
    Torrent * fTorrent;
    NSTimer * fTimer;
    
    IBOutlet NSImageView * fImageView;
}

- (void) setTorrent: (Torrent *) torrent;

//- (void) viewFirst;
- (void) updateView: (NSTimer *) timer;

@end
