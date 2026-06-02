#include "SafirmaOverlay.hpp"
#include "../Renderer.hpp"
#include "../../config/ConfigManager.hpp"
#include "../../core/hyprlock.hpp"
#include "../../helpers/Log.hpp"
#include "../../helpers/Color.hpp"
#include "../../auth/SafirmaAuth.hpp"
#include "../../core/LockSurface.hpp"
#include "../../core/Seat.hpp"
#include "../../core/hyprlock.hpp"

#ifdef HAS_RSVG
#include <librsvg/rsvg.h>
#endif

#include <hyprlang.hpp>
#include <pango/pangocairo.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

CSafirmaOverlay::CSafirmaOverlay(bool demoMode) : m_demoMode(demoMode) {
    static const auto IDLETEXT     = g_pConfigManager->getValue<Hyprlang::STRING>("auth:safirma:idle_text");
    static const auto FONTFAMILY   = g_pConfigManager->getValue<Hyprlang::STRING>("auth:safirma:font_family");
    static const auto FONTSIZE     = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:font_size");
    static const auto ACCENTCOLOR  = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:accent_color");
    static const auto PANELBG      = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:panel_bg");
    static const auto PANELBORDER  = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:panel_border");
    static const auto BUTTONBG     = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:button_bg");
    static const auto BUTTONTEXT   = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:button_text");
    static const auto SUCCESSCOLOR = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:success_color");
    static const auto ERRORCOLOR   = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:error_color");

    m_idleText    = *IDLETEXT;
    m_fontFamily  = *FONTFAMILY;
    m_fontSize    = *FONTSIZE;
    m_accentColor = CHyprColor((uint64_t)*ACCENTCOLOR);
    m_panelBg     = CHyprColor((uint64_t)*PANELBG);
    m_panelBorder = CHyprColor((uint64_t)*PANELBORDER);
    m_buttonBg    = CHyprColor((uint64_t)*BUTTONBG);
    m_buttonText  = CHyprColor((uint64_t)*BUTTONTEXT);
    m_successColor = CHyprColor((uint64_t)*SUCCESSCOLOR);
    m_errorColor  = CHyprColor((uint64_t)*ERRORCOLOR);

    Log::logger->log(Log::INFO, "safirma: overlay created (demo={})", m_demoMode ? "yes" : "no");
}

CSafirmaOverlay::~CSafirmaOverlay() {
    releaseTextures();
}

void CSafirmaOverlay::releaseTextures() {
    for (auto& [key, ct] : m_textCache) {
        if (ct.texture.m_iTexID > 0)
            glDeleteTextures(1, &ct.texture.m_iTexID);
    }
    m_textCache.clear();
}

void CSafirmaOverlay::configure(const std::unordered_map<std::string, std::any>& prop, const SP<COutput>& pOutput) {
    m_viewport          = pOutput->getViewport();
    m_fractionalScale   = pOutput->m_sessionLockSurface ? pOutput->m_sessionLockSurface->fractionalScale : 1.0;
}

// ─── Text / Icon rendering (Cairo + Pango → GL texture) ───────────────────

size_t CSafirmaOverlay::hashKey(const std::string& text, int fontSize, uint64_t color) {
    size_t h = std::hash<std::string>{}(text);
    h ^= std::hash<int>{}(fontSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint64_t>{}(color) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

CSafirmaOverlay::CachedText CSafirmaOverlay::getOrCreateText(const std::string& text, int fontSize, CHyprColor color) {
    uint64_t colorKey = ((uint64_t)(uint8_t)(color.r * 255) << 24) |
                        ((uint64_t)(uint8_t)(color.g * 255) << 16) |
                        ((uint64_t)(uint8_t)(color.b * 255) << 8)  |
                        (uint64_t)(uint8_t)(color.a * 255);
    size_t key = hashKey(text, fontSize, colorKey);

    auto it = m_textCache.find(key);
    if (it != m_textCache.end())
        return it->second;

    cairo_surface_t* tmpSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t*         cr      = cairo_create(tmpSurf);
    PangoLayout*     layout  = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);

    PangoFontDescription* fontDesc = pango_font_description_from_string(m_fontFamily.c_str());
    pango_font_description_set_size(fontDesc, fontSize * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);

    int textW = 0, textH = 0;
    pango_layout_get_pixel_size(layout, &textW, &textH);
    cairo_surface_destroy(tmpSurf);
    cairo_destroy(cr);

    const int PAD    = std::max(fontSize / 3 + 2, 4);
    int       width  = textW + PAD * 2;
    int       height = textH + PAD * 2;
    if (width <= 0)  width  = 1;
    if (height <= 0) height = 1;

    cairo_surface_t* surface  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cr                        = cairo_create(surface);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    PangoLayout* finalLayout = pango_cairo_create_layout(cr);
    pango_layout_set_text(finalLayout, text.c_str(), -1);
    pango_layout_set_font_description(finalLayout, fontDesc);

    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, PAD, PAD);
    pango_cairo_show_layout(cr, finalLayout);
    cairo_destroy(cr);

    GLuint texID = 0;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);

    uint8_t* srcData = cairo_image_surface_get_data(surface);
    int      stride  = cairo_image_surface_get_stride(surface);

    std::vector<uint8_t> rgba(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int si       = y * stride + x * 4;
            int di       = (y * width + x) * 4;
            rgba[di + 0] = srcData[si + 2];
            rgba[di + 1] = srcData[si + 1];
            rgba[di + 2] = srcData[si + 0];
            rgba[di + 3] = srcData[si + 3];
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    cairo_surface_destroy(surface);
    pango_font_description_free(fontDesc);
    g_object_unref(finalLayout);
    g_object_unref(layout);

    CachedText ct;
    ct.texture.m_iTarget    = GL_TEXTURE_2D;
    ct.texture.m_iTexID     = texID;
    ct.texture.m_vSize      = Vector2D(width, height);
    ct.texture.m_bAllocated = false;
    ct.texture.m_iType      = TEXTURE_RGBA;
    ct.width                = width;
    ct.height               = height;

    m_textCache[key] = ct;
    return ct;
}

