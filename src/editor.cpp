#include "editor.h"

#include "lexer.h"

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <set>

namespace {

const wchar_t kClass[] = L"TetradkataEdit";
constexpr UINT_PTR kBlinkTimer = 1, kDragTimer = 2;
constexpr int kCompletionId = 1;

bool IsWordChar(wchar_t c) { return c == L'_' || iswalnum(c); }

int CharClass(wchar_t c) {
    if (c == L' ' || c == L'\t') return 0;
    return IsWordChar(c) ? 1 : 2;
}

bool IsWide(wchar_t c) {
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
           (c >= 0xFFE0 && c <= 0xFFE6);
}

COLORREF Blend(COLORREF a, COLORREF b, int alpha) {
    auto mix = [alpha](int x, int y) { return (x * (255 - alpha) + y * alpha) / 255; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

void FillSolid(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
}

std::wstring ToCRLF(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + s.size() / 20);
    for (wchar_t c : s) {
        if (c == L'\n') out += L'\r';
        out += c;
    }
    return out;
}

std::wstring FromClipboardText(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\r') {
            out += L'\n';
            if (i + 1 < s.size() && s[i + 1] == L'\n') i++;
        } else if (s[i] != 0) {
            out += s[i];
        }
    }
    return out;
}

bool SetClipboard(HWND owner, const std::wstring& text, UINT extraFormat) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    if (extraFormat) {
        HGLOBAL e = GlobalAlloc(GMEM_MOVEABLE, 1);
        if (e) SetClipboardData(extraFormat, e);
    }
    CloseClipboard();
    return true;
}

bool GetClipboard(HWND owner, std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    bool ok = false;
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            text = p;
            ok = true;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return ok;
}

struct CiLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        int c = _wcsicmp(a.c_str(), b.c_str());
        return c != 0 ? c < 0 : a < b;
    }
};

} // namespace

bool Editor::Register(HINSTANCE hi) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    return RegisterClassExW(&wc) != 0;
}

HWND Editor::Create(HWND parent, int id) {
    return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN, 0, 0,
                           0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), this);
}

LRESULT CALLBACK Editor::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* ed = (Editor*)((CREATESTRUCTW*)lp)->lpCreateParams;
        ed->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ed);
    }
    auto* ed = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!ed || !ed->doc) {
        if (ed && (msg == WM_CREATE || msg == WM_SIZE || msg == WM_DESTROY)) return ed->Handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
    return ed->Handle(msg, wp, lp);
}

int Editor::Scale(int v) const { return MulDiv(v, dpi, 96); }

void Editor::CreateFonts() {
    DestroyFonts();
    int pt = std::max(2, fontSize + zoom);
    int h = -MulDiv(pt, dpi, 72);
    for (int i = 0; i < 4; i++)
        fonts[i] = CreateFontW(h, 0, 0, 0, (i & 1) ? FW_BOLD : FW_NORMAL, (i & 2) ? TRUE : FALSE, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH | FF_MODERN, fontName.c_str());
    smallFont = CreateFontW(h * 3 / 4, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, fonts[0]);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"MMMMMMMMMM", 10, &sz);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    lineH = std::max(4, (int)tm.tmHeight);
    charW = std::max(1, (int)((sz.cx + 5) / 10));
    if (acList) SendMessageW(acList, WM_SETFONT, (WPARAM)fonts[0], TRUE);
}

void Editor::DestroyFonts() {
    for (auto& f : fonts) {
        if (f) DeleteObject(f);
        f = nullptr;
    }
    if (smallFont) DeleteObject(smallFont);
    smallFont = nullptr;
}

void Editor::ApplyOptions() {
    if (!hwnd) return;
    CreateFonts();
    scrollWidth = 0;
    layoutCols = -1;
    if (doc) Changed(true);
}

void Editor::SetDocument(Document* d) {
    HideCompletion();
    doc = d;
    DocumentChanged();
}

void Editor::DocumentChanged() {
    if (!doc) return;
    layoutDoc = nullptr;
    styledDoc = nullptr;
    braceDoc = nullptr;
    scrollWidth = 0;
    doc->caret = doc->Clamp(doc->caret);
    doc->anchor = doc->Clamp(doc->anchor);
    lastModified = doc->IsModified();
    UpdateMargins();
    EnsureLayout();
    SetTopRow(doc->topRow);
    Changed(true);
}

