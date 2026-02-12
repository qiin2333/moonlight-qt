#pragma once

#include <QString>
#include <string>
#include <vector>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

enum TextAlignment {
    AlignTop,        // 顶部对齐
    AlignCenter,     // 居中对齐 
    AlignBottom      // 底部对齐（默认）
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);
    void setTextAlignment(OverlayType type, TextAlignment alignment);
    TextAlignment getTextAlignment(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    
    // 文本格式解析相关方法
    struct TextSegment {
        std::string text;
        bool isBold;
        bool isItalic;
        int fontSize;        // 字体大小，-1表示使用默认大小
        bool isRelativeSize; // 是否为相对大小调整
    };
    
    std::vector<TextSegment> parseFormattedText(const char* text);
    SDL_Surface* renderFormattedText(OverlayType type, const std::vector<TextSegment>& segments);
    TTF_Font* getFontForStyle(OverlayType type, bool isBold, bool isItalic);
    TTF_Font* getFontForStyleAndSize(OverlayType type, bool isBold, bool isItalic, int fontSize);
    SDL_Surface* renderSmoothTextSegment(TTF_Font* font, const std::string& text, SDL_Color color, SDL_Color bgcolor);
    int calculateActualFontSize(OverlayType type, int requestedSize, bool isRelative);
    int calculateTextBaseline(TTF_Font* font);
    void calculateSegmentMetrics(const std::vector<TextSegment>& segments, OverlayType type, 
                                int& totalWidth, int& maxHeight, int& maxAscent, int& maxDescent);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        SDL_Color bgcolor;
        char text[1024];
        TextAlignment textAlignment;  // 文本对齐方式

        TTF_Font* font;          // 普通字体
        TTF_Font* fontBold;      // 粗体字体
        TTF_Font* fontItalic;    // 斜体字体
        TTF_Font* fontBoldItalic; // 粗体斜体字体
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
