#include "overlaymanager.h"
#include "path.h"
#include <string>
#include <vector>
#include <regex>
#include <algorithm>

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    // 获取默认显示器的DPI
    float ddpi, hdpi, vdpi;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "无法获取显示器DPI: %s", SDL_GetError());
        ddpi = 96.0f; // 使用默认DPI
    }
    
    // DPI缩放因子（基于标准96 DPI）
    float dpiScale = ddpi / 96.0f;
    
    // 使用DPI缩放调整字体大小
    m_Overlays[OverlayType::OverlayDebug].color = {0xBD, 0xF9, 0xE7, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = (int)(20 * dpiScale);
    m_Overlays[OverlayType::OverlayDebug].bgcolor = {0x00, 0x00, 0x00, 0x96};
    m_Overlays[OverlayType::OverlayDebug].textAlignment = TextAlignment::AlignBottom;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = (int)(36 * dpiScale);
    m_Overlays[OverlayType::OverlayStatusUpdate].textAlignment = TextAlignment::AlignCenter;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
    
    // 设置全局字体渲染质量
    // 启用高质量字体缩放（如果支持）
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");  // 使用高质量线性过滤
    
    // 为更好的字体渲染启用垂直同步（减少闪烁）
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
        if (m_Overlays[i].fontBold != nullptr) {
            TTF_CloseFont(m_Overlays[i].fontBold);
        }
        if (m_Overlays[i].fontItalic != nullptr) {
            TTF_CloseFont(m_Overlays[i].fontItalic);
        }
        if (m_Overlays[i].fontBoldItalic != nullptr) {
            TTF_CloseFont(m_Overlays[i].fontBoldItalic);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    strncpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    m_Overlays[type].text[getOverlayMaxTextLength() - 1] = '\0';

    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }

    if (m_Overlays[type].enabled)
    {
        // 解析格式化文本
        std::vector<TextSegment> segments = parseFormattedText(m_Overlays[type].text);
        
        // 渲染格式化文本
        SDL_Surface* formattedSurface = renderFormattedText(type, segments);
        
        if (formattedSurface != nullptr) {
            SDL_AtomicSetPtr((void **)&m_Overlays[type].surface, formattedSurface);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                       "格式化文本渲染失败");
            return;
        }
    }

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);
}

std::vector<OverlayManager::TextSegment> OverlayManager::parseFormattedText(const char* text)
{
    std::vector<TextSegment> segments;
    std::string input(text);
    
    // 支持的格式：
    // ***粗体斜体***, **粗体**, *斜体*
    // {16}指定字号, {+2}相对增大, {-1}相对减小
    // 组合格式：{18}**大号粗体**
    std::regex formatRegex(R"(\{([+-]?\d+)\}|(\*\*\*([^\*]+)\*\*\*)|(\*\*([^\*]+)\*\*)|(\*([^\*]+)\*))");
    std::sregex_iterator iter(input.begin(), input.end(), formatRegex);
    std::sregex_iterator end;
    
    size_t lastEnd = 0;
    int currentFontSize = -1;  // 当前字号，-1表示使用默认
    bool isRelativeSize = false;
    
    for (; iter != end; ++iter) {
        const std::smatch& match = *iter;
        
        // 添加匹配前的普通文本
        if (match.position() > lastEnd) {
            std::string normalText = input.substr(lastEnd, match.position() - lastEnd);
            if (!normalText.empty()) {
                segments.push_back({normalText, false, false, currentFontSize, isRelativeSize});
            }
        }
        
        // 检查是否为字号标记 {数字}
        if (!match[1].str().empty()) {
            std::string sizeStr = match[1].str();
            try {
                int size = std::stoi(sizeStr);
                if (sizeStr[0] == '+' || sizeStr[0] == '-') {
                    // 相对大小调整
                    currentFontSize = size;
                    isRelativeSize = true;
                } else {
                    // 绝对大小
                    currentFontSize = size;
                    isRelativeSize = false;
                }
            } catch (const std::exception& e) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                           "无效的字号格式: %s", sizeStr.c_str());
                currentFontSize = -1;
                isRelativeSize = false;
            }
        }
        // 检查格式化文本标记
        else {
            TextSegment segment;
            segment.fontSize = currentFontSize;
            segment.isRelativeSize = isRelativeSize;
            
            if (!match[3].str().empty()) {
                // ***粗体斜体***
                segment.text = match[3].str();
                segment.isBold = true;
                segment.isItalic = true;
            } else if (!match[5].str().empty()) {
                // **粗体**
                segment.text = match[5].str();
                segment.isBold = true;
                segment.isItalic = false;
            } else if (!match[7].str().empty()) {
                // *斜体*
                segment.text = match[7].str();
                segment.isBold = false;
                segment.isItalic = true;
            }
            
            segments.push_back(segment);
        }
        
        lastEnd = match.position() + match.length();
    }
    
    // 添加最后的普通文本
    if (lastEnd < input.length()) {
        std::string normalText = input.substr(lastEnd);
        if (!normalText.empty()) {
            segments.push_back({normalText, false, false, currentFontSize, isRelativeSize});
        }
    }
    
    // 如果没有找到任何格式化标记，返回整个文本作为普通文本
    if (segments.empty()) {
        segments.push_back({input, false, false, -1, false});
    }
    
    return segments;
}