void Editor::Invalidate() {
    bufferValid = false;
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- layout

int Editor::CellWidth(wchar_t c, int cell) const {
    if (c == L'\t') return tabWidth - (cell % tabWidth);
    if (IS_LOW_SURROGATE(c)) return 0;
    if (IS_HIGH_SURROGATE(c) || IsWide(c)) return 2;
    return 1;
}

int Editor::CellAt(const std::wstring& s, int col) const {
    int cell = 0;
    int n = std::min(col, (int)s.size());
    for (int i = 0; i < n; i++) cell += CellWidth(s[i], cell);
    return cell;
}

void Editor::UpdateMargins() {
    int digits = 1;
    for (int n = doc->LineCount(); n >= 10; n /= 10) digits++;
    digits = std::max(3, digits);
    marginNumbersW = lineNumbers ? (digits + 1) * charW + Scale(4) : 0;
    marginBookmarkW = bookmarkMargin ? Scale(16) : 0;
    marginGapW = Scale(4);
}

void Editor::ComputeBreaks(const std::wstring& s, int cols, std::vector<int>& out) const {
    out.clear();
    int segStart = 0, segStartCell = 0, lastSpace = -1, lastSpaceCell = 0, cell = 0;
    int len = (int)s.size();
    for (int c = 0; c < len; c++) {
        int w = CellWidth(s[c], cell);
        if (w > 0 && cell + w - segStartCell > cols && c > segStart) {
            if (lastSpace > segStart && lastSpace <= c) {
                segStart = lastSpace;
                segStartCell = lastSpaceCell;
            } else {
                segStart = c;
                segStartCell = cell;
            }
            out.push_back(segStart);
            lastSpace = -1;
        }
        cell += w;
        if (s[c] == L' ' || s[c] == L'\t') {
            lastSpace = c + 1;
            lastSpaceCell = cell;
        }
    }
}

void Editor::EnsureLayout() {
    int cols = wordWrap ? std::max(8, (clientW - TextLeft()) / charW - 1) : 0;
    if (layoutDoc == doc && layoutVersion == doc->version && layoutCols == cols && layoutWrap == wordWrap) return;
    layoutDoc = doc;
    layoutVersion = doc->version;
    layoutCols = cols;
    layoutWrap = wordWrap;
    int n = doc->LineCount();
    if (!wordWrap) {
        breaks.clear();
        rowStart.clear();
        totalRows = n;
        return;
    }
    breaks.resize(n);
    rowStart.resize(n + 1);
    int row = 0;
    for (int i = 0; i < n; i++) {
        rowStart[i] = row;
        ComputeBreaks(doc->lines[i], cols, breaks[i]);
        row += 1 + (int)breaks[i].size();
    }
    rowStart[n] = row;
    totalRows = row;
}

int Editor::LineToRow(int line) const {
    if (!wordWrap) return line;
    if (line < 0) return 0;
    if (line >= (int)rowStart.size()) return totalRows;
    return rowStart[line];
}

void Editor::RowToLine(int row, int& line, int& sub) const {
    int n = doc->LineCount();
    if (!wordWrap || rowStart.empty()) {
        line = std::clamp(row, 0, n - 1);
        sub = 0;
        return;
    }
    row = std::clamp(row, 0, std::max(0, totalRows - 1));
    auto it = std::upper_bound(rowStart.begin(), rowStart.begin() + n, row);
    line = std::clamp((int)(it - rowStart.begin()) - 1, 0, n - 1);
    sub = row - rowStart[line];
}

int Editor::SubOf(Pos p) const {
    if (!wordWrap || p.line >= (int)breaks.size()) return 0;
    const auto& b = breaks[p.line];
    return (int)(std::upper_bound(b.begin(), b.end(), p.col) - b.begin());
}

int Editor::SegCount(int line) const {
    if (!wordWrap || line >= (int)breaks.size()) return 1;
    return 1 + (int)breaks[line].size();
}

void Editor::SegRange(int line, int sub, int& a, int& b) const {
    int len = (int)doc->lines[line].size();
    if (!wordWrap || line >= (int)breaks.size()) {
        a = 0;
        b = len;
        return;
    }
    const auto& br = breaks[line];
    sub = std::clamp(sub, 0, (int)br.size());
    a = sub == 0 ? 0 : br[sub - 1];
    b = sub < (int)br.size() ? br[sub] : len;
}

int Editor::ColFromCell(int line, int a, int b, int cellRel) const {
    const std::wstring& s = doc->lines[line];
    int base = CellAt(s, a), cell = base, col = a;
    while (col < b) {
        int w = CellWidth(s[col], cell);
        if (cell - base + w > cellRel) {
            if (cellRel - (cell - base) > w / 2 && w > 0) col++;
            break;
        }
        cell += w;
        col++;
    }
    if (col < (int)s.size() && IS_LOW_SURROGATE(s[col]) && col > 0) col++;
    int sub = 0;
    if (wordWrap && line < (int)breaks.size()) {
        const auto& br = breaks[line];
        sub = (int)(std::upper_bound(br.begin(), br.end(), a) - br.begin());
        if (sub < (int)br.size() && col >= b && b > a) col = b - 1;
    }
    return std::min(col, (int)s.size());
}

Pos Editor::PosFromPoint(int x, int y) const {
    int r = y >= 0 ? y / lineH : -((-y + lineH - 1) / lineH);
    int row = std::clamp(doc->topRow + r, 0, std::max(0, totalRows - 1));
    int line, sub;
    RowToLine(row, line, sub);
    int a, b;
    SegRange(line, sub, a, b);
    int px = x - TextLeft() + (wordWrap ? 0 : doc->xOffset);
    int cellRel = px < 0 ? 0 : px / charW;
    // Pick the nearest boundary by half cells.
    const std::wstring& s = doc->lines[line];
    int base = CellAt(s, a), cell = base, col = a;
    while (col < b) {
        int w = CellWidth(s[col], cell);
        if (px < (cell - base) * charW + (w * charW) / 2) break;
        cell += w;
        col++;
        if (col < b && IS_LOW_SURROGATE(s[col])) col++;
    }
    (void)cellRel;
    if (wordWrap && sub < SegCount(line) - 1 && col >= b && b > a) col = b - 1;
    return {line, col};
}

bool Editor::PosToPoint(Pos p, int& x, int& y) {
    EnsureLayout();
    p = doc->Clamp(p);
    int sub = SubOf(p);
    int row = LineToRow(p.line) + sub;
    int a, b;
    SegRange(p.line, sub, a, b);
    const std::wstring& s = doc->lines[p.line];
    y = (row - doc->topRow) * lineH;
    x = TextLeft() - (wordWrap ? 0 : doc->xOffset) + (CellAt(s, p.col) - CellAt(s, a)) * charW;
    return y > -lineH && y < clientH;
}

int Editor::VisibleRows() const { return std::max(1, clientH / std::max(1, lineH)); }

int Editor::FirstVisibleLine() {
    EnsureLayout();
    int line, sub;
    RowToLine(doc->topRow, line, sub);
    return line;
}

void Editor::EnsureLexed(int line) {
    if ((int)doc->lexState.size() < doc->LineCount() + 1) doc->lexState.resize(doc->LineCount() + 1, 0);
    if (doc->lexValid < 1) doc->lexValid = 1;
    doc->lexState[0] = 0;
    while (doc->lexValid <= line) {
        int i = doc->lexValid - 1;
        doc->lexState[i + 1] = LexLine(doc->lang, doc->lines[i], doc->lexState[i], lexScratch);
        doc->lexValid++;
    }
}

const std::vector<uint8_t>& Editor::LineStyles(int line) {
    if (styledDoc == doc && styledVersion == doc->version && styledLine == line && styledLang == doc->lang)
        return styleBuf;
    EnsureLexed(line);
    LexLine(doc->lang, doc->lines[line], doc->lexState[line], styleBuf);
    styledDoc = doc;
    styledVersion = doc->version;
    styledLine = line;
    styledLang = doc->lang;
    return styleBuf;
}

// ---------------------------------------------------------------- scrolling

void Editor::SetTopRow(int row) {
    EnsureLayout();
    int maxTop = std::max(0, totalRows - VisibleRows());
    row = std::clamp(row, 0, maxTop);
    if (row != doc->topRow) {
        doc->topRow = row;
        HideCompletion();
        Invalidate();
    }
    UpdateScrollBars();
}

void Editor::SetXOffset(int x) {
    int area = std::max(1, clientW - TextLeft());
    int maxX = std::max(0, scrollWidth - area / 2);
    x = std::clamp(x, 0, maxX);
    if (x != doc->xOffset) {
        doc->xOffset = x;
        HideCompletion();
        Invalidate();
    }
    UpdateScrollBars();
}

void Editor::UpdateScrollBars() {
    if (!hwnd || !doc) return;
    SCROLLINFO si{sizeof(si)};
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = std::max(0, totalRows - 1);
    si.nPage = VisibleRows();
    si.nPos = doc->topRow;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    bool hasH = GetWindowLongW(hwnd, GWL_STYLE) & WS_HSCROLL;
    if (wordWrap) {
        if (hasH) ShowScrollBar(hwnd, SB_HORZ, FALSE);
        return;
    }
    if (!hasH) ShowScrollBar(hwnd, SB_HORZ, TRUE);
    int area = std::max(1, clientW - TextLeft());
    si.nMax = std::max(scrollWidth, area);
    si.nPage = area;
    si.nPos = doc->xOffset;
    SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);
}

void Editor::ScrollCaretIntoView(bool center) {
    EnsureLayout();
    Pos c = doc->caret;
    int row = LineToRow(c.line) + SubOf(c);
    int vis = VisibleRows();
    int top = doc->topRow;
    if (center) {
        if (row < top || row >= top + vis) top = row - vis / 2;
    } else if (row < top) {
        top = row;
    } else if (row >= top + vis) {
        top = row - vis + 1;
    }
    SetTopRow(top);
    if (!wordWrap) {
        const std::wstring& s = doc->lines[c.line];
        int x = CellAt(s, c.col) * charW;
        int area = std::max(charW * 4, clientW - TextLeft());
        if (x + charW * 2 > scrollWidth) scrollWidth = x + charW * 8;
        int off = doc->xOffset;
        if (x < off)
            off = std::max(0, x - area / 3);
        else if (x > off + area - charW * 2)
            off = x - area + area / 3;
        SetXOffset(off);
    }
}

void Editor::Changed(bool text) {
    if (!doc) return;
    if (text) {
        UpdateMargins();
        EnsureLayout();
        int maxTop = std::max(0, totalRows - VisibleRows());
        if (doc->topRow > maxTop) doc->topRow = maxTop;
        UpdateScrollBars();
    }
    Invalidate();
    RestartBlink();
    if (onUpdateUI) onUpdateUI();
    bool mod = doc->IsModified();
    if (mod != lastModified) {
        lastModified = mod;
        if (onSavePointChanged) onSavePointChanged();
    }
}

void Editor::RestartBlink() {
    caretOn = true;
    if (!hwnd) return;
    if (focused) {
        UINT t = GetCaretBlinkTime();
        if (t == INFINITE || t == 0) t = 530;
        SetTimer(hwnd, kBlinkTimer, t, nullptr);
    }
}

// ---------------------------------------------------------------- selection & movement

void Editor::GetSel(Pos& a, Pos& b) const {
    a = doc->anchor;
    b = doc->caret;
    if (b < a) std::swap(a, b);
}

void Editor::SetSel(Pos anchor, Pos caret, bool scroll, bool center) {
    doc->BreakTyping();
    doc->anchor = doc->Clamp(anchor);
    doc->caret = doc->Clamp(caret);
    doc->desiredCell = -1;
    if (scroll) ScrollCaretIntoView(center);
    Changed(false);
}

void Editor::GotoLine(int line) {
    line = std::clamp(line, 0, doc->LineCount() - 1);
    SetSel({line, 0}, {line, 0}, true, true);
}

void Editor::MoveTo(Pos p, bool extend, bool keepDesired) {
    HideCompletion();
    doc->BreakTyping();
    p = doc->Clamp(p);
    doc->caret = p;
    if (!extend) doc->anchor = p;
    if (!keepDesired) doc->desiredCell = -1;
    ScrollCaretIntoView();
    Changed(false);
}

void Editor::Place(Pos p) {
    doc->caret = doc->anchor = doc->Clamp(p);
    doc->desiredCell = -1;
    ScrollCaretIntoView();
    Changed(true);
}

Pos Editor::Left(Pos p) const {
    if (p.col > 0) {
        const std::wstring& s = doc->lines[p.line];
        p.col--;
        if (p.col > 0 && IS_LOW_SURROGATE(s[p.col]) && IS_HIGH_SURROGATE(s[p.col - 1])) p.col--;
        return p;
    }
    if (p.line > 0) return {p.line - 1, (int)doc->lines[p.line - 1].size()};
    return p;
}

