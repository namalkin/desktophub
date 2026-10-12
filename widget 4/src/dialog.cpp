#include "dialog.h"

#include <windowsx.h>

#include <vector>

#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClassName[] = L"DiskVacDialog";
constexpr int kFirstEditId = 2000;

// Логические размеры (96 DPI).
constexpr int kW = 360;
constexpr int kPad = 22;
constexpr int kFieldH = 38;
constexpr int kFieldMultiH = 92;
constexpr int kBtnH = 38;
constexpr int kRadius = 12;

struct FieldUi {
    HWND edit = nullptr;
    int yLabel = 0;
    int yField = 0;
    int fieldH = 0;  // многострочное поле выше обычного
    int yHint = 0;
    bool hasHint = false;
};

struct DialogState {
    DialogSpec* spec = nullptr;
    bool accepted = false;
    int dpi = 96;
    HFONT font = nullptr;
    HBRUSH fieldBrush = nullptr;
    std::vector<FieldUi> ui;
    RECT okBtn{};
    RECT cancelBtn{};
    int hotBtn = 0;    // 0 — ничего, 1 — OK, 2 — отмена
    int errorAt = -1;  // индекс поля с ошибкой
    int yButtons = 0;
    int height = 0;
};

DialogState* StateOf(HWND hwnd) {
    return reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void ComputeLayout(DialogState& st) {
    const int dpi = st.dpi;
    st.ui.assign(st.spec->fields.size(), FieldUi{});

    int y = Sc(58, dpi);
    for (size_t i = 0; i < st.spec->fields.size(); i++) {
        FieldUi& f = st.ui[i];
        f.hasHint = !st.spec->fields[i].hint.empty();
        f.fieldH = Sc(st.spec->fields[i].multiline ? kFieldMultiH : kFieldH, dpi);
        f.yLabel = y;
        y += Sc(20, dpi);
        f.yField = y;
        y += f.fieldH;
        if (f.hasHint) {
            y += Sc(8, dpi);
            f.yHint = y;
            y += Sc(24, dpi);
        } else {
            y += Sc(14, dpi);
        }
    }
    y += Sc(8, dpi);
    st.yButtons = y;
    st.height = y + Sc(kBtnH + kPad, dpi);
}

void PaintDialog(HWND hwnd, DialogState& st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;

    // Двойная буферизация — иначе поля мигают при перерисовке.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(P().dialogBg));

        const int dpi = st.dpi;
        const float pad = static_cast<float>(Sc(kPad, dpi));
        FontFamily family(L"Segoe UI");
        Font fontTitle(&family, ScF(15.0f, dpi), FontStyleBold, UnitPixel);
        Font fontLabel(&family, ScF(11.0f, dpi), FontStyleRegular, UnitPixel);
        Font fontBtn(&family, ScF(13.0f, dpi), FontStyleBold, UnitPixel);

        gfx::DrawText(g, st.spec->title.c_str(), fontTitle, P().textPrimary, pad,
                      static_cast<float>(Sc(22, dpi)));

        for (size_t i = 0; i < st.spec->fields.size(); i++) {
            const DialogField& field = st.spec->fields[i];
            const FieldUi& f = st.ui[i];
            const bool bad = (st.errorAt == static_cast<int>(i));

            gfx::DrawText(g, field.label.c_str(), fontLabel, P().textMuted, pad,
                          static_cast<float>(f.yLabel));

            RectF box(pad, static_cast<float>(f.yField), w - pad * 2, static_cast<float>(f.fieldH));
            gfx::FillRoundRect(g, box, ScF(8.0f, dpi), P().fieldBg);
            gfx::StrokeRoundRect(g, box, ScF(8.0f, dpi), bad ? P().danger : P().fieldBorder, ScF(1.0f, dpi));

            if (bad) {
                gfx::DrawText(g, L"Заполните поле", fontLabel, P().danger, pad,
                              static_cast<float>(f.hasHint ? f.yHint : f.yField + f.fieldH + Sc(4, dpi)));
            } else if (f.hasHint) {
                gfx::DrawText(g, field.hint.c_str(), fontLabel, P().textFaint, pad,
                              static_cast<float>(f.yHint));
            }
        }

        auto drawButton = [&](const RECT& r, const wchar_t* text, bool primary, bool hot) {
            RectF rf(static_cast<float>(r.left), static_cast<float>(r.top),
                     static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
            ARGB bg;
            if (primary) {
                bg = P().accent;
                if (hot) bg = (bg & 0x00FFFFFF) | 0xD0000000;  // при наведении чуть светлее
            } else {
                bg = hot ? P().buttonBgHover : P().buttonBg;
            }
            gfx::FillRoundRect(g, rf, ScF(8.0f, dpi), bg);
            gfx::DrawText(g, text, fontBtn, primary ? P().onAccent : P().textPrimary,
                          rf.X + rf.Width / 2.0f, rf.Y + (rf.Height - ScF(16.0f, dpi)) / 2.0f,
                          gfx::Align::Center);
        };
        drawButton(st.cancelBtn, L"Отмена", false, st.hotBtn == 2);
        drawButton(st.okBtn, st.spec->okLabel.c_str(), true, st.hotBtn == 1);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void Accept(HWND hwnd, DialogState& st) {
    std::vector<std::wstring> values(st.spec->fields.size());
    for (size_t i = 0; i < st.spec->fields.size(); i++) {
        wchar_t buf[1024] = {0};
        GetWindowTextW(st.ui[i].edit, buf, 1024);
        values[i] = TrimSpaces(buf);

        if (st.spec->fields[i].required && values[i].empty()) {
            st.errorAt = static_cast<int>(i);
            InvalidateRect(hwnd, nullptr, FALSE);
            SetFocus(st.ui[i].edit);
            SendMessageW(st.ui[i].edit, EM_SETSEL, 0, -1);
            return;
        }
    }
    for (size_t i = 0; i < st.spec->fields.size(); i++) {
        st.spec->fields[i].value = values[i];
    }
    st.accepted = true;
    DestroyWindow(hwnd);
}

int HitButton(DialogState& st, POINT pt) {
    if (PtInRect(&st.okBtn, pt)) return 1;
    if (PtInRect(&st.cancelBtn, pt)) return 2;
    return 0;
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DialogState* st = StateOf(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;  // не return: DefWindowProc должен доделать создание, включая заголовок окна
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (st) PaintDialog(hwnd, *st);
            return 0;
        case WM_CTLCOLOREDIT: {
            if (!st) break;
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, P().editText);
            SetBkColor(dc, P().editBack);
            return reinterpret_cast<LRESULT>(st->fieldBrush);
        }
        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int hot = HitButton(*st, pt);
            if (hot != st->hotBtn) {
                st->hotBtn = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (st && st->hotBtn) {
                st->hotBtn = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!st) break;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int hit = HitButton(*st, pt);
            if (hit == 1) Accept(hwnd, *st);
            else if (hit == 2) DestroyWindow(hwnd);
            return 0;
        }
        case WM_COMMAND:
            if (!st) break;
            if (LOWORD(wp) == IDOK) { Accept(hwnd, *st); return 0; }
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);  // выходим из локального модального цикла
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureClass(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

bool ShowDialog(HWND owner, HWND anchor, DialogSpec& spec) {
    if (spec.fields.empty()) return false;
    if (!anchor) anchor = owner;

    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!inst) inst = GetModuleHandleW(nullptr);
    EnsureClass(inst);

    DialogState st;
    st.spec = &spec;
    st.dpi = WindowDpi(anchor);
    ComputeLayout(st);

    const int w = Sc(kW, st.dpi);
    const int h = st.height;

    // Центрируем по блоку, из которого позвали, но держим в пределах рабочей области монитора.
    RECT ownerRect{};
    GetWindowRect(anchor, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - w) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - h) / 2;
    HMONITOR mon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        x = max(static_cast<int>(mi.rcWork.left), min(x, static_cast<int>(mi.rcWork.right) - w));
        y = max(static_cast<int>(mi.rcWork.top), min(y, static_cast<int>(mi.rcWork.bottom) - h));
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClassName, spec.title.c_str(),
                                WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, owner, nullptr, inst, &st);
    if (!hwnd) return false;

    // Скруглённые углы без DWM — работает одинаково на Windows 10 и 11.
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, Sc(kRadius * 2, st.dpi), Sc(kRadius * 2, st.dpi));
    SetWindowRgn(hwnd, rgn, TRUE);

    st.fieldBrush = CreateSolidBrush(P().editBack);
    st.font = CreateFontW(-Sc(15, st.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          L"Segoe UI");

    const int pad = Sc(kPad, st.dpi);
    const int inset = Sc(10, st.dpi);
    const int lineH = Sc(20, st.dpi);

    for (size_t i = 0; i < spec.fields.size(); i++) {
        const bool multi = spec.fields[i].multiline;
        // Однострочное поле центрируем в рамке, многострочное — растягиваем с отступом.
        const int editH = multi ? st.ui[i].fieldH - Sc(16, st.dpi) : lineH;
        const int editY = multi ? st.ui[i].yField + Sc(8, st.dpi)
                                : st.ui[i].yField + (st.ui[i].fieldH - editH) / 2;
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                            (multi ? (ES_MULTILINE | ES_AUTOVSCROLL) : ES_AUTOHSCROLL);
        st.ui[i].edit = CreateWindowExW(0, L"EDIT", spec.fields[i].value.c_str(), style, pad + inset, editY,
                                        w - pad * 2 - inset * 2, editH, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kFirstEditId + i)),
                                        inst, nullptr);
        SendMessageW(st.ui[i].edit, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    }

    const int btnW = Sc(120, st.dpi);
    const int btnH = Sc(kBtnH, st.dpi);
    st.cancelBtn = {pad, st.yButtons, pad + btnW, st.yButtons + btnH};
    st.okBtn = {w - pad - btnW, st.yButtons, w - pad, st.yButtons + btnH};

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (!st.ui.empty() && st.ui[0].edit) {
        SetFocus(st.ui[0].edit);
        SendMessageW(st.ui[0].edit, EM_SETSEL, 0, -1);
    }

    EnableWindow(owner, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);

    if (st.font) DeleteObject(st.font);
    if (st.fieldBrush) DeleteObject(st.fieldBrush);
    return st.accepted;
}