TTF_Font* OverlayManager::getFontForStyle(OverlayType type, bool isBold, bool isItalic)
{
    TTF_Font** targetFont;
    
    if (isBold && isItalic) {
        targetFont = &m_Overlays[type].fontBoldItalic;
    } else if (isBold) {
        targetFont = &m_Overlays[type].fontBold;
    } else if (isItalic) {
        targetFont = &m_Overlays[type].fontItalic;
    } else {
        targetFont = &m_Overlays[type].font;
    }
    
    // 如果字体还没有创建，创建它
    if (*targetFont == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL覆盖层字体数据为空");
            return nullptr;
        }
        
        *targetFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                     1,
                                     m_Overlays[type].fontSize);
        if (*targetFont == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() 失败: %s",
                        TTF_GetError());
            return nullptr;
        }
        
        // 设置字体样式
        int style = TTF_STYLE_NORMAL;
        if (isBold) style |= TTF_STYLE_BOLD;
        if (isItalic) style |= TTF_STYLE_ITALIC;
        TTF_SetFontStyle(*targetFont, style);
        
        // 启用字体平滑渲染设置
        // 设置字体提示以改善渲染质量
        TTF_SetFontHinting(*targetFont, TTF_HINTING_LIGHT);
        
        // 启用字体轮廓 (如果支持)
        TTF_SetFontOutline(*targetFont, 0);
        
        // 设置字体距离 (字符间距)
        // TTF_SetFontKerning(*targetFont, 1); // 启用字距调整
        
        // 额外的字体质量设置
        // 设置字体样式优化
        if (TTF_GetFontKerning != nullptr) {
            // 如果支持字距调整，启用它以获得更好的字符间距
            TTF_SetFontKerning(*targetFont, 1);
        }
        
        // 设置字体包装对齐（用于多行文本）
        TTF_SetFontWrappedAlign(*targetFont, TTF_WRAPPED_ALIGN_CENTER);
    }
    
    return *targetFont;
}