Pos Editor::Right(Pos p) const {
    const std::wstring& s = doc->lines[p.line];
    if (p.col < (int)s.size()) {
        p.col++;
        if (p.col < (int)s.size() && IS_LOW_SURROGATE(s[p.col])) p.col++;
        return p;
    }
    if (p.line + 1 < doc->LineCount()) return {p.line + 1, 0};
    return p;
}

Pos Editor::WordLeft(Pos p) const {
    if (p.col == 0) return p.line > 0 ? Pos{p.line - 1, (int)doc->lines[p.line - 1].size()} : p;
    const std::wstring& s = doc->lines[p.line];
    int i = p.col;
    while (i > 0 && CharClass(s[i - 1]) == 0) i--;
    if (i > 0) {
        int cls = CharClass(s[i - 1]);
        while (i > 0 && CharClass(s[i - 1]) == cls) i--;
    }
    return {p.line, i};
}

Pos Editor::WordRight(Pos p) const {
    const std::wstring& s = doc->lines[p.line];
    int len = (int)s.size();
    if (p.col >= len) return p.line + 1 < doc->LineCount() ? Pos{p.line + 1, 0} : p;
    int i = p.col;
    int cls = CharClass(s[i]);
    while (i < len && CharClass(s[i]) == cls) i++;
    while (i < len && CharClass(s[i]) == 0) i++;
    return {p.line, i};
}

void Editor::MoveVertical(int rows, bool extend) {
    EnsureLayout();
    Pos c = doc->caret;
    int sub = SubOf(c);
    int row = LineToRow(c.line) + sub;
    int a, b;
    SegRange(c.line, sub, a, b);
    const std::wstring& s = doc->lines[c.line];
    if (doc->desiredCell < 0) doc->desiredCell = CellAt(s, c.col) - CellAt(s, a);
    int nr = std::clamp(row + rows, 0, std::max(0, totalRows - 1));
    int line, nsub;
    RowToLine(nr, line, nsub);
    SegRange(line, nsub, a, b);
    int col = ColFromCell(line, a, b, doc->desiredCell);
    MoveTo({line, col}, extend, true);
}

std::wstring Editor::GetSelText() const {
    Pos a, b;
    GetSel(a, b);
    return doc->GetText(a, b);
}

bool Editor::WordAt(Pos p, Pos& start, Pos& end) const {
    p = doc->Clamp(p);
    const std::wstring& s = doc->lines[p.line];
    int a = p.col, b = p.col;
    while (a > 0 && IsWordChar(s[a - 1])) a--;
    while (b < (int)s.size() && IsWordChar(s[b])) b++;
    if (a == b) {
        // Select a run of punctuation or spaces instead, like a double click in Scintilla.
        if (p.col < (int)s.size()) {
            int cls = CharClass(s[p.col]);
            a = p.col;
            b = p.col;
            while (a > 0 && CharClass(s[a - 1]) == cls) a--;
            while (b < (int)s.size() && CharClass(s[b]) == cls) b++;
        }
        if (a == b) return false;
    }
    start = {p.line, a};
    end = {p.line, b};
    return true;
}

std::wstring Editor::CurrentWord() const {
    if (HasSelection()) {
        Pos a, b;
        GetSel(a, b);
        if (a.line == b.line) return doc->GetText(a, b);
        return L"";
    }
    const std::wstring& s = doc->lines[doc->caret.line];
    int a = doc->caret.col, b = a;
    while (a > 0 && IsWordChar(s[a - 1])) a--;
    while (b < (int)s.size() && IsWordChar(s[b])) b++;
    return s.substr(a, b - a);
}

bool Editor::Editable() const { return doc && !doc->readOnly && !doc->fileReadOnly && !doc->monitoring; }

// ---------------------------------------------------------------- editing

void Editor::ReplaceSel(const std::wstring& text, bool typing) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    if (a == b && overwrite && typing && text.size() <= 2 && a.col < (int)doc->lines[a.line].size()) {
        b = Right(a);
        if (b.line != a.line) b = a;
    }
    Pos e;
    if (a != b || !typing) {
        doc->BeginUndoGroup(doc->anchor, doc->caret);
        doc->Erase(a, b);
        e = doc->Insert(a, text);
        doc->EndUndoGroup();
    } else {
        e = doc->Insert(a, text, true);
    }
    Place(e);
}

void Editor::ReplaceRange(Pos a, Pos b, const std::wstring& text, bool selectResult) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    if (b < a) std::swap(a, b);
    doc->BeginUndoGroup(doc->anchor, doc->caret);
    doc->Erase(a, b);
    Pos e = doc->Insert(a, text);
    doc->EndUndoGroup();
    if (selectResult) {
        doc->anchor = doc->Clamp(a);
        doc->caret = e;
        doc->desiredCell = -1;
        ScrollCaretIntoView();
        Changed(true);
    } else {
        Place(e);
    }
}

void Editor::Backspace(bool word) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    if (HasSelection()) {
        ReplaceSel(L"");
        return;
    }
    Pos c = doc->caret;
    if (c.line == 0 && c.col == 0) return;
    Pos from = word ? WordLeft(c) : Left(c);
    doc->Erase(from, c, !word && from.line == c.line);
    Place(from);
    if (acList && IsWindowVisible(acList)) ShowCompletion(true);
}

void Editor::DeleteForward(bool word) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    if (HasSelection()) {
        ReplaceSel(L"");
        return;
    }
    Pos c = doc->caret;
    Pos to = word ? WordRight(c) : Right(c);
    if (to == c) return;
    doc->Erase(c, to, !word && to.line == c.line);
    Place(c);
}

void Editor::Clear() {
    if (HasSelection())
        ReplaceSel(L"");
    else
        DeleteForward(false);
}

void Editor::SelectAll() { SetSel({0, 0}, doc->EndPos(), false); }

void Editor::Copy() {
    if (HasSelection()) {
        SetClipboard(hwnd, ToCRLF(GetSelText()), 0);
    } else {
        // Like Scintilla's SCI_COPYALLOWLINE: copy the whole line and mark it as a line copy.
        SetClipboard(hwnd, ToCRLF(doc->lines[doc->caret.line] + L"\n"), lineSelectFormat);
    }
}

void Editor::Cut() {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Copy();
    if (HasSelection())
        ReplaceSel(L"");
    else
        DeleteLine();
}

void Editor::Paste() {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    std::wstring t;
    if (!GetClipboard(hwnd, t)) return;
    t = FromClipboardText(t);
    if (t.empty()) return;
    bool lineMode = lineSelectFormat && IsClipboardFormatAvailable(lineSelectFormat) && !HasSelection() &&
                    !t.empty() && t.back() == L'\n';
    if (lineMode) {
        Pos c = doc->caret;
        doc->BeginUndoGroup(doc->anchor, doc->caret);
        Pos e = doc->Insert({c.line, 0}, t);
        doc->EndUndoGroup();
        Place({e.line, c.col});
        return;
    }
    ReplaceSel(t);
}

void Editor::Undo() {
    Pos a, c;
    if (!Editable() || !doc->Undo(a, c)) return;
    doc->anchor = doc->Clamp(a);
    doc->caret = doc->Clamp(c);
    doc->desiredCell = -1;
    ScrollCaretIntoView();
    Changed(true);
}

void Editor::Redo() {
    Pos a, c;
    if (!Editable() || !doc->Redo(a, c)) return;
    doc->anchor = doc->Clamp(a);
    doc->caret = doc->Clamp(c);
    doc->desiredCell = -1;
    ScrollCaretIntoView();
    Changed(true);
}

void Editor::InsertNewline() {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    std::wstring t = L"\n";
    if (autoIndent) {
        Pos a, b;
        GetSel(a, b);
        const std::wstring& s = doc->lines[a.line];
        int i = 0;
        while (i < a.col && i < (int)s.size() && (s[i] == L' ' || s[i] == L'\t')) i++;
        t += s.substr(0, i);
    }
    ReplaceSel(t);
}

void Editor::InsertTab(bool shift) {
    Pos a, b;
    GetSel(a, b);
    if (a.line != b.line || shift) {
        Indent(shift);
        return;
    }
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    std::wstring t = L"\t";
    if (useSpaces) {
        int cell = CellAt(doc->lines[a.line], a.col);
        t.assign(tabWidth - cell % tabWidth, L' ');
    }
    ReplaceSel(t, true);
}

static void SelectedLineRange(const Document* d, Pos a, Pos b, int& l0, int& l1) {
    l0 = a.line;
    l1 = b.line;
    if (b.line > a.line && b.col == 0) l1--;
    (void)d;
}

