#import <AppKit/AppKit.h>

extern "C" void ClipboardHelperSetBackgroundActivationPolicy()
{
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

extern "C" int ClipboardHelperPasteboardChangeCount()
{
    return static_cast<int>([[NSPasteboard generalPasteboard] changeCount]);
}

// True when the pasteboard was produced by a macOS Finder file copy. Finder
// stamps node-reference flavors that QMimeData never surfaces (Qt only maps
// the flavors it understands), so this must read the raw pasteboard types.
// Finder copies carry only the 512x512 file icon as image data, whereas
// document/chat-app image copies attach real bitmaps alongside their cache
// file references and must still sync.
extern "C" bool ClipboardHelperIsFinderPasteboard()
{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    for (NSPasteboardType type in pb.types) {
        if ([type isEqualToString:@"com.apple.finder.noderef"] || [type isEqualToString:@"fndf"]) {
            return true;
        }
    }
    return false;
}

// Write text and image flavors in ONE pasteboard transaction. Qt's macOS
// backend drops the text flavor from a QMimeData that also carries an
// image, so compound clipboard writes must bypass it. Returns false when
// either payload is unusable; callers then fall back to single-flavor
// writes.
extern "C" bool ClipboardHelperWritePasteboardCompound(const char* utf8Text,
                                                       const unsigned char* pngBytes,
                                                       int pngLength)
{
    if (utf8Text == nullptr || pngBytes == nullptr || pngLength <= 0) {
        return false;
    }

    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:utf8Text
                                                  length:strlen(utf8Text)
                                                encoding:NSUTF8StringEncoding];
        NSData* pngData = [NSData dataWithBytes:pngBytes length:static_cast<NSUInteger>(pngLength)];
        NSImage* image = [[NSImage alloc] initWithData:pngData];
        if (text.length == 0 || pngData.length == 0 || image == nil) {
            return false;
        }

        NSData* tiff = [image TIFFRepresentation];

        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSMutableArray<NSPasteboardType>* types = [NSMutableArray array];
        [types addObject:NSPasteboardTypeString];
        [types addObject:NSPasteboardTypePNG];
        if (tiff != nil) {
            [types addObject:NSPasteboardTypeTIFF];
        }
        [pb declareTypes:types owner:nil];
        [pb setString:text forType:NSPasteboardTypeString];
        [pb setData:pngData forType:NSPasteboardTypePNG];
        if (tiff != nil) {
            [pb setData:tiff forType:NSPasteboardTypeTIFF];
        }
        return true;
    }
}
