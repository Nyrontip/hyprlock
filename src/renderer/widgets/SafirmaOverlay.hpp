#pragma once

#include "IWidget.hpp"
#include "../../defines.hpp"
#include "../../helpers/Color.hpp"
#include "../Texture.hpp"

#include <chrono>
#include <string>

enum eSafirmaUIState {
    SAFIRMA_UI_IDLE = 0,
    SAFIRMA_UI_SCANNING,
    SAFIRMA_UI_APPROVED,
    SAFIRMA_UI_DENIED,
    SAFIRMA_UI_EXPIRED,
    SAFIRMA_UI_ERROR,
};

class CSafirmaOverlay : public IWidget {
  public:
    CSafirmaOverlay(bool demoMode);
    virtual ~CSafirmaOverlay();

    virtual void configure(const std::unordered_map<std::string, std::any>& prop, const SP<COutput>& pOutput);
    virtual bool draw(const SRenderData& data);
    virtual void onAssetUpdate(ResourceID id, ASP<CTexture> newAsset) {}
    virtual void onHover(const Vector2D& pos);
    virtual void onClick(uint32_t button, bool down, const Vector2D& pos);
    virtual CBox getBoundingBoxWl() const;

  private:
    bool m_demoMode = false;

    // Config-driven styling
    std::string m_idleText;
    std::string m_fontFamily;
    int         m_fontSize       = 16;
    CHyprColor  m_accentColor    = CHyprColor(0xCC945C58);
    CHyprColor  m_panelBg        = CHyprColor(0x661A1A1A);
    CHyprColor  m_panelBorder    = CHyprColor(0x33945C58);
    CHyprColor  m_buttonBg       = CHyprColor(0x66945C58);
    CHyprColor  m_buttonText     = CHyprColor(0xFFFFFFFF);
    CHyprColor  m_successColor   = CHyprColor(0xFF44CC44);
    CHyprColor  m_errorColor     = CHyprColor(0xFFFF4444);
    CHyprColor  m_textColor      = CHyprColor(0xFFFFFFFF);

    // Internal state for demo mode
    eSafirmaUIState    m_state        = SAFIRMA_UI_IDLE;
    int                m_demoStep     = 0;
    std::chrono::steady_clock::time_point m_stateStart;
    bool               m_timerStarted = false;

    // Viewport, scale, and hit areas
    Vector2D m_viewport;
    float    m_fractionalScale = 1.0;
    CBox     m_buttonBox;
    CBox     m_cancelBox;  // Cancel button in Wayland coords (y=0 top) for hit testing
    CBox     m_modalBox;

    // Text texture cache (RGBA textures uploaded via Cairo+Pango)
    struct CachedText {
        CTexture texture;
        int      width  = 0;
        int      height = 0;
    };
    std::unordered_map<size_t, CachedText> m_textCache;

    size_t     hashKey(const std::string& text, int fontSize, uint64_t color);
    CachedText getOrCreateText(const std::string& text, int fontSize, CHyprColor color);
    CachedText renderFingerprintIcon(int iconSize, CHyprColor color);
    void       releaseTextures();

    // Demo state machine
    void advanceDemoState();
    void startDemoTimer();

    // Drawing helpers
    void drawIdle(const SRenderData& data);
    void drawModal(const SRenderData& data, eSafirmaUIState state);

    // Cursor
    void updateCursor(const Vector2D& pos);
};