void Editor::TransformLines(const std::function<void(std::vector<std::wstring>&)>& fn, bool wholeDocIfSingleLine) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    int l0, l1;
    bool whole = a.line == b.line && wholeDocIfSingleLine;
    if (whole) {
        l0 = 0;
        l1 = doc->LineCount() - 1;
    } else {
        SelectedLineRange(doc, a, b, l0, l1);
    }
    std::vector<std::wstring> v(doc->lines.begin() + l0, doc->lines.begin() + l1 + 1);
    std::vector<std::wstring> orig = v;
    fn(v);
    if (v == orig) return;
    doc->BeginUndoGroup(doc->anchor, doc->caret);
    Pos from{l0, 0}, to{l1, (int)doc->lines[l1].size()};
    if (v.empty()) {
        if (l1 + 1 < doc->LineCount())
            to = {l1 + 1, 0};
        else if (l0 > 0)
            from = {l0 - 1, (int)doc->lines[l0 - 1].size()};
        doc->Erase(from, to);
        doc->EndUndoGroup();
        Place(from);
        return;
    }
    std::wstring text;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) text += L'\n';
        text += v[i];
    }
    doc->Erase(from, to);
    Pos e = doc->Insert(from, text);
    doc->EndUndoGroup();
    if (whole) {
        Place(doc->Clamp(doc->caret));
    } else {
        doc->anchor = from;
        doc->caret = e;
        doc->desiredCell = -1;
        ScrollCaretIntoView();
        Changed(true);
    }
}

void Editor::TransformSelection(const std::function<std::wstring(const std::wstring&)>& fn) {
    if (!HasSelection()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    std::wstring t = doc->GetText(a, b);
    std::wstring r = fn(t);
    if (r == t) return;
    bool forward = doc->anchor < doc->caret;
    ReplaceRange(a, b, r, true);
    if (!forward) std::swap(doc->anchor, doc->caret);
    Changed(false);
}

void Editor::DuplicateLine() {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    if (HasSelection()) {
        Pos a, b;
        GetSel(a, b);
        Pos oa = doc->anchor, oc = doc->caret;
        std::wstring t = doc->GetText(a, b);
        doc->BeginUndoGroup(oa, oc);
        doc->Insert(b, t);
        doc->EndUndoGroup();
        doc->anchor = oa;
        doc->caret = oc;
        Changed(true);
        return;
    }
    Pos c = doc->caret;
    doc->BeginUndoGroup(doc->anchor, doc->caret);
    doc->Insert({c.line, (int)doc->lines[c.line].size()}, L"\n" + doc->lines[c.line]);
    doc->EndUndoGroup();
    Place(c);
}

void Editor::MoveLines(int dir) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    int l0, l1;
    SelectedLineRange(doc, a, b, l0, l1);
    if ((dir < 0 && l0 == 0) || (dir > 0 && l1 >= doc->LineCount() - 1)) return;
    int r0 = dir < 0 ? l0 - 1 : l0, r1 = dir < 0 ? l1 : l1 + 1;
    std::vector<std::wstring> v(doc->lines.begin() + r0, doc->lines.begin() + r1 + 1);
    std::vector<uint8_t> bm(doc->bookmarks.begin() + r0, doc->bookmarks.begin() + r1 + 1);
    if (dir < 0) {
        std::rotate(v.begin(), v.begin() + 1, v.end());
        std::rotate(bm.begin(), bm.begin() + 1, bm.end());
    } else {
        std::rotate(v.rbegin(), v.rbegin() + 1, v.rend());
        std::rotate(bm.rbegin(), bm.rbegin() + 1, bm.rend());
    }
    std::wstring text;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) text += L'\n';
        text += v[i];
    }
    Pos oa = doc->anchor, oc = doc->caret;
    doc->BeginUndoGroup(oa, oc);
    doc->Erase({r0, 0}, {r1, (int)doc->lines[r1].size()});
    doc->Insert({r0, 0}, text);
    doc->EndUndoGroup();
    std::copy(bm.begin(), bm.end(), doc->bookmarks.begin() + r0);
    doc->anchor = doc->Clamp({oa.line + dir, oa.col});
    doc->caret = doc->Clamp({oc.line + dir, oc.col});
    ScrollCaretIntoView();
    Changed(true);
}

void Editor::DeleteLine() {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    int l0, l1;
    SelectedLineRange(doc, a, b, l0, l1);
    Pos from{l0, 0}, to{l1, (int)doc->lines[l1].size()};
    if (l1 + 1 < doc->LineCount())
        to = {l1 + 1, 0};
    else if (l0 > 0)
        from = {l0 - 1, (int)doc->lines[l0 - 1].size()};
    doc->BeginUndoGroup(doc->anchor, doc->caret);
    doc->Erase(from, to);
    doc->EndUndoGroup();
    Place({std::min(l0, doc->LineCount() - 1), 0});
}

void Editor::JoinLines() {
    Pos a, b;
    GetSel(a, b);
    int l0, l1;
    SelectedLineRange(doc, a, b, l0, l1);
    if (l0 == l1) {
        MessageBeep(MB_OK);
        return;
    }
    auto trimRight = [](std::wstring s) {
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
        return s;
    };
    std::wstring out = trimRight(doc->lines[l0]);
    for (int l = l0 + 1; l <= l1; l++) {
        std::wstring t = doc->lines[l];
        size_t i = t.find_first_not_of(L" \t");
        t = i == std::wstring::npos ? L"" : trimRight(t.substr(i));
        if (!t.empty()) out += (out.empty() ? L"" : L" ") + t;
    }
    ReplaceRange({l0, 0}, {l1, (int)doc->lines[l1].size()}, out, false);
}

void Editor::Indent(bool outdent) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    int l0, l1;
    SelectedLineRange(doc, a, b, l0, l1);
    bool multi = l0 != l1;
    std::wstring unit = useSpaces ? std::wstring(tabWidth, L' ') : L"\t";
    Pos oc = doc->caret;
    int caretDelta = 0;
    doc->BeginUndoGroup(doc->anchor, doc->caret);
    for (int l = l0; l <= l1; l++) {
        const std::wstring& s = doc->lines[l];
        if (!outdent) {
            if (s.empty() && multi) continue;
            doc->Insert({l, 0}, unit);
            if (l == oc.line) caretDelta = (int)unit.size();
        } else {
            int n = 0;
            if (!s.empty() && s[0] == L'\t')
                n = 1;
            else
                while (n < tabWidth && n < (int)s.size() && s[n] == L' ') n++;
            if (n) doc->Erase({l, 0}, {l, n});
            if (l == oc.line) caretDelta = -n;
        }
    }
    doc->EndUndoGroup();
    if (multi) {
        bool forward = doc->anchor < doc->caret || doc->anchor == doc->caret;
        Pos s{l0, 0}, e{l1, (int)doc->lines[l1].size()};
        doc->anchor = forward ? s : e;
        doc->caret = forward ? e : s;
    } else {
        Pos c = doc->Clamp({oc.line, std::max(0, oc.col + caretDelta)});
        doc->anchor = doc->caret = c;
    }
    ScrollCaretIntoView();
    Changed(true);
}

std::wstring Editor::CommentToken() const {
    const Language& L = GetLanguage(doc->lang);
    if (L.lineComment) return L.lineComment;
    if (L.kind == LexKind::Batch) return L"REM ";
    if (L.kind == LexKind::Ini) return (wcscmp(L.name, L"INI file") == 0 || wcscmp(L.name, L"Registry") == 0) ? L";" : L"#";
    return L"";
}