// ─── Fingerprint icon ────────────────────────────────────────────────

// Material Design fingerprint SVG (md_fingerprint outlined 24px)
static const char* FINGERPRINT_SVG = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 -960 960 960">
<path d="M481-781q106 0 200 45.5T838-604q7 9 4.5 16t-8.5 12q-6 5-14 4.5t-14-8.5q-55-78-141.5-119.5T481-741q-97 0-182 41.5T158-580q-6 9-14 10t-14-4q-7-5-8.5-12.5T126-602q62-85 155.5-132T481-781Zm0 94q135 0 232 90t97 223q0 50-35.5 83.5T688-257q-51 0-87.5-33.5T564-374q0-33-24.5-55.5T481-452q-34 0-58.5 22.5T398-374q0 97 57.5 162T604-121q9 3 12 10t1 15q-2 7-8 12t-15 3q-104-26-170-103.5T358-374q0-50 36-84t87-34q51 0 87 34t36 84q0 33 25 55.5t59 22.5q34 0 58-22.5t24-55.5q0-116-85-195t-203-79q-118 0-203 79t-85 194q0 24 4.5 60t21.5 84q3 9-.5 16T208-205q-8 3-15.5-.5T182-217q-15-39-21.5-77.5T154-374q0-133 96.5-223T481-687Zm0-192q64 0 125 15.5T724-819q9 5 10.5 12t-1.5 14q-3 7-10 11t-17-1q-53-27-109.5-41.5T481-839q-58 0-114 13.5T260-783q-8 5-16 2.5T232-791q-4-8-2-14.5t10-11.5q56-30 117-46t124-16Zm0 289q93 0 160 62.5T708-374q0 9-5.5 14.5T688-354q-8 0-14-5.5t-6-14.5q0-75-55.5-125.5T481-550q-76 0-130.5 50.5T296-374q0 81 28 137.5T406-123q6 6 6 14t-6 14q-6 6-14 6t-14-6q-59-62-90.5-126.5T256-374q0-91 66-153.5T481-590Zm-1 196q9 0 14.5 6t5.5 14q0 75 54 123t126 48q6 0 17-1t23-3q9-2 15.5 2.5T744-191q2 8-3 14t-13 8q-18 5-31.5 5.5t-16.5.5q-89 0-154.5-60T460-374q0-8 5.5-14t14.5-6Z"/>
</svg>
)";

