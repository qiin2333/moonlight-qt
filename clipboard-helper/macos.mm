#import <AppKit/AppKit.h>

extern "C" void ClipboardHelperSetBackgroundActivationPolicy()
{
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

extern "C" int ClipboardHelperPasteboardChangeCount()
{
    return static_cast<int>([[NSPasteboard generalPasteboard] changeCount]);
}