SDL_Surface* OverlayManager::renderFormattedText(OverlayType type, const std::vector<TextSegment>& segments)
{
    if (segments.empty()) {
        return nullptr;
    }
    
    // 使用精确的度量计算来获取文本信息
    int totalWidth, maxHeight, maxAscent, maxDescent;
    calculateSegmentMetrics(segments, type, totalWidth, maxHeight, maxAscent, maxDescent);
    
    if (totalWidth == 0 || maxHeight == 0) {
        return nullptr;
    }
    
    std::vector<SDL_Surface*> segmentSurfaces;
    std::vector<TTF_Font*> temporaryFonts; // 用于跟踪需要清理的临时字体
    std::vector<int> segmentAscents; // 记录每个片段的ascent值
    
    // 渲染所有文本片段
    for (const auto& segment : segments) {
        // 计算实际字号
        int actualFontSize = calculateActualFontSize(type, segment.fontSize, segment.isRelativeSize);
        
        TTF_Font* font;
        bool isTemporaryFont = false;
        
        if (segment.fontSize == -1) {
            // 使用缓存的默认字体
            font = getFontForStyle(type, segment.isBold, segment.isItalic);
        } else {
            // 创建指定字号的临时字体
            font = getFontForStyleAndSize(type, segment.isBold, segment.isItalic, actualFontSize);
            isTemporaryFont = true;
        }
        
        if (font == nullptr) {
            continue;
        }
        
        if (isTemporaryFont) {
            temporaryFonts.push_back(font);
        }
        
        // 使用优化的平滑文本渲染方法
        SDL_Surface* surface = renderSmoothTextSegment(font, segment.text, 
                                                     m_Overlays[type].color, 
                                                     m_Overlays[type].bgcolor);
        
        if (surface != nullptr) {
            segmentSurfaces.push_back(surface);
            segmentAscents.push_back(TTF_FontAscent(font));
        }
    }
    
    if (segmentSurfaces.empty()) {
        // 清理临时字体
        for (TTF_Font* font : temporaryFonts) {
            TTF_CloseFont(font);
        }
        return nullptr;
    }
    
    // 添加内边距
    float ddpi, hdpi, vdpi;
    int padding = 2; // 默认内边距
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0) {
        float dpiScale = ddpi / 96.0f;
        padding = (int)(padding * dpiScale);
    }
    
    // 使用精确的高度计算（考虑ascent和descent）
    int surfaceHeight = maxAscent + maxDescent;
    
    // 创建组合表面 - 使用32位RGBA格式以支持更好的混合
    SDL_Surface* combinedSurface = SDL_CreateRGBSurfaceWithFormat(
        0,
        totalWidth + padding * 2,
        surfaceHeight + padding * 2,
        32,
        SDL_PIXELFORMAT_RGBA32
    );
    
    if (combinedSurface == nullptr) {
        // 清理片段表面和临时字体
        for (SDL_Surface* surface : segmentSurfaces) {
            SDL_FreeSurface(surface);
        }
        for (TTF_Font* font : temporaryFonts) {
            TTF_CloseFont(font);
        }
        return nullptr;
    }
    
    // 用背景色填充组合表面
    SDL_FillRect(combinedSurface, nullptr, 
                SDL_MapRGBA(combinedSurface->format, 
                           m_Overlays[type].bgcolor.r,
                           m_Overlays[type].bgcolor.g,
                           m_Overlays[type].bgcolor.b,
                           m_Overlays[type].bgcolor.a));
    
    // 将所有片段复制到组合表面，使用精确的基线对齐
    int currentX = padding;
    for (size_t i = 0; i < segmentSurfaces.size(); ++i) {
        SDL_Surface* surface = segmentSurfaces[i];
        int segmentAscent = segmentAscents[i];
        
        // 根据对齐方式计算Y偏移
        int yOffset;
        switch (m_Overlays[type].textAlignment) {
            case TextAlignment::AlignTop:
                // 顶部对齐：所有文本的顶部对齐
                yOffset = padding;
                break;
            case TextAlignment::AlignCenter:
                // 居中对齐：基于整个文本区域的中心
                yOffset = padding + (surfaceHeight - surface->h) / 2;
                break;
            case TextAlignment::AlignBottom:
            default:
                // 底部对齐（基线对齐）：使用ascent信息进行精确基线对齐
                yOffset = padding + (maxAscent - segmentAscent);
                break;
        }
        
        SDL_Rect destRect = {currentX, yOffset, surface->w, surface->h};
        
        // 启用alpha混合以获得更平滑的效果
        SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(surface, nullptr, combinedSurface, &destRect);
        
        currentX += surface->w;
        SDL_FreeSurface(surface);
    }
    
    // 清理临时字体
    for (TTF_Font* font : temporaryFonts) {
        TTF_CloseFont(font);
    }
    
    return combinedSurface;
}

SDL_Surface* OverlayManager::renderSmoothTextSegment(TTF_Font* font, const std::string& text, SDL_Color color, SDL_Color bgcolor)
{
    if (font == nullptr || text.empty()) {
        return nullptr;
    }
    
    SDL_Surface* surface = nullptr;
    
    // 尝试使用最高质量的渲染方法
    
    // 1. 首先尝试使用Blended渲染（最佳抗锯齿效果）
    surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    
    if (surface != nullptr) {
        return surface;
    }
    
    // 2. 如果Blended失败，尝试使用Blended Wrapped（适合长文本）
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
               "Blended渲染失败，尝试Blended Wrapped渲染");
    
    surface = TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), color, 0);
    
    if (surface != nullptr) {
        return surface;
    }
    
    // 3. 如果还是失败，使用Shaded渲染（中等质量）
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
               "Blended Wrapped渲染失败，尝试Shaded渲染");
    
    surface = TTF_RenderUTF8_Shaded(font, text.c_str(), color, bgcolor);
    
    if (surface != nullptr) {
        return surface;
    }
    
    // 4. 最后的后备方案：使用Solid渲染（基本质量）
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
               "Shaded渲染失败，使用Solid渲染作为后备");
    
    surface = TTF_RenderUTF8_Solid(font, text.c_str(), color);
    
    if (surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "所有文本渲染方法都失败了: %s", TTF_GetError());
    }
    
    return surface;
}

