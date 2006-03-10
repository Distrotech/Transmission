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

#import "NameCell.h"
#import "Torrent.h"
#import "StringAdditions.h"
#import "Utils.h"

@implementation NameCell

- (void) drawWithFrame: (NSRect) cellFrame inView: (NSView *) view
{
    Torrent * torrent = [self objectValue];

    NSString * string;
    NSPoint pen;
    NSMutableDictionary * attributes;

    if( ![view lockFocusIfCanDraw] )
    {
        return;
    }

    pen = cellFrame.origin;
    float cellWidth = cellFrame.size.width;

    pen.x += 5; pen.y += 5;
    NSImage * icon = [torrent icon];
    [icon drawAtPoint: pen fromRect:
        NSMakeRect( 0, 0, [icon size].width, [icon size].height )
        operation: NSCompositeSourceOver fraction: 1.0];

    attributes = [NSMutableDictionary dictionaryWithCapacity: 2];
    [attributes setObject: /*fWhiteText*/0 ? [NSColor whiteColor] :
        [NSColor blackColor] forKey: NSForegroundColorAttributeName];

    [attributes setObject: [NSFont messageFontOfSize: 12.0]
        forKey: NSFontAttributeName];

    pen.x += 37;
    NSString * sizeString = [NSString stringWithFormat: @" (%@)",
        [NSString stringForFileSize: [torrent size]]];
    string = [[[torrent name] stringFittingInWidth: cellWidth -
        72 - [sizeString sizeWithAttributes: attributes].width
        withAttributes: attributes] stringByAppendingString: sizeString];
    [string drawAtPoint: pen withAttributes: attributes];

    [attributes setObject: [NSFont messageFontOfSize: 10.0]
        forKey: NSFontAttributeName];

    pen.x += 5; pen.y += 20;
    [[torrent statusString] drawAtPoint: pen withAttributes: attributes];

    pen.x += 0; pen.y += 15;
    string = [[torrent infoString] stringFittingInWidth:
        ( cellFrame.size.width - 77 ) withAttributes: attributes];
    [string drawAtPoint: pen withAttributes: attributes];

    [view unlockFocus];
}

@end