CSafirmaOverlay::CachedText CSafirmaOverlay::renderFingerprintIcon(int iconSize, CHyprColor color) {
    uint64_t colorKey = ((uint64_t)(uint8_t)(color.r * 255) << 24) |
                        ((uint64_t)(uint8_t)(color.g * 255) << 16) |
                        ((uint64_t)(uint8_t)(color.b * 255) << 8)  |
                        (uint64_t)(uint8_t)(color.a * 255);
    size_t key = hashKey("\xff__fingerprint__", iconSize, colorKey);

    auto it = m_textCache.find(key);
    if (it != m_textCache.end())
        return it->second;

    const int PAD = 6;
    int       w   = iconSize + PAD * 2;
    int       h   = iconSize + PAD * 2;

    cairo_surface_t* finalSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t*         finalCr   = cairo_create(finalSurf);
    cairo_set_source_rgba(finalCr, 0, 0, 0, 0);
    cairo_paint(finalCr);

#ifdef HAS_RSVG
    // ── SVG rendering via librsvg ──
    GError*        error  = NULL;
    RsvgHandle*    handle = rsvg_handle_new_from_data((const guint8*)FINGERPRINT_SVG, strlen(FINGERPRINT_SVG), &error);
    if (handle && !error) {
        // Render SVG to a mask surface
        cairo_surface_t* maskSurf = cairo_surface_create_similar(finalSurf, CAIRO_CONTENT_ALPHA, w, h);
        cairo_t*         maskCr   = cairo_create(maskSurf);
        cairo_set_source_rgba(maskCr, 0, 0, 0, 0);
        cairo_paint(maskCr);

        // Render SVG into the icon area — librsvg handles viewBox→viewport mapping
        RsvgRectangle viewport = {
            (double)(w - iconSize) / 2.0,
            (double)(h - iconSize) / 2.0,
            (double)iconSize,
            (double)iconSize,
        };
        rsvg_handle_render_document(handle, maskCr, &viewport, &error);
        cairo_destroy(maskCr);

        // Colorize: fill with accent, mask with SVG alpha
        cairo_set_source_rgba(finalCr, color.r, color.g, color.b, color.a);
        cairo_mask_surface(finalCr, maskSurf, 0, 0);
        cairo_surface_destroy(maskSurf);

        g_object_unref(handle);
    } else {
        // Fallback if SVG fails
        if (error)
            g_error_free(error);
        cairo_set_source_rgba(finalCr, color.r, color.g, color.b, color.a);
        cairo_select_font_face(finalCr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(finalCr, iconSize * 0.5);
        cairo_move_to(finalCr, w / 2.0 - iconSize * 0.2, h / 2.0 + iconSize * 0.2);
        cairo_show_text(finalCr, "\xE2\x9F\xB3");
    }
#else
    // ── Fallback: bezier fingerprint ──
    cairo_translate(finalCr, w / 2.0, h / 2.0);
    const double S = iconSize;
    const double lw = std::max(S * 0.05, 1.8);
    cairo_set_source_rgba(finalCr, color.r, color.g, color.b, color.a);
    cairo_set_line_width(finalCr, lw);
    cairo_set_line_cap(finalCr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(finalCr, CAIRO_LINE_JOIN_ROUND);

    struct { double sx, sy, c1x, c1y, c2x, c2y, ex, ey; } const RIDGES[] = {
        {-0.44, -0.22, -0.44, 0.30, 0.44, 0.30, 0.44, -0.22},
        {-0.40, -0.12, -0.40, 0.36, 0.40, 0.36, 0.40, -0.12},
        {-0.36, -0.04, -0.34, 0.42, 0.38, 0.42, 0.36, -0.04},
        {-0.30, 0.02, -0.28, 0.48, 0.32, 0.48, 0.30, 0.02},
        {-0.24, 0.08, -0.22, 0.50, 0.26, 0.50, 0.24, 0.08},
        {-0.18, 0.14, -0.16, 0.48, 0.20, 0.48, 0.18, 0.14},
        {-0.14, 0.20, -0.12, 0.44, 0.16, 0.40, 0.12, 0.22},
        {-0.08, 0.26, -0.06, 0.38, 0.10, 0.38, 0.08, 0.26},
    };
    for (const auto& r : RIDGES) {
        cairo_move_to(finalCr, r.sx * S, r.sy * S);
        cairo_curve_to(finalCr, r.c1x * S, r.c1y * S, r.c2x * S, r.c2y * S, r.ex * S, r.ey * S);
        cairo_stroke(finalCr);
    }
    cairo_set_line_width(finalCr, lw * 0.85);
    struct { double sx, sy, ex, ey; } const FRAGMENTS[] = {
        {0.10, -0.20, 0.28, -0.20}, {-0.28, -0.20, -0.18, -0.22},
        {-0.06, 0.34, 0.06, 0.34}, {0.32, 0.06, 0.36, 0.18},
        {-0.34, 0.16, -0.30, 0.06},
    };
    for (const auto& f : FRAGMENTS) {
        cairo_move_to(finalCr, f.sx * S, f.sy * S);
        cairo_line_to(finalCr, f.ex * S, f.ey * S);
        cairo_stroke(finalCr);
    }
    // Sensor pad
    cairo_set_line_width(finalCr, lw * 0.65);
    cairo_save(finalCr);
    cairo_scale(finalCr, 1.0, 0.55);
    cairo_new_sub_path(finalCr);
    cairo_arc(finalCr, 0, (-S * 0.34) / 0.55, S * 0.14, 0.15 * M_PI, 0.85 * M_PI);
    cairo_stroke(finalCr);
    cairo_restore(finalCr);
#endif

    cairo_destroy(finalCr);

    // ── Cairo → GL texture ──
    GLuint texID = 0;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);

    uint8_t*             srcData = cairo_image_surface_get_data(finalSurf);
    int                  stride  = cairo_image_surface_get_stride(finalSurf);
    std::vector<uint8_t> rgba(w * h * 4);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int si = y * stride + x * 4;
            int di = (y * w + x) * 4;
            rgba[di + 0] = srcData[si + 2];
            rgba[di + 1] = srcData[si + 1];
            rgba[di + 2] = srcData[si + 0];
            rgba[di + 3] = srcData[si + 3];
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    cairo_surface_destroy(finalSurf);

    CachedText ct;
    ct.texture.m_iTarget    = GL_TEXTURE_2D;
    ct.texture.m_iTexID     = texID;
    ct.texture.m_vSize      = Vector2D(w, h);
    ct.texture.m_bAllocated = false;
    ct.texture.m_iType      = TEXTURE_RGBA;
    ct.width                = w;
    ct.height               = h;

    m_textCache[key] = ct;
    return ct;
}

// ─── Demo state machine ───────────────────────────────────────────────────

void CSafirmaOverlay::startDemoTimer() {
    m_stateStart   = std::chrono::steady_clock::now();
    m_timerStarted = true;
}

// ─── Draw ─────────────────────────────────────────────────────────────────

bool CSafirmaOverlay::draw(const SRenderData& data) {
    // Update cursor every frame (onHover only fires on entry)
    updateCursor(g_pHyprlock->m_vMouseLocation);

    eSafirmaUIState currentState = SAFIRMA_UI_IDLE;

    if (m_demoMode) {
        currentState = m_state;

        if (m_timerStarted) {
            auto elapsed = std::chrono::steady_clock::now() - m_stateStart;

            if (m_state == SAFIRMA_UI_SCANNING && elapsed >= std::chrono::seconds(30)) {
                m_state = SAFIRMA_UI_APPROVED;
                startDemoTimer();
            }
            if (m_state == SAFIRMA_UI_APPROVED && elapsed >= std::chrono::seconds(3)) {
                m_state        = SAFIRMA_UI_IDLE;
                m_timerStarted = false;
            }
            if (m_state == SAFIRMA_UI_DENIED && elapsed >= std::chrono::seconds(3)) {
                m_state        = SAFIRMA_UI_IDLE;
                m_timerStarted = false;
            }
            if (m_state == SAFIRMA_UI_EXPIRED && elapsed >= std::chrono::seconds(3)) {
                m_state        = SAFIRMA_UI_IDLE;
                m_timerStarted = false;
            }
            if (m_state == SAFIRMA_UI_ERROR && elapsed >= std::chrono::seconds(4)) {
                m_state        = SAFIRMA_UI_IDLE;
                m_timerStarted = false;
            }
        }
        currentState = m_state;
    } else if (g_pSafirmaAuth) {
        switch (g_pSafirmaAuth->getState()) {
            case SAFIRMA_IDLE:      currentState = SAFIRMA_UI_IDLE;     break;
            case SAFIRMA_CONNECTING:
            case SAFIRMA_WAITING:   currentState = SAFIRMA_UI_SCANNING; break;
            case SAFIRMA_APPROVED:  currentState = SAFIRMA_UI_APPROVED; break;
            case SAFIRMA_DENIED:    currentState = SAFIRMA_UI_DENIED;   break;
            case SAFIRMA_EXPIRED:   currentState = SAFIRMA_UI_EXPIRED;  break;
            case SAFIRMA_ERROR:     currentState = SAFIRMA_UI_ERROR;    break;
        }
    } else {
        return false;
    }

    if (currentState == SAFIRMA_UI_IDLE)
        drawIdle(data);
    else
        drawModal(data, currentState);

    return currentState != SAFIRMA_UI_IDLE;
}

// ─── Idle state: subtle pill button at bottom-center ─────────────────────

void CSafirmaOverlay::drawIdle(const SRenderData& data) {
    // ── Premium fingerprint auth card ──
    const double ICON_SIZE = 44;
    const double LABEL_SZ  = 20;

    auto iconTex = renderFingerprintIcon(ICON_SIZE, m_accentColor);
    if (iconTex.texture.m_iTexID == 0)
        return;

    // Label: strip FontAwesome icon prefix, keep "SAFIRMA"
    std::string label = m_idleText;
    auto        isFA  = [](const std::string& s, int i) { return i + 2 < (int)s.size() && (unsigned char)s[i] == 0xEF && (unsigned char)s[i + 1] == 0x82 && (unsigned char)s[i + 2] == 0x90; };
    if (isFA(label, 0))
        label.erase(0, 3);
    while (!label.empty() && label[0] == ' ')
        label.erase(0, 1);
    if (label.empty())
        label = "SAFIRMA";

    auto labelTex = getOrCreateText(label, LABEL_SZ, m_accentColor);
    if (labelTex.texture.m_iTexID == 0)
        return;

    // ── Layout: match password field width (300px) ──
    const double HPAD = 28, VPAD = 12, GAP = 18;

    double btnW = 300;
    double contentW = iconTex.width + GAP + labelTex.width;
    double contentH = std::max((double)iconTex.height, (double)labelTex.height);
    double btnH = contentH + VPAD * 2;
    double btnX = (m_viewport.x - btnW) / 2.0;
    double btnY = m_viewport.y * 0.25;

    // Content offset within the card
    double contentX = btnX + (btnW - contentW) / 2.0;

    CBox cardBox{btnX, btnY, btnW, btnH};

    // ── Accent glow behind card ──
    CHyprColor glow = m_accentColor;
    glow.a *= 0.08 * data.opacity;
    g_pRenderer->renderRect({btnX - 8, btnY - 8, btnW + 16, btnH + 16}, glow, 44);

    // ── Depth shadows ──
    CHyprColor shadow(0x00000000);
    shadow.a *= 0.45 * data.opacity;
    g_pRenderer->renderRect({btnX + 6, btnY + 6, btnW, btnH}, shadow, 40);
    shadow.a *= 0.25;
    g_pRenderer->renderRect({btnX + 3, btnY + 3, btnW, btnH}, shadow, 40);

    // ── Glass-morphism card ──
    CHyprColor bg = m_panelBg;
    bg.a *= 0.78 * data.opacity;
    g_pRenderer->renderRect(cardBox, bg, 40);

    // ── Accent border ──
    CHyprColor borderClr = m_accentColor;
    borderClr.a *= 0.50 * data.opacity;
    CBox borderBox{btnX - 1, btnY - 1, btnW + 2, btnH + 2};
    g_pRenderer->renderBorder(borderBox, CGradientValueData(borderClr), 1, 41, borderClr.a);

    // ── Inner top shine ──
    CHyprColor shine = m_accentColor;
    shine.a *= 0.10 * data.opacity;
    CBox shineBox{btnX + 8, btnY + 1, btnW - 16, 2};
    g_pRenderer->renderRect(shineBox, shine, 1);

    // ── Icon ──
    double iconX = contentX;
    double iconY = btnY + (btnH - iconTex.height) / 2.0;
    g_pRenderer->renderTexture({iconX, iconY, iconTex.width, iconTex.height}, iconTex.texture, data.opacity, 0);

    // ── Label ──
    double labelX = iconX + iconTex.width + GAP;
    double labelY = btnY + (btnH - labelTex.height) / 2.0;
    g_pRenderer->renderTexture({labelX, labelY, labelTex.width, labelTex.height}, labelTex.texture, data.opacity, 0);

    m_buttonBox = cardBox;
}

// ─── Modal: centered card dialog ─────────────────────────────────────

/* Measure total content height for a state so we can center the block vertically.
   Modal shows: icon + message + (timer) + (cancel button or hint).
   No title — the icon + message are self-explanatory. */
static double measureModalContentHeight(eSafirmaUIState state,
                                         bool hasIcon, double iconH,
                                         bool hasMsg, double msgH,
                                         bool hasTimer, double timerH,
                                         bool hasCancel, double btnH,
                                         bool hasHint, double hintH,
                                         double& iconGap, double& msgGap,
                                         double& timerGap, double& btnGap,
                                         double& hintGap) {
    iconGap  = 8;   // after icon → message
    msgGap   = 14;  // after message → timer
    timerGap = 20;  // after timer → button
    btnGap   = 24;  // after message → button (no timer)
    hintGap  = 20;  // after message → hint

    double h = 0;
    if (hasIcon)  h += iconH;
    if (hasMsg) {
        if (h > 0) h += iconGap;
        h += msgH;
    }
    if (hasTimer) {
        if (h > 0) h += msgGap;
        h += timerH;
    }
    if (hasCancel) {
        if (h > 0) h += btnGap;
        h += btnH;
    }
    if (hasHint) {
        if (h > 0) h += hintGap;
        h += hintH;
    }
    return h;
}

void CSafirmaOverlay::drawModal(const SRenderData& data, eSafirmaUIState state) {
    // Full-screen dark overlay
    CHyprColor overlayBg(0x60000000);
    overlayBg.a *= data.opacity;
    g_pRenderer->renderRect({0, 0, m_viewport.x, m_viewport.y}, overlayBg, 0);

    // ── Panel ──
    const double PANEL_W = 480;
    const double PANEL_H = 380;
    const double PX      = (m_viewport.x - PANEL_W) / 2.0;
    const double PY      = (m_viewport.y - PANEL_H) / 2.0;

    CBox panel{PX, PY, PANEL_W, PANEL_H};

    // Shadow layers for depth
    CHyprColor shadow(0x00000000);
    shadow.a *= 0.5 * data.opacity;
    g_pRenderer->renderRect({PX + 6, PY + 6, PANEL_W, PANEL_H}, shadow, 14);
    g_pRenderer->renderRect({PX + 3, PY + 3, PANEL_W, PANEL_H}, shadow, 14);

    // Card background
    CHyprColor bg = m_panelBg;
    bg.a *= data.opacity;
    g_pRenderer->renderRect(panel, bg, 14);

    // Card border
    CHyprColor border = m_panelBorder;
    border.a *= data.opacity;
    CBox borderBox{PX - 2, PY - 2, PANEL_W + 4, PANEL_H + 4};
    g_pRenderer->renderBorder(borderBox, CGradientValueData(border), 2, 15, border.a);

    m_modalBox = panel;

    // ── Determine content per state ──
    // No title — icon + message are self-explanatory
    // SCANNING uses custom Cairo fingerprint icon; others use emoji
    const std::string iconStr   = state == SAFIRMA_UI_SCANNING ? "" :                   // custom fingerprint below
                                  state == SAFIRMA_UI_APPROVED ? "\xE2\x9C\x85" :      // checkmark
                                  state == SAFIRMA_UI_DENIED   ? "\xE2\x9C\x97" :      // cross
                                  state == SAFIRMA_UI_EXPIRED  ? "\xE2\x8F\xB0" :      // alarm clock
                                  state == SAFIRMA_UI_ERROR    ? "\xE2\x9A\xA0\xEF\xB8\x8F" : "";  // warning
    const std::string msgStr    = state == SAFIRMA_UI_SCANNING ? "Authenticate on your phone" :
                                  state == SAFIRMA_UI_APPROVED ? "Authenticated!" :
                                  state == SAFIRMA_UI_DENIED   ? "Authentication denied" :
                                  state == SAFIRMA_UI_EXPIRED  ? "Challenge expired" :
                                  state == SAFIRMA_UI_ERROR    ? "Could not connect" : "";
    const bool        hasCancel = (state == SAFIRMA_UI_SCANNING || state == SAFIRMA_UI_DENIED || state == SAFIRMA_UI_EXPIRED);
    const bool        hasHint   = (state == SAFIRMA_UI_APPROVED);
    const bool        hasTimer  = (state == SAFIRMA_UI_SCANNING);

    // Colors
    CHyprColor iconColor = m_accentColor;
    CHyprColor msgColor  = m_textColor;
    if (state == SAFIRMA_UI_APPROVED) { iconColor = m_successColor; msgColor = m_successColor; }
    if (state == SAFIRMA_UI_DENIED)   { iconColor = m_errorColor;   msgColor = m_errorColor; }
    if (state == SAFIRMA_UI_EXPIRED)  { iconColor = m_accentColor;  msgColor = m_accentColor; }
    if (state == SAFIRMA_UI_ERROR)    { iconColor = m_errorColor;   msgColor = m_errorColor; }

    // ── PASS 1: measure all text ──
    const double ICON_SIZE  = 96;
    const double MSG_SIZE   = 16;
    const double TIMER_SIZE = 30;
    const double BTN_W      = 140;
    const double BTN_H      = 36;

    // SCANNING → custom Cairo fingerprint; other states → emoji
    auto iconTex  = (state == SAFIRMA_UI_SCANNING) ? renderFingerprintIcon(ICON_SIZE, iconColor) :
                    iconStr.empty()                ? CachedText{} :
                    getOrCreateText(iconStr, ICON_SIZE, iconColor);
    auto msgTex   = msgStr.empty()  ? CachedText{} : getOrCreateText(msgStr, MSG_SIZE, msgColor);
    auto cancelTex = getOrCreateText("Cancel", 15, m_buttonText);
    auto hintTex  = getOrCreateText("Tap to dismiss", 12, m_accentColor);

    // Timer
    int  remaining = 30;
    auto timerTex  = CachedText{};
    if (hasTimer) {
        auto elapsed = std::chrono::steady_clock::now() - m_stateStart;
        remaining    = 30 - (int)std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        if (remaining < 0) remaining = 0;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", remaining / 60, remaining % 60);
        CHyprColor timerColor = (remaining <= 10) ? m_errorColor : m_accentColor;
        timerTex = getOrCreateText(buf, TIMER_SIZE, timerColor);
    }

    const bool hasIcon  = iconTex.texture.m_iTexID > 0;
    const bool hasMsg   = msgTex.texture.m_iTexID > 0;
    const bool hasTimerTex = timerTex.texture.m_iTexID > 0;
    const double iconH  = hasIcon  ? iconTex.height : 0;
    const double msgH   = hasMsg   ? msgTex.height : 0;
    const double timerH = hasTimerTex ? timerTex.height : 0;
    const double hintH  = hasHint  ? hintTex.height : 0;

    double iconGap, msgGap, timerGap, btnGap, hintGap;
    measureModalContentHeight(state, hasIcon, iconH, hasMsg, msgH,
                               hasTimerTex, timerH, hasCancel, BTN_H,
                               hasHint, hintH,
                               iconGap, msgGap, timerGap, btnGap, hintGap);
    // ── PASS 2: render ──
    const double VPAD = 36;
    double       y    = 0;

        // Cancel button: anchored to card bottom
    if (hasCancel) {
        double by = PY + PANEL_H - VPAD - BTN_H;
        double bx = PX + (PANEL_W - BTN_W) / 2.0;
        CBox btnBox{bx, by, BTN_W, BTN_H};

        CHyprColor btnBg = m_buttonBg;
        btnBg.a *= data.opacity;
        g_pRenderer->renderRect(btnBox, btnBg, 8);

        CHyprColor btnBorder = m_panelBorder;
        btnBorder.a *= data.opacity;
        CBox btnBorderBox{bx - 1, by - 1, BTN_W + 2, BTN_H + 2};
        g_pRenderer->renderBorder(btnBorderBox, CGradientValueData(btnBorder), 1, 9, btnBorder.a);

        if (cancelTex.texture.m_iTexID > 0) {
            double tx = bx + (BTN_W - cancelTex.width) / 2.0;
            double ty = by + (BTN_H - cancelTex.height) / 2.0;
            g_pRenderer->renderTexture({tx, ty, cancelTex.width, cancelTex.height}, cancelTex.texture, data.opacity, 0);
        }

        m_buttonBox = btnBox;
        m_cancelBox = CBox{Vector2D{bx, m_viewport.y - by - BTN_H}, Vector2D{BTN_W, BTN_H}};
    }

    // Icon + message + timer: centered in the space above the button
    {
        // Available height for content block
        double contentTop = PY + VPAD;
        double contentBot = hasCancel ? (PY + PANEL_H - VPAD - BTN_H - 18) : (PY + PANEL_H - VPAD);
        double contentH   = contentBot - contentTop;

        double blockH = 0;
        if (hasIcon) blockH += iconH;
        if (hasMsg) {
            if (blockH > 0) blockH += iconGap;
            blockH += msgH;
        }
        if (hasTimerTex) {
            if (blockH > 0) blockH += msgGap;
            blockH += timerH;
        }
        if (hasHint) {
            if (blockH > 0) blockH += hintGap;
            blockH += hintH;
        }

        y = contentTop + (contentH - blockH) / 2.0;
        if (y < contentTop) y = contentTop;
    }

    // Icon
    if (hasIcon) {
        double ix = PX + (PANEL_W - iconTex.width) / 2.0;
        g_pRenderer->renderTexture({ix, y, iconTex.width, iconTex.height}, iconTex.texture, data.opacity, 0);
        y += iconH;
    }

    // Message
    if (hasMsg) {
        if (hasIcon) y += iconGap;
        double mx = PX + (PANEL_W - msgTex.width) / 2.0;
        g_pRenderer->renderTexture({mx, y, msgTex.width, msgTex.height}, msgTex.texture, data.opacity, 0);
        y += msgH;
    }

    // Timer
    if (hasTimerTex) {
        y += msgGap;
        double tx = PX + (PANEL_W - timerTex.width) / 2.0;
        g_pRenderer->renderTexture({tx, y, timerTex.width, timerTex.height}, timerTex.texture, data.opacity, 0);
        y += timerH;
    }

    // Hint (Tap to dismiss)
    if (hasHint) {
        y += hintGap;
        double hx = PX + (PANEL_W - hintTex.width) / 2.0;
        g_pRenderer->renderTexture({hx, y, hintTex.width, hintTex.height}, hintTex.texture, data.opacity * 0.6, 0);
    }
}

// ─── Cursor handling ─────────────────────────────────────────────────────

void CSafirmaOverlay::updateCursor(const Vector2D& wlPos) {
    if (!g_pSeatManager || !g_pSeatManager->m_pCursorShape)
        return;

    Vector2D localPos = wlPos * m_fractionalScale;
    localPos.y        = m_viewport.y - localPos.y;

    auto setPointer = [this]() { g_pSeatManager->m_pCursorShape->setShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER); };
    auto setDefault = [this]() { g_pSeatManager->m_pCursorShape->setShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT); };

    if (m_demoMode) {
        if (m_state == SAFIRMA_UI_IDLE) { // pill button → pointer
            setPointer();
            return;
        }

        if (m_state == SAFIRMA_UI_SCANNING) { // only Cancel button → pointer
            if (m_buttonBox.containsPoint(localPos))
                setPointer();
            else
                setDefault();
            return;
        }

        // APPROVED, DENIED, EXPIRED, ERROR: clickable → pointer
        setPointer();
        return;
    }

    // Non-demo: real auth mode
    if (!g_pSafirmaAuth) {
        setDefault();
        return;
    }

    auto authState = g_pSafirmaAuth->getState();

    if (authState == SAFIRMA_IDLE) { // pill button
        setPointer();
        return;
    }

    if (authState == SAFIRMA_WAITING || authState == SAFIRMA_CONNECTING) { // only Cancel button
        if (m_buttonBox.containsPoint(localPos))
            setPointer();
        else
            setDefault();
        return;
    }

    // Other states: clickable
    setPointer();
}