int OverlayManager::calculateActualFontSize(OverlayType type, int requestedSize, bool isRelative)
{
    int baseFontSize = m_Overlays[type].fontSize;
    
    if (requestedSize == -1) {
        // 使用默认字号
        return baseFontSize;
    }
    
    if (isRelative) {
        // 相对调整：基础字号 + 调整值
        int newSize = baseFontSize + requestedSize;
        
        // 限制字号范围（最小8，最大128）
        if (newSize < 8) newSize = 8;
        if (newSize > 128) newSize = 128;
        
        return newSize;
    } else {
        // 绝对字号：直接使用指定值
        // 同样限制范围
        if (requestedSize < 8) return 8;
        if (requestedSize > 128) return 128;
        
        return requestedSize;
    }
}

TTF_Font* OverlayManager::getFontForStyleAndSize(OverlayType type, bool isBold, bool isItalic, int fontSize)
{
    // 如果fontSize为-1，使用标准的getFontForStyle方法
    if (fontSize == -1) {
        return getFontForStyle(type, isBold, isItalic);
    }
    
    // 为不同字号创建临时字体（这里简化处理，实际可以考虑缓存机制）
    if (m_FontData.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL覆盖层字体数据为空");
        return nullptr;
    }
    
    TTF_Font* tempFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                       1, fontSize);
    if (tempFont == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_OpenFont() 失败 (字号: %d): %s",
                    fontSize, TTF_GetError());
        // 回退到默认字体
        return getFontForStyle(type, isBold, isItalic);
    }
    
    // 设置字体样式
    int style = TTF_STYLE_NORMAL;
    if (isBold) style |= TTF_STYLE_BOLD;
    if (isItalic) style |= TTF_STYLE_ITALIC;
    TTF_SetFontStyle(tempFont, style);
    
    // 应用字体平滑设置
    TTF_SetFontHinting(tempFont, TTF_HINTING_LIGHT);
    TTF_SetFontOutline(tempFont, 0);
    
    if (TTF_GetFontKerning != nullptr) {
        TTF_SetFontKerning(tempFont, 1);
    }
    
    TTF_SetFontWrappedAlign(tempFont, TTF_WRAPPED_ALIGN_CENTER);
    
    return tempFont;
}

void OverlayManager::setTextAlignment(OverlayType type, TextAlignment alignment)
{
    if (type >= OverlayMax) {
        return;
    }
    
    bool stateChanged = m_Overlays[type].textAlignment != alignment;
    m_Overlays[type].textAlignment = alignment;
    
    // 如果对齐方式发生变化且覆盖层已启用，重新渲染
    if (stateChanged && m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

TextAlignment OverlayManager::getTextAlignment(OverlayType type)
{
    if (type >= OverlayMax) {
        return TextAlignment::AlignBottom;
    }
    
    return m_Overlays[type].textAlignment;
}

int OverlayManager::calculateTextBaseline(TTF_Font* font)
{
    if (font == nullptr) {
        return 0;
    }
    
    // 获取字体的上升高度（基线到顶部的距离）
    return TTF_FontAscent(font);
}

void OverlayManager::calculateSegmentMetrics(const std::vector<TextSegment>& segments, OverlayType type, 
                                           int& totalWidth, int& maxHeight, int& maxAscent, int& maxDescent)
{
    totalWidth = 0;
    maxHeight = 0;
    maxAscent = 0;
    maxDescent = 0;
    
    for (const auto& segment : segments) {
        // 计算实际字号
        int actualFontSize = calculateActualFontSize(type, segment.fontSize, segment.isRelativeSize);
        
        TTF_Font* font;
        bool isTemporaryFont = false;
        
        if (segment.fontSize == -1) {
            font = getFontForStyle(type, segment.isBold, segment.isItalic);
        } else {
            font = getFontForStyleAndSize(type, segment.isBold, segment.isItalic, actualFontSize);
            isTemporaryFont = true;
        }
        
        if (font == nullptr) {
            continue;
        }
        
        // 获取文本宽度和高度
        int textWidth, textHeight;
        if (TTF_SizeUTF8(font, segment.text.c_str(), &textWidth, &textHeight) == 0) {
            totalWidth += textWidth;
            
            // 获取字体度量信息
            int ascent = TTF_FontAscent(font);
            int descent = TTF_FontDescent(font);
            int height = TTF_FontHeight(font);
            
            maxHeight = std::max(maxHeight, height);
            maxAscent = std::max(maxAscent, ascent);
            maxDescent = std::max(maxDescent, std::abs(descent)); // descent通常是负数
        }
        
        if (isTemporaryFont) {
            TTF_CloseFont(font);
        }
    }
}
