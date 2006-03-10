#import <Cocoa/Cocoa.h>
#import <transmission.h>

@class Controller;

@interface TorrentTableView : NSTableView

{
    IBOutlet Controller  * fController;

    NSPoint                fClickPoint;
    
    IBOutlet NSMenu     * fContextRow;
    IBOutlet NSMenu     * fContextNoRow;
}

@end
