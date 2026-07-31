#include "macwindowchrome.h"

#import <AppKit/AppKit.h>

namespace
{

// 把系统标题栏那条带子拉高到 barHeight，并让红绿灯在新高度里垂直居中。
//
// 这里动的是 standardWindowButton 的 superview 链：
//   NSButton（红绿灯） → NSTitlebarView → NSTitlebarContainerView → 窗口的 frame view
// 这套层级从 10.10 一直稳定到现在，是各家做「高标题栏」的通用做法，但毕竟不是公开
// API，所以每一步都做了判空，拿不到就原样返回、什么都不改（最坏情况是红绿灯留在
// 最上面那条，功能不受影响）。
void applyTallTitleBar(NSWindow* window, CGFloat barHeight)
{
    if (window == nil) {
        return;
    }

    NSButton* buttons[] = {
        [window standardWindowButton:NSWindowCloseButton],
        [window standardWindowButton:NSWindowMiniaturizeButton],
        [window standardWindowButton:NSWindowZoomButton],
    };

    NSView* titleBarView = buttons[0].superview;
    if (titleBarView == nil) {
        return;
    }

    NSView* container = titleBarView.superview;
    if (container == nil) {
        return;
    }

    // 带子贴着窗口顶边，所以拉高的同时要把原点往下挪同样的量
    NSRect containerFrame = container.frame;
    if (containerFrame.size.height < barHeight) {
        CGFloat delta = barHeight - containerFrame.size.height;
        containerFrame.size.height = barHeight;
        containerFrame.origin.y -= delta;
        container.frame = containerFrame;
    }

    // 里面那层 NSTitlebarView 也铺满容器。实测只拉容器就已经能把按钮居中了，
    // 但两层高度对不上时 AppKit 在窗口尺寸变化后重新布局的结果不好预料，
    // 顺手对齐，省得留一个只在特定版本成立的巧合。
    NSRect titleBarFrame = titleBarView.frame;
    titleBarFrame.origin.y = 0;
    titleBarFrame.size.height = barHeight;
    titleBarView.frame = titleBarFrame;

    // 三颗按钮在带子里垂直居中。NSView 默认不翻转，y 从下往上算。
    for (NSButton* button : buttons) {
        if (button == nil) {
            continue;
        }

        NSRect buttonFrame = button.frame;
        buttonFrame.origin.y = (barHeight - buttonFrame.size.height) / 2.0;
        button.frame = buttonFrame;
    }
}

} // namespace

void MacWindowChrome::useTallTitleBar(QWindow* window, int barHeight)
{
    if (window == nullptr) {
        return;
    }

    // 需要已经有 NSWindow 才能动它的视图层级。窗口还没创建出来时先等一轮事件循环。
    window->create();

    NSView* view = reinterpret_cast<NSView*>(window->winId());
    NSWindow* nsWindow = view.window;
    if (nsWindow == nil) {
        return;
    }

    applyTallTitleBar(nsWindow, barHeight);

    // AppKit 会在窗口尺寸变化、进出全屏之后把带子的布局改回默认高度，所以这几个
    // 时机都要重新应用一遍。用 block 版观察者，生命周期跟着 NSWindow 走。
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSArray<NSNotificationName>* names = @[
        NSWindowDidResizeNotification,
        NSWindowDidEndLiveResizeNotification,
        NSWindowDidEnterFullScreenNotification,
        NSWindowDidExitFullScreenNotification,
        NSWindowDidBecomeKeyNotification,
    ];

    for (NSNotificationName name in names) {
        [center addObserverForName:name
                           object:nsWindow
                            queue:nil
                       usingBlock:^(NSNotification* note) {
            applyTallTitleBar(static_cast<NSWindow*>(note.object), barHeight);
        }];
    }
}
