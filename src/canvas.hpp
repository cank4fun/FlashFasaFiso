#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace fff {
using Microsoft::WRL::ComPtr;
using Rect = D2D1_RECT_F;
inline Rect box(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
inline bool inside(Rect b, float x, float y) {
    return x >= b.left && x < b.right && y >= b.top && y < b.bottom;
}

enum class Font { tiny, body, strong, title, heading, large, glyph, path, count };

class Canvas {
public:
    Canvas() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                  reinterpret_cast<IUnknown**>(textFactory_.GetAddressOf())))) {
            throw std::runtime_error("Direct2D/DirectWrite initialization failed");
        }
        constexpr float sizes[]{11, 13, 13, 15, 25, 27, 16, 13};
        constexpr DWRITE_FONT_WEIGHT weights[]{DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_NORMAL};
        for (unsigned i = 0; i < static_cast<unsigned>(Font::count); ++i) {
            auto& format = fonts_[i];
            if (FAILED(textFactory_->CreateTextFormat(i == static_cast<unsigned>(Font::glyph) ?
                       L"Segoe MDL2 Assets" : L"Segoe UI", nullptr, weights[i], DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"tr-TR", &format))) {
                throw std::runtime_error("DirectWrite text format failed");
            }
            ComPtr<IDWriteInlineObject> ellipsis;
            textFactory_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            const bool path = i == static_cast<unsigned>(Font::path);
            const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, path ? UINT32(L'\\') : 0u,
                                      path ? 2u : 0u};
            format->SetTrimming(&trim, ellipsis.Get());
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    bool begin(HWND window, UINT dpi) {
        RECT r{};
        GetClientRect(window, &r);
        if (r.right <= 0 || r.bottom <= 0) return false;
        if (!target_) {
            if (FAILED(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                       D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(r.right, r.bottom)), &target_)) ||
                FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0u), &brush_))) {
                release();
                return false;
            }
            target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        target_->BeginDraw();
        return true;
    }

    bool resize(UINT width, UINT height) {
        return !target_ || !width || !height || SUCCEEDED(target_->Resize(D2D1::SizeU(width, height)));
    }
    bool finish() { return SUCCEEDED(target_->EndDraw()); }
    void release() { brush_.Reset(); target_.Reset(); }
    ID2D1HwndRenderTarget* target() const { return target_.Get(); }

    void fill(Rect r, UINT color, float radius = 0) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        brush_->SetColor(D2D1::ColorF(color));
        if (radius) target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
        else target_->FillRectangle(r, brush_.Get());
    }
    void border(Rect r, UINT color, float radius = 0) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        brush_->SetColor(D2D1::ColorF(color));
        if (radius) target_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
        else target_->DrawRectangle(r, brush_.Get());
    }
    void line(float x, float y, float toX, float toY, UINT color, float width = 1) {
        brush_->SetColor(D2D1::ColorF(color));
        target_->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(toX, toY), brush_.Get(), width);
    }
    void text(std::wstring_view value, Rect r, Font font, UINT color, bool centered = false, bool wrap = false) {
        if (value.empty() || r.right <= r.left || r.bottom <= r.top) return;
        auto* format = fonts_[static_cast<unsigned>(font)].Get();
        format->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(wrap && !centered ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush_->SetColor(D2D1::ColorF(color));
        target_->DrawText(value.data(), static_cast<UINT32>(value.size()), format, r, brush_.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

private:
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> textFactory_;
    ComPtr<IDWriteTextFormat> fonts_[static_cast<unsigned>(Font::count)];
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
};
}