void Editor::ToggleComment(int mode) {
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    std::wstring tok = CommentToken();
    if (tok.empty()) {
        BlockComment(mode == 2);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    int l0, l1;
    SelectedLineRange(doc, a, b, l0, l1);
    bool multi = HasSelection();
    Pos oc = doc->caret;
    int delta = 0;
    doc->BeginUndoGroup(doc->anchor, doc->caret);
    for (int l = l0; l <= l1; l++) {
        const std::wstring& s = doc->lines[l];
        size_t ind = s.find_first_not_of(L" \t");
        if (ind == std::wstring::npos) continue;
        bool commented = _wcsnicmp(s.c_str() + ind, tok.c_str(), tok.size()) == 0;
        if ((mode == 0 && commented) || mode == 2) {
            if (commented) {
                doc->Erase({l, (int)ind}, {l, (int)(ind + tok.size())});
                if (l == oc.line) delta = -(int)tok.size();
            }
        } else {
            doc->Insert({l, (int)ind}, tok);
            if (l == oc.line) delta = (int)tok.size();
        }
    }
    doc->EndUndoGroup();
    if (multi) {
        doc->anchor = {l0, 0};
        doc->caret = {l1, (int)doc->lines[l1].size()};
    } else {
        doc->anchor = doc->caret = doc->Clamp({oc.line, std::max(0, oc.col + delta)});
    }
    ScrollCaretIntoView();
    Changed(true);
}

void Editor::BlockComment(bool uncomment) {
    const Language& L = GetLanguage(doc->lang);
    std::wstring open, close;
    if (L.kind == LexKind::Markup || L.kind == LexKind::Markdown) {
        open = L"<!--";
        close = L"-->";
    } else if (L.blockStart && L.blockEnd) {
        open = L.blockStart;
        close = L.blockEnd;
    }
    if (open.empty() || !Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Pos a, b;
    GetSel(a, b);
    if (a == b) {
        const std::wstring& s = doc->lines[a.line];
        size_t ind = s.find_first_not_of(L" \t");
        a = {a.line, ind == std::wstring::npos ? 0 : (int)ind};
        b = {a.line, (int)s.size()};
    }
    std::wstring t = doc->GetText(a, b);
    if (uncomment) {
        size_t s0 = t.find_first_not_of(L" \t\n");
        size_t s1 = t.find_last_not_of(L" \t\n");
        if (s0 == std::wstring::npos || t.compare(s0, open.size(), open) != 0 || s1 + 1 < close.size() ||
            t.compare(s1 + 1 - close.size(), close.size(), close) != 0 || s1 + 1 - close.size() < s0 + open.size()) {
            MessageBeep(MB_OK);
            return;
        }
        t = t.substr(0, s0) + t.substr(s0 + open.size(), s1 + 1 - close.size() - s0 - open.size()) +
            t.substr(s1 + 1);
    } else {
        t = open + t + close;
    }
    ReplaceRange(a, b, t, true);
}

bool Editor::FindBrace(Pos& a, Pos& b, bool& bad) {
    if (braceDoc == doc && braceVersion == doc->version && braceCaret == doc->caret) {
        a = braceA;
        b = braceB;
        bad = braceBad;
        return braceFound;
    }
    braceDoc = doc;
    braceVersion = doc->version;
    braceCaret = doc->caret;
    braceFound = false;
    auto isBrace = [](wchar_t ch) { return ch && wcschr(L"()[]{}", ch); };
    Pos c = doc->caret;
    const std::wstring& s = doc->lines[c.line];
    Pos p;
    if (c.col > 0 && isBrace(s[c.col - 1]))
        p = {c.line, c.col - 1};
    else if (c.col < (int)s.size() && isBrace(s[c.col]))
        p = c;
    else
        return false;
    wchar_t ch = doc->lines[p.line][p.col];
    static const wchar_t open[] = L"([{", close[] = L")]}";
    const wchar_t* o = wcschr(open, ch);
    wchar_t match = o ? close[o - open] : open[wcschr(close, ch) - close];
    int dir = o ? 1 : -1;
    int depth = 0;
    int line = p.line, col = p.col;
    int budget = 2000000;
    bool found = false;
    while (budget-- > 0) {
        const std::wstring& ln = doc->lines[line];
        if (col >= 0 && col < (int)ln.size()) {
            if (ln[col] == ch)
                depth++;
            else if (ln[col] == match && --depth == 0) {
                found = true;
                break;
            }
        }
        col += dir;
        if (col >= (int)ln.size() && dir > 0) {
            if (++line >= doc->LineCount()) break;
            col = 0;
        } else if (col < 0 && dir < 0) {
            if (--line < 0) break;
            col = (int)doc->lines[line].size() - 1;
        }
    }
    braceFound = true;
    braceA = p;
    braceB = found ? Pos{line, col} : p;
    braceBad = !found;
    a = braceA;
    b = braceB;
    bad = braceBad;
    return true;
}

void Editor::GoToMatchingBrace(bool select) {
    Pos a, b;
    bool bad;
    if (!FindBrace(a, b, bad) || bad) return;
    Pos c = doc->caret;
    Pos target = (a == c) ? b : Pos{b.line, b.col + 1};
    if (select) {
        Pos s = a < b ? a : b, e = a < b ? b : a;
        SetSel(s, {e.line, e.col + 1});
    } else {
        MoveTo(target, false);
    }
}

void Editor::SelectBetweenBraces() {
    // Walk backwards from the caret to the innermost unmatched opening bracket.
    static const wchar_t open[] = L"([{", close[] = L")]}";
    int depth[3] = {0, 0, 0};
    Pos c = doc->caret;
    int line = c.line, col = c.col - 1;
    for (int budget = 2000000; budget > 0; budget--) {
        if (col < 0) {
            if (--line < 0) return;
            col = (int)doc->lines[line].size() - 1;
            continue;
        }
        wchar_t ch = doc->lines[line][col];
        if (const wchar_t* k = wcschr(close, ch); k && ch) depth[k - close]++;
        if (const wchar_t* k = wcschr(open, ch); k && ch) {
            if (depth[k - open] == 0) break;
            depth[k - open]--;
        }
        col--;
    }
    Pos o{line, col};
    Pos keep = doc->caret;
    doc->caret = o;
    Pos a, b;
    bool bad;
    braceDoc = nullptr;
    bool ok = FindBrace(a, b, bad);
    doc->caret = keep;
    braceDoc = nullptr;
    if (!ok || bad) return;
    SetSel({o.line, o.col + 1}, b);
}

void Editor::ToggleBookmark(int line) {
    if (line < 0 || line >= doc->LineCount()) return;
    doc->bookmarks[line] = !doc->bookmarks[line];
    Invalidate();
}

void Editor::NextBookmark(bool forward) {
    int n = doc->LineCount();
    for (int i = 1; i <= n; i++) {
        int l = ((doc->caret.line + (forward ? i : -i)) % n + n) % n;
        if (doc->bookmarks[l]) {
            GotoLine(l);
            return;
        }
    }
}

void Editor::ZoomBy(int delta) {
    zoom = std::clamp(zoom + delta, -10, 20);
    ApplyOptions();
}

void Editor::ZoomReset() {
    zoom = 0;
    ApplyOptions();
}

// ---------------------------------------------------------------- word completion

void Editor::HideCompletion() {
    if (acList && IsWindowVisible(acList)) ShowWindow(acList, SW_HIDE);
}

void Editor::ShowCompletion(bool automatic) {
    if (!Editable()) return;
    bool visible = acList && IsWindowVisible(acList);
    Pos c = doc->caret;
    const std::wstring& s = doc->lines[c.line];
    int st = c.col;
    while (st > 0 && IsWordChar(s[st - 1])) st--;
    std::wstring prefix = s.substr(st, c.col - st);
    if (automatic && (int)prefix.size() < (visible ? 1 : 3)) {
        HideCompletion();
        return;
    }
    std::wstring lp = Lower(prefix);
    std::set<std::wstring, CiLess> words;
    size_t scanned = 0;
    for (int l = 0; l < doc->LineCount() && scanned < 4000000 && words.size() < 2000; l++) {
        const std::wstring& t = doc->lines[l];
        scanned += t.size();
        int i = 0, n = (int)t.size();
        while (i < n) {
            if (!IsWordChar(t[i])) {
                i++;
                continue;
            }
            int j = i;
            while (j < n && IsWordChar(t[j])) j++;
            if (j - i >= 2 && !(l == c.line && i == st) && (size_t)(j - i) > lp.size() &&
                _wcsnicmp(t.c_str() + i, lp.c_str(), lp.size()) == 0)
                words.insert(t.substr(i, j - i));
            i = j;
        }
    }
    if (words.empty()) {
        HideCompletion();
        return;
    }
    if (!acList) {
        acList = CreateWindowExW(0, L"LISTBOX", nullptr,
                                 WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0,
                                 hwnd, (HMENU)(INT_PTR)kCompletionId, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(acList, WM_SETFONT, (WPARAM)fonts[0], FALSE);
    }
    SendMessageW(acList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(acList, LB_RESETCONTENT, 0, 0);
    size_t longest = 4;
    for (auto& w : words) {
        SendMessageW(acList, LB_ADDSTRING, 0, (LPARAM)w.c_str());
        longest = std::max(longest, w.size());
    }
    SendMessageW(acList, LB_SETCURSEL, 0, 0);
    SendMessageW(acList, WM_SETREDRAW, TRUE, 0);
    int itemH = (int)SendMessageW(acList, LB_GETITEMHEIGHT, 0, 0);
    int rows = std::min((int)words.size(), 8);
    int w = std::max(Scale(120), (int)std::min<size_t>(longest, 60) * charW + GetSystemMetrics(SM_CXVSCROLL) + Scale(10));
    int h = rows * itemH + Scale(4);
    acStart = {c.line, st};
    int x, y;
    PosToPoint(acStart, x, y);
    y += lineH;
    if (y + h > clientH && y - lineH - h >= 0) y = y - lineH - h;
    x = std::clamp(x, 0, std::max(0, clientW - w));
    SetWindowPos(acList, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

bool Editor::CompletionKey(UINT vk) {
    if (!acList || !IsWindowVisible(acList)) return false;
    int count = (int)SendMessageW(acList, LB_GETCOUNT, 0, 0);
    int sel = (int)SendMessageW(acList, LB_GETCURSEL, 0, 0);
    switch (vk) {
    case VK_UP: sel = std::max(0, sel - 1); break;
    case VK_DOWN: sel = std::min(count - 1, sel + 1); break;
    case VK_PRIOR: sel = std::max(0, sel - 7); break;
    case VK_NEXT: sel = std::min(count - 1, sel + 7); break;
    case VK_RETURN:
    case VK_TAB: AcceptCompletion(); return true;
    case VK_ESCAPE: HideCompletion(); return true;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END: HideCompletion(); return false;
    default: return false;
    }
    SendMessageW(acList, LB_SETCURSEL, sel, 0);
    return true;
}

void Editor::AcceptCompletion() {
    int sel = (int)SendMessageW(acList, LB_GETCURSEL, 0, 0);
    if (sel < 0) {
        HideCompletion();
        return;
    }
    int len = (int)SendMessageW(acList, LB_GETTEXTLEN, sel, 0);
    std::wstring w(len, L'\0');
    SendMessageW(acList, LB_GETTEXT, sel, (LPARAM)&w[0]);
    HideCompletion();
    Pos c = doc->caret;
    if (acStart.line != c.line || acStart.col > c.col) return;
    ReplaceRange(acStart, c, w, false);
    SetFocus(hwnd);
}

// ---------------------------------------------------------------- input

void Editor::Record(MacroKind k, UINT v, UINT mods) {
    if (recording && onMacroRecord) onMacroRecord({k, v, mods});
}

void Editor::ExecuteMacroStep(const MacroStep& s) {
    if (s.kind == MacroKind::Char)
        Char((wchar_t)s.value);
    else if (s.kind == MacroKind::Key)
        KeyDown(s.value, s.mods & 1, s.mods & 2, s.mods & 4);
}

bool Editor::KeyDown(UINT vk, bool shift, bool ctrl, bool alt) {
    if (alt) return false;
    Pos c = doc->caret;
    switch (vk) {
    case VK_LEFT:
        if (HasSelection() && !shift) {
            Pos a, b;
            GetSel(a, b);
            MoveTo(a, false);
        } else {
            MoveTo(ctrl ? WordLeft(c) : Left(c), shift);
        }
        break;
    case VK_RIGHT:
        if (HasSelection() && !shift) {
            Pos a, b;
            GetSel(a, b);
            MoveTo(b, false);
        } else {
            MoveTo(ctrl ? WordRight(c) : Right(c), shift);
        }
        break;
    case VK_UP:
        if (ctrl)
            SetTopRow(doc->topRow - 1);
        else
            MoveVertical(-1, shift);
        break;
    case VK_DOWN:
        if (ctrl)
            SetTopRow(doc->topRow + 1);
        else
            MoveVertical(1, shift);
        break;
    case VK_PRIOR:
    case VK_NEXT: {
        int n = std::max(1, VisibleRows() - 1) * (vk == VK_PRIOR ? -1 : 1);
        SetTopRow(doc->topRow + n);
        MoveVertical(n, shift);
        break;
    }
    case VK_HOME:
        if (ctrl) {
            MoveTo({0, 0}, shift);
        } else {
            const std::wstring& s = doc->lines[c.line];
            int first = 0;
            while (first < (int)s.size() && (s[first] == L' ' || s[first] == L'\t')) first++;
            MoveTo({c.line, c.col == first ? 0 : first}, shift);
        }
        break;
    case VK_END:
        MoveTo(ctrl ? doc->EndPos() : Pos{c.line, (int)doc->lines[c.line].size()}, shift);
        break;
    case VK_BACK: Backspace(ctrl); break;
    case VK_DELETE:
        if (shift && !ctrl)
            Cut();
        else
            DeleteForward(ctrl);
        break;
    case VK_INSERT:
        if (ctrl && !shift)
            Copy();
        else if (shift && !ctrl)
            Paste();
        else if (!ctrl && !shift) {
            overwrite = !overwrite;
            Changed(false);
        }
        break;
    case VK_TAB:
        if (ctrl) return false;
        InsertTab(shift);
        break;
    case VK_RETURN:
        if (ctrl) return false;
        InsertNewline();
        break;
    default: return false;
    }
    Record(MacroKind::Key, vk, (shift ? 1 : 0) | (ctrl ? 2 : 0));
    return true;
}

void Editor::Char(wchar_t ch) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') return; // handled in KeyDown
    if (ch < 32 || ch == 127) return;
    if (IS_HIGH_SURROGATE(ch)) {
        pendingHigh = ch;
        return;
    }
    std::wstring t;
    if (IS_LOW_SURROGATE(ch) && pendingHigh) t += pendingHigh;
    pendingHigh = 0;
    t += ch;
    if (!Editable()) {
        MessageBeep(MB_OK);
        return;
    }
    Record(MacroKind::Char, ch);
    if (autoClose && t.size() == 1 && !HasSelection()) {
        Pos c = doc->caret;
        const std::wstring& s = doc->lines[c.line];
        wchar_t next = c.col < (int)s.size() ? s[c.col] : 0;
        if ((ch == L')' || ch == L']' || ch == L'}' || ch == L'"' || ch == L'\'') && next == ch) {
            MoveTo({c.line, c.col + 1}, false);
            return;
        }
        wchar_t close = ch == L'(' ? L')' : ch == L'[' ? L']' : ch == L'{' ? L'}' : ch == L'"' ? L'"' : 0;
        if (close && (next == 0 || next == L' ' || next == L')' || next == L']' || next == L'}')) {
            ReplaceSel(std::wstring{ch, close});
            Place({c.line, c.col + 1});
            return;
        }
    }
    ReplaceSel(t, true);
    bool visible = acList && IsWindowVisible(acList);
    if (IsWordChar(ch) && (autoComplete || visible))
        ShowCompletion(true);
    else
        HideCompletion();
}

void Editor::OnMouseDown(int x, int y, bool shift) {
    DWORD now = GetMessageTime();
    if (now - lastClickTime <= GetDoubleClickTime() &&
        abs(x - lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
        abs(y - lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK))
        clickCount = clickCount % 3 + 1;
    else
        clickCount = 1;
    lastClickTime = now;
    lastClickPt = {x, y};
    Pos p = PosFromPoint(std::max(x, TextLeft()), y);
    if (x < marginBookmarkW) {
        ToggleBookmark(p.line);
        if (onMarginClick) onMarginClick(p.line);
        return;
    }
    auto lineEnd = [&](int line) {
        return line + 1 < doc->LineCount() ? Pos{line + 1, 0} : Pos{line, (int)doc->lines[line].size()};
    };
    if (x < TextLeft() || clickCount == 3) {
        dragMode = 2;
        Pos s{p.line, 0}, e = lineEnd(p.line);
        if (shift && x < TextLeft()) {
            dragOrigin = {doc->anchor.line, 0};
            dragOriginEnd = lineEnd(doc->anchor.line);
            if (p.line < doc->anchor.line)
                SetSel(dragOriginEnd, s, true);
            else
                SetSel(dragOrigin, e, true);
        } else {
            dragOrigin = s;
            dragOriginEnd = e;
            SetSel(s, e, true);
        }
    } else if (clickCount == 2) {
        dragMode = 1;
        Pos s, e;
        if (!WordAt(p, s, e)) s = e = p;
        dragOrigin = s;
        dragOriginEnd = e;
        SetSel(s, e, false);
    } else {
        dragMode = 0;
        MoveTo(p, shift);
    }
    dragging = true;
    SetCapture(hwnd);
    SetTimer(hwnd, kDragTimer, 50, nullptr);
}

void Editor::OnMouseMove(int x, int y) {
    if (y < 0)
        SetTopRow(doc->topRow - 1);
    else if (y >= clientH)
        SetTopRow(doc->topRow + 1);
    if (!wordWrap && dragMode != 2) {
        if (x < TextLeft())
            SetXOffset(doc->xOffset - charW * 2);
        else if (x > clientW)
            SetXOffset(doc->xOffset + charW * 2);
    }
    Pos p = PosFromPoint(std::max(x, TextLeft()), y);
    if (dragMode == 0) {
        doc->caret = p;
    } else if (dragMode == 1) {
        Pos s, e;
        if (!WordAt(p, s, e)) s = e = p;
        if (p < dragOrigin) {
            doc->anchor = dragOriginEnd;
            doc->caret = s;
        } else {
            doc->anchor = dragOrigin;
            doc->caret = e;
        }
    } else {
        Pos ls{p.line, 0};
        Pos le = p.line + 1 < doc->LineCount() ? Pos{p.line + 1, 0} : Pos{p.line, (int)doc->lines[p.line].size()};
        if (p.line < dragOrigin.line) {
            doc->anchor = dragOriginEnd;
            doc->caret = ls;
        } else {
            doc->anchor = dragOrigin;
            doc->caret = le;
        }
    }
    doc->desiredCell = -1;
    Changed(false);
}

void Editor::UpdateImePos() {
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;
    int x, y;
    PosToPoint(doc->caret, x, y);
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = {x, y};
    ImmSetCompositionWindow(himc, &cf);
    LOGFONTW lf;
    GetObjectW(fonts[0], sizeof(lf), &lf);
    ImmSetCompositionFontW(himc, &lf);
    ImmReleaseContext(hwnd, himc);
}

// ---------------------------------------------------------------- painting

void Editor::Render() {
    EnsureLayout();
    const Theme& th = GetTheme(dark);
    HDC dc = backDC;
    FillSolid(dc, 0, 0, clientW, clientH, th.bg);
    SetBkMode(dc, TRANSPARENT);

    Pos sa, sb;
    GetSel(sa, sb);
    bool hasSel = sa != sb;
    Pos braceAp, braceBp;
    bool bad = false;
    bool hasBrace = FindBrace(braceAp, braceBp, bad);

    std::wstring smart;
    if (smartHighlight && hasSel && sa.line == sb.line && sb.col - sa.col <= 200) {
        smart = Lower(doc->GetText(sa, sb));
        for (wchar_t ch : smart)
            if (!IsWordChar(ch)) {
                smart.clear();
                break;
            }
    }

    const int left = TextLeft();
    const int rows = clientH / lineH + 1;
    const int x0 = left - (wordWrap ? 0 : doc->xOffset);
    std::vector<int> xs;
    std::vector<INT> dx;
    HPEN wsPen = CreatePen(PS_SOLID, std::max(1, Scale(1)), th.whitespace);
    bool widened = false;

    for (int r = 0; r < rows; r++) {
        int row = doc->topRow + r;
        if (row >= totalRows) break;
        int line, sub;
        RowToLine(row, line, sub);
        int a, b;
        SegRange(line, sub, a, b);
        const std::wstring& s = doc->lines[line];
        const int y = r * lineH;
        const bool lastSeg = sub == SegCount(line) - 1;
        const std::vector<uint8_t>& st = LineStyles(line);
        COLORREF rowBg = (highlightCurrentLine && line == doc->caret.line) ? th.curLine : th.bg;
        if (rowBg != th.bg) FillSolid(dc, left, y, clientW, y + lineH, rowBg);

        int baseCell = CellAt(s, a);
        xs.resize(b - a + 1);
        int cell = baseCell;
        for (int k = a; k < b; k++) {
            xs[k - a] = x0 + (cell - baseCell) * charW;
            cell += CellWidth(s[k], cell);
        }
        xs[b - a] = x0 + (cell - baseCell) * charW;
        int endX = xs[b - a];
        if (!wordWrap && lastSeg) {
            int w = (cell - baseCell) * charW + charW * 8;
            if (w > scrollWidth) {
                scrollWidth = w;
                widened = true;
            }
        }

        // Selection
        if (hasSel && line >= sa.line && line <= sb.line) {
            int c0 = line == sa.line ? sa.col : 0;
            int c1 = line == sb.line ? sb.col : (int)s.size();
            int s0 = std::max(c0, a), s1 = std::min(c1, b);
            if (s0 < s1) FillSolid(dc, xs[s0 - a], y, xs[s1 - a], y + lineH, th.selBg);
            if (line < sb.line && lastSeg) FillSolid(dc, endX, y, endX + charW, y + lineH, th.selBg);
        }

        // Find marks and smart highlight boxes
        auto box = [&](int c0, int c1, COLORREF color) {
            int s0 = std::max(c0, a), s1 = std::min(c1, b);
            if (s0 >= s1) return;
            COLORREF fill = Blend(rowBg, color, dark ? 140 : 100);
            FillSolid(dc, xs[s0 - a], y, xs[s1 - a], y + lineH, fill);
            HBRUSH br = CreateSolidBrush(Blend(rowBg, color, 200));
            RECT fr{xs[s0 - a], y, xs[s1 - a], y + lineH};
            FrameRect(dc, &fr, br);
            DeleteObject(br);
        };
        for (const MarkRange& m : doc->marks)
            if (m.line == line) box(m.col, m.col + m.len, th.findMark);
        if (!smart.empty() && s.size() >= smart.size()) {
            std::wstring ls = Lower(s);
            size_t pos = 0;
            while ((pos = ls.find(smart, pos)) != std::wstring::npos) {
                size_t e = pos + smart.size();
                bool whole = (pos == 0 || !IsWordChar(ls[pos - 1])) && (e >= ls.size() || !IsWordChar(ls[e]));
                if (whole && !(line == sa.line && (int)pos == sa.col)) box((int)pos, (int)e, th.smart);
                pos = e;
            }
        }

        // Indent guides
        if (showIndentGuides && a == 0) {
            int lead = 0, cells = 0;
            while (lead < (int)s.size() && (s[lead] == L' ' || s[lead] == L'\t')) cells += CellWidth(s[lead++], cells);
            if (lead == (int)s.size()) cells = 0;
            for (int g = tabWidth; g < cells; g += tabWidth) {
                int gx = x0 + g * charW;
                if (gx < left) continue;
                for (int yy = y + (row & 1); yy < y + lineH; yy += 2) SetPixel(dc, gx, yy, th.indentGuide);
            }
        }

        // Text
        int k = a;
        auto isBraceAt = [&](int col) {
            return hasBrace && ((line == braceAp.line && col == braceAp.col) || (line == braceBp.line && col == braceBp.col));
        };
        while (k < b) {
            wchar_t ch = s[k];
            if (ch == L'\t' || (ch == L' ' && showSpaces)) {
                if (showSpaces) {
                    HGDIOBJ op = SelectObject(dc, wsPen);
                    int cx0 = xs[k - a], cx1 = xs[k + 1 - a], cy = y + lineH / 2;
                    if (ch == L' ') {
                        int d = std::max(2, Scale(2));
                        int mx = (cx0 + cx1) / 2 - d / 2;
                        FillSolid(dc, mx, cy - d / 2, mx + d, cy - d / 2 + d, th.whitespace);
                    } else if (cx1 - cx0 > Scale(6)) {
                        int ax0 = cx0 + Scale(2), ax1 = cx1 - Scale(3), hs = Scale(3);
                        MoveToEx(dc, ax0, cy, nullptr);
                        LineTo(dc, ax1, cy);
                        MoveToEx(dc, ax1 - hs, cy - hs, nullptr);
                        LineTo(dc, ax1, cy);
                        LineTo(dc, ax1 - hs - 1, cy + hs + 1);
                    }
                    SelectObject(dc, op);
                }
                k++;
                continue;
            }
            if (ch < 32) {
                int cx0 = xs[k - a], cx1 = xs[k + 1 - a];
                FillSolid(dc, cx0 + 1, y + Scale(2), cx1 - 1, y + lineH - Scale(2), th.eolBoxBg);
                k++;
                continue;
            }
            uint8_t style = k < (int)st.size() ? st[k] : (uint8_t)ST_DEFAULT;
            bool brace = isBraceAt(k);
            int j = k + 1;
            if (!brace)
                while (j < b && j < k + 512 && (j >= (int)st.size() ? ST_DEFAULT : st[j]) == style && s[j] >= 32 &&
                       s[j] != L'\t' && !(s[j] == L' ' && showSpaces) && !isBraceAt(j))
                    j++;
            if (xs[j - a] < left - charW * 2) {
                k = j;
                continue;
            }
            if (xs[k - a] > clientW) break;
            const StyleDef& sd = th.styles[style < ST_COUNT ? style : 0];
            COLORREF fg = sd.fg;
            bool bold = sd.bold, ital = sd.italic;
            if (brace) {
                fg = bad ? th.badBraceFg : th.braceFg;
                bold = true;
            }
            SelectObject(dc, fonts[(bold ? 1 : 0) + (ital ? 2 : 0)]);
            SetTextColor(dc, fg);
            dx.resize(j - k);
            for (int i = k; i < j; i++) dx[i - k] = xs[i + 1 - a] - xs[i - a];
            ExtTextOutW(dc, xs[k - a], y, 0, nullptr, s.c_str() + k, j - k, dx.data());
            k = j;
        }

        if (showEol && lastSeg && line < doc->LineCount() - 1) {
            const wchar_t* t = doc->eol == Eol::CRLF ? L"CRLF" : doc->eol == Eol::LF ? L"LF" : L"CR";
            SelectObject(dc, smallFont);
            SIZE sz;
            GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
            int bx = endX + Scale(2), pad = Scale(2);
            FillSolid(dc, bx, y + 1, bx + sz.cx + pad * 2, y + lineH - 1, th.eolBoxBg);
            SetTextColor(dc, th.eolBoxFg);
            ExtTextOutW(dc, bx + pad, y + (lineH - sz.cy) / 2, 0, nullptr, t, (int)wcslen(t), nullptr);
        }
    }
    DeleteObject(wsPen);

    // Margins are drawn last so horizontally scrolled text never shows through them.
    if (marginBookmarkW) FillSolid(dc, 0, 0, marginBookmarkW, clientH, th.bookmarkBg);
    if (marginNumbersW) FillSolid(dc, marginBookmarkW, 0, marginBookmarkW + marginNumbersW, clientH, th.lineNumBg);
    FillSolid(dc, marginBookmarkW + marginNumbersW, 0, left, clientH, th.bg);
    SelectObject(dc, fonts[0]);
    SetTextColor(dc, th.lineNumFg);
    HBRUSH bmBrush = CreateSolidBrush(th.bookmark);
    HPEN bmPen = CreatePen(PS_SOLID, 1, Blend(th.bookmark, RGB(0, 0, 0), 80));
    for (int r = 0; r < rows; r++) {
        int row = doc->topRow + r;
        if (row >= totalRows) break;
        int line, sub;
        RowToLine(row, line, sub);
        if (sub != 0) continue;
        int y = r * lineH;
        if (marginNumbersW) {
            wchar_t num[16];
            int n = wsprintfW(num, L"%d", line + 1);
            RECT rc{marginBookmarkW, y, marginBookmarkW + marginNumbersW - Scale(4), y + lineH};
            DrawTextW(dc, num, n, &rc, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_TOP);
        }
        if (marginBookmarkW && doc->bookmarks[line]) {
            int d = std::min(marginBookmarkW, lineH) - Scale(4);
            int cx = marginBookmarkW / 2, cy = y + lineH / 2;
            HGDIOBJ ob = SelectObject(dc, bmBrush), op = SelectObject(dc, bmPen);
            RoundRect(dc, cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d, d / 2, d / 2);
            SelectObject(dc, ob);
            SelectObject(dc, op);
        }
    }
    DeleteObject(bmBrush);
    DeleteObject(bmPen);
    if (widened) UpdateScrollBars();
}

void Editor::Paint(HDC hdc, const RECT& rc) {
    int w = std::max(1, clientW), h = std::max(1, clientH);
    if (!backDC || backW != w || backH != h) {
        if (backDC) DeleteDC(backDC);
        if (backBmp) DeleteObject(backBmp);
        backDC = CreateCompatibleDC(hdc);
        backBmp = CreateCompatibleBitmap(hdc, w, h);
        SelectObject(backDC, backBmp);
        backW = w;
        backH = h;
        bufferValid = false;
    }
    if (!bufferValid) {
        Render();
        bufferValid = true;
    }
    BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, backDC, rc.left, rc.top, SRCCOPY);
    if (focused && caretOn) {
        int x, y;
        if (PosToPoint(doc->caret, x, y) && x >= TextLeft() - 1 && x < clientW) {
            const Theme& th = GetTheme(dark);
            if (overwrite)
                FillSolid(hdc, x, y + lineH - Scale(2), x + charW, y + lineH, th.caret);
            else
                FillSolid(hdc, x, y, x + std::max(1, Scale(1)), y + lineH, th.caret);
        }
    }
}

// ---------------------------------------------------------------- window procedure

LRESULT Editor::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        dpi = GetDpiForWindow(hwnd);
        CreateFonts();
        lineSelectFormat = RegisterClipboardFormatW(L"MSDEVLineSelect");
        return 0;
    case WM_DESTROY:
        DestroyFonts();
        if (backDC) DeleteDC(backDC);
        if (backBmp) DeleteObject(backBmp);
        backDC = nullptr;
        backBmp = nullptr;
        return 0;
    case WM_SIZE:
        clientW = LOWORD(lp);
        clientH = HIWORD(lp);
        if (!doc) return 0;
        HideCompletion();
        EnsureLayout();
        SetTopRow(doc->topRow);
        Invalidate();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS:
        focused = true;
        RestartBlink();
        Invalidate();
        return 0;
    case WM_KILLFOCUS:
        focused = false;
        KillTimer(hwnd, kBlinkTimer);
        if ((HWND)wp != acList) HideCompletion();
        Invalidate();
        return 0;
    case WM_TIMER:
        if (wp == kBlinkTimer) {
            caretOn = !caretOn;
            int x, y;
            if (PosToPoint(doc->caret, x, y)) {
                RECT r{x - 1, y, x + charW + 2, y + lineH};
                InvalidateRect(hwnd, &r, FALSE);
            }
        } else if (wp == kDragTimer && dragging) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (pt.y < 0 || pt.y >= clientH || pt.x < TextLeft() || pt.x > clientW) OnMouseMove(pt.x, pt.y);
        }
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (!alt && CompletionKey((UINT)wp)) return 0;
        if (KeyDown((UINT)wp, shift, ctrl, alt)) return 0;
        break;
    }
    case WM_CHAR: Char((wchar_t)wp); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        HideCompletion();
        OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_SHIFT) != 0);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging) OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (dragging) {
            dragging = false;
            ReleaseCapture();
            KillTimer(hwnd, kDragTimer);
        }
        return 0;
    case WM_CAPTURECHANGED:
        dragging = false;
        KillTimer(hwnd, kDragTimer);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            ZoomBy(delta > 0 ? 1 : -1);
            return 0;
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) {
            SetXOffset(doc->xOffset - delta / 40 * charW);
            return 0;
        }
        wheelAccum += delta;
        int notches = wheelAccum / WHEEL_DELTA;
        wheelAccum -= notches * WHEEL_DELTA;
        if (notches) {
            int n = lines == WHEEL_PAGESCROLL ? VisibleRows() : (int)lines;
            SetTopRow(doc->topRow - notches * n);
        }
        return 0;
    }
    case WM_MOUSEHWHEEL:
        SetXOffset(doc->xOffset + GET_WHEEL_DELTA_WPARAM(wp) / 40 * charW);
        return 0;
    case WM_VSCROLL: {
        int r = doc->topRow;
        switch (LOWORD(wp)) {
        case SB_LINEUP: r--; break;
        case SB_LINEDOWN: r++; break;
        case SB_PAGEUP: r -= VisibleRows(); break;
        case SB_PAGEDOWN: r += VisibleRows(); break;
        case SB_TOP: r = 0; break;
        case SB_BOTTOM: r = totalRows; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{sizeof(si), SIF_TRACKPOS};
            GetScrollInfo(hwnd, SB_VERT, &si);
            r = si.nTrackPos;
            break;
        }
        }
        SetTopRow(r);
        return 0;
    }
    case WM_HSCROLL: {
        int x = doc->xOffset, area = std::max(1, clientW - TextLeft());
        switch (LOWORD(wp)) {
        case SB_LINELEFT: x -= charW * 2; break;
        case SB_LINERIGHT: x += charW * 2; break;
        case SB_PAGELEFT: x -= area; break;
        case SB_PAGERIGHT: x += area; break;
        case SB_LEFT: x = 0; break;
        case SB_RIGHT: x = scrollWidth; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{sizeof(si), SIF_TRACKPOS};
            GetScrollInfo(hwnd, SB_HORZ, &si);
            x = si.nTrackPos;
            break;
        }
        }
        SetXOffset(x);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursor(nullptr, pt.x < TextLeft() ? IDC_ARROW : IDC_IBEAM));
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU:
        if (onContextMenu) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (x == -1 && y == -1) {
                int cx, cy;
                PosToPoint(doc->caret, cx, cy);
                POINT pt{cx, cy + lineH};
                ClientToScreen(hwnd, &pt);
                x = pt.x;
                y = pt.y;
            }
            onContextMenu(x, y);
        }
        return 0;
    case WM_RBUTTONDOWN: {
        // Keep the selection when right-clicking inside it, otherwise move the caret first.
        SetFocus(hwnd);
        Pos p = PosFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        Pos a, b;
        GetSel(a, b);
        if (GET_X_LPARAM(lp) >= TextLeft() && (p < a || b < p || a == b)) MoveTo(p, false);
        return 0;
    }
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION: UpdateImePos(); break;
    case WM_COMMAND:
        if (LOWORD(wp) == kCompletionId) {
            if (HIWORD(wp) == LBN_DBLCLK)
                AcceptCompletion();
            else if (HIWORD(wp) == LBN_SELCHANGE)
                SetFocus(hwnd);
        }
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        dpi = GetDpiForWindow(hwnd);
        ApplyOptions();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