void CSafirmaOverlay::onHover(const Vector2D& pos) {
    updateCursor(pos);
}

// ─── Click handling ───────────────────────────────────────────────────────

void CSafirmaOverlay::onClick(uint32_t button, bool down, const Vector2D& pos) {
    if (!down)
        return;

    // Convert Wayland surface pos (unscaled, y=0 top) → hyprlock logical (scaled, y=0 bottom)
    Vector2D localPos = pos * m_fractionalScale;
    localPos.y = m_viewport.y - localPos.y;

    if (m_demoMode) {
        if (m_state == SAFIRMA_UI_IDLE) {
            Log::logger->log(Log::INFO, "safirma: demo idle clicked → scanning");
            m_state = SAFIRMA_UI_SCANNING;
            startDemoTimer();
            return;
        }

        // SCANNING: only Cancel button dismisses
        if (m_state == SAFIRMA_UI_SCANNING) {
            Log::logger->log(Log::INFO, "safirma: demo cancel button clicked → idle");
            m_state        = SAFIRMA_UI_IDLE;
            m_timerStarted = false;
            return;
        }

        // APPROVED, DENIED, EXPIRED, ERROR: click anywhere to dismiss
        Log::logger->log(Log::INFO, "safirma: demo modal clicked → idle");
        m_state        = SAFIRMA_UI_IDLE;
        m_timerStarted = false;
        return;
    }

    if (!g_pSafirmaAuth)
        return;

    auto authState = g_pSafirmaAuth->getState();

    if (authState == SAFIRMA_IDLE) {
        Log::logger->log(Log::INFO, "safirma: button clicked, starting auth");
        g_pSafirmaAuth->startAuth();
        return;
    }

    if (authState == SAFIRMA_WAITING || authState == SAFIRMA_CONNECTING) {
        // Only Cancel button cancels during waiting
        if (m_buttonBox.containsPoint(localPos))
            g_pSafirmaAuth->cancelAuth();
        return;
    }

    if (authState != SAFIRMA_APPROVED) {
        Log::logger->log(Log::INFO, "safirma: modal clicked, cancelling");
        g_pSafirmaAuth->cancelAuth();
    }
}

CBox CSafirmaOverlay::getBoundingBoxWl() const {
    if (m_demoMode) {
        if (m_state == SAFIRMA_UI_IDLE)
            return CBox{Vector2D{m_buttonBox.x, m_viewport.y - m_buttonBox.y - m_buttonBox.h}, Vector2D{m_buttonBox.w, m_buttonBox.h}};
        if (m_state == SAFIRMA_UI_SCANNING)
            return m_cancelBox;
        return CBox{0, 0, m_viewport.x, m_viewport.y};
    }

    if (!g_pSafirmaAuth)
        return CBox{};

    auto state = g_pSafirmaAuth->getState();
    if (state == SAFIRMA_IDLE)
        return CBox{Vector2D{m_buttonBox.x, m_viewport.y - m_buttonBox.y - m_buttonBox.h}, Vector2D{m_buttonBox.w, m_buttonBox.h}};
    return CBox{0, 0, m_viewport.x, m_viewport.y};
}
