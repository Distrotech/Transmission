/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2007 Transmission authors and contributors
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

#import "CreatorWindowController.h"
#import "StringAdditions.h"

#define DEFAULT_SAVE_LOCATION @"~/Desktop/"

#warning rename?
@interface CreatorWindowController (Private)

+ (NSString *) chooseFile;
- (void) setPath: (NSString *) path;
- (void) locationSheetClosed: (NSOpenPanel *) openPanel returnCode: (int) code contextInfo: (void *) info;

@end

@implementation CreatorWindowController

+ (void) createTorrentFile
{
    //get file/folder for torrent
    NSString * path;
    if (!(path = [CreatorWindowController chooseFile]))
        return;
    
    CreatorWindowController * creator = [[self alloc] initWithWindowNibName: @"Creator"];
    [creator setPath: path];
    [creator showWindow: nil];
}

- (void) awakeFromNib
{
    fInfo = tr_metaInfoBuilderCreate([fPath UTF8String]);
    
    #warning if count == 0, end
    
    NSString * name = [fPath lastPathComponent];
    
    [[self window] setTitle: name];
    
    [fNameField setStringValue: name];
    [fNameField setToolTip: fPath];
    
    BOOL multifile = !fInfo->isSingleFile;
    
    NSImage * icon = [[[NSWorkspace sharedWorkspace] iconForFileType: multifile
                        ? NSFileTypeForHFSTypeCode('fldr') : [fPath pathExtension]] copy];
    [fIcon setImage: icon];
    [icon release];
    
    NSString * statusString = [NSString stringForFileSize: fInfo->totalSize];
    if (multifile)
    {
        NSString * fileString;
    
        int count = fInfo->fileCount;
        if (count != 1)
            fileString = [NSString stringWithFormat: NSLocalizedString(@"%d Files, ", "Create torrent -> info"), count];
        else
            fileString = NSLocalizedString(@"1 File, ", "Create torrent -> info");
        statusString = [fileString stringByAppendingString: statusString];
    }
    [fStatusField setStringValue: statusString];
    
    [fPiecesField setStringValue: [NSString stringWithFormat: NSLocalizedString(@"%d pieces, %@ each", "Create torrent -> info"),
                                    fInfo->pieceCount, [NSString stringForFileSize: fInfo->pieceSize]]];
    
    fLocation = [[DEFAULT_SAVE_LOCATION stringByExpandingTildeInPath] retain];
    [fLocationIcon setImage: [[NSWorkspace sharedWorkspace] iconForFile: fLocation]];
    [fLocationField setStringValue: [fLocation stringByAbbreviatingWithTildeInPath]];
    [fLocationField setToolTip: fLocation];
    
    [fTorrentNameField setStringValue: [name stringByAppendingPathExtension: @"torrent"]];
}

- (void) dealloc
{
    [fPath release];
    if (fLocation)
        [fLocation release];
    
    if (fInfo)
        tr_metaInfoBuilderFree(fInfo);
    
    [super dealloc];
}

- (void) setLocation: (id) sender
{
    NSOpenPanel * panel = [NSOpenPanel openPanel];

    [panel setPrompt: @"Select"];
    [panel setAllowsMultipleSelection: NO];
    [panel setCanChooseFiles: NO];
    [panel setCanChooseDirectories: YES];
    [panel setCanCreateDirectories: YES];

    [panel beginSheetForDirectory: nil file: nil types: nil modalForWindow: [self window] modalDelegate: self
            didEndSelector: @selector(locationSheetClosed:returnCode:contextInfo:) contextInfo: nil];
}

- (void) create: (id) sender
{
    //parse tracker string
    NSString * trackerString = [fTrackerField stringValue];
    if ([trackerString rangeOfString: @"://"].location != NSNotFound)
    {
        if (![trackerString hasPrefix: @"http://"])
        {
            NSAlert * alert = [[[NSAlert alloc] init] autorelease];
            [alert addButtonWithTitle: NSLocalizedString(@"OK", "Create torrent -> http warning -> button")];
            [alert setMessageText: NSLocalizedString(@"Tracker address must begin with \"http://\".",
                                                    "Create torrent -> http warning -> title")];
            [alert setInformativeText: NSLocalizedString(@"Change the tracker address to create the torrent.",
                                                        "Create torrent -> http warning -> warning")];
            [alert setAlertStyle: NSWarningAlertStyle];
            
            [alert beginSheetModalForWindow: [self window] modalDelegate: self didEndSelector: nil contextInfo: nil];
            return;
        }
    }
    else
        trackerString = [@"http://" stringByAppendingString: trackerString];
    
    NSString * torrentName = [fTorrentNameField stringValue];
    if ([[torrentName pathExtension] caseInsensitiveCompare: @"torrent"] != NSOrderedSame)
    {
        NSAlert * alert = [[[NSAlert alloc] init] autorelease];
        [alert addButtonWithTitle: NSLocalizedString(@"OK", "Create torrent -> torrent extension warning -> button")];
        [alert setMessageText: NSLocalizedString(@"Torrents must end in \".torrent\".",
                                                "Create torrent -> torrent extension warning -> title")];
        [alert setInformativeText: NSLocalizedString(@"Add this file extension to create the torrent.",
                                                    "Create torrent -> torrent extension warning -> warning")];
        [alert setAlertStyle: NSWarningAlertStyle];
        
        [alert beginSheetModalForWindow: [self window] modalDelegate: self didEndSelector: nil contextInfo: nil];
        return;
    }
    
    #warning fix
    tr_makeMetaInfo(fInfo, NULL, self, [[fLocation stringByAppendingPathComponent: torrentName] UTF8String],
            [trackerString UTF8String], [[fCommentView string] UTF8String], [fPrivateCheck state] == NSOnState);
    
    #warning add to T

    [[self window] close];
}

- (void) cancelCreate: (id) sender
{
    [[self window] close];
}

- (void) windowWillClose: (NSNotification *) notification
{
    [self release];
}

@end

@implementation CreatorWindowController (Private)

+ (NSString *) chooseFile
{
    NSOpenPanel * panel = [NSOpenPanel openPanel];
    
    [panel setPrompt: NSLocalizedString(@"Select", "Create torrent -> select file")];
    [panel setAllowsMultipleSelection: NO];
    [panel setCanChooseFiles: YES];
    [panel setCanChooseDirectories: YES];
    [panel setCanCreateDirectories: NO];

    [panel setMessage: NSLocalizedString(@"Select a file or folder for the torrent file.", "Create torrent -> select file")];
    
    BOOL success = [panel runModal] == NSOKButton;
    return success ? [[panel filenames] objectAtIndex: 0] : nil;
}

- (void) setPath: (NSString *) path
{
    fPath = [path retain];
}

- (void) locationSheetClosed: (NSOpenPanel *) openPanel returnCode: (int) code contextInfo: (void *) info
{
    if (code == NSOKButton)
    {
        [fLocation release];
        fLocation = [[[openPanel filenames] objectAtIndex: 0] retain];
        
        [fLocationIcon setImage: [[NSWorkspace sharedWorkspace] iconForFile: fLocation]];
        [fLocationField setStringValue: [fLocation stringByAbbreviatingWithTildeInPath]];
        [fLocationField setToolTip: fLocation];
    }
}

@end
