#import <AppKit/AppKit.h>

extern "C" void ClipboardHelperSetBackgroundActivationPolicy()
{
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

extern "C" int ClipboardHelperPasteboardChangeCount()
{
    return static_cast<int>([[NSPasteboard generalPasteboard] changeCount]);
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
