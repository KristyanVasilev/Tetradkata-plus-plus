// The text editing control: one window that displays and edits whichever Document is active,
// the way Notepad++ uses one Scintilla view for many buffers (finding #12).
#pragma once
#include "document.h"

#include <functional>
#include <string>
#include <vector>

enum class MacroKind { Char, Key, Command };
struct MacroStep {
    MacroKind kind;
    UINT value;
    UINT mods; // 1 shift, 2 ctrl, 4 alt
};

class Editor {
public:
    static bool Register(HINSTANCE hi);
    HWND Create(HWND parent, int id);
    HWND hwnd = nullptr;

    void SetDocument(Document* d);
    Document* Doc() const { return doc; }

    // Options
    bool wordWrap = false, showSpaces = false, showEol = false, showIndentGuides = true;
    bool dark = false, overwrite = false, smartHighlight = true, autoIndent = true, useSpaces = false;
    bool lineNumbers = true, bookmarkMargin = true, highlightCurrentLine = true, autoClose = false;
    bool autoComplete = true;
    int tabWidth = 4, zoom = 0;
    std::wstring fontName = L"Courier New";
    int fontSize = 10;
    void ApplyOptions();

    // Callbacks
    std::function<void()> onUpdateUI;
    std::function<void()> onSavePointChanged;
    std::function<void(int x, int y)> onContextMenu;
    std::function<void(const MacroStep&)> onMacroRecord;
    std::function<void(int line)> onMarginClick;
    bool recording = false;

    // Selection & navigation
    Pos Caret() const { return doc->caret; }
    Pos Anchor() const { return doc->anchor; }
    bool HasSelection() const { return doc->caret != doc->anchor; }
    void GetSel(Pos& a, Pos& b) const;
    void SetSel(Pos anchor, Pos caret, bool scroll = true, bool center = false);
    void GotoLine(int line);
    void ScrollCaretIntoView(bool center = false);
    std::wstring GetSelText() const;
    bool WordAt(Pos p, Pos& start, Pos& end) const;
    std::wstring CurrentWord() const;
    bool Editable() const;
    int FirstVisibleLine();
    int VisibleRows() const;

    // Editing
    void ReplaceSel(const std::wstring& text, bool typing = false);
    void ReplaceRange(Pos a, Pos b, const std::wstring& text, bool selectResult = false);
    void Cut();
    void Copy();
    void Paste();
    void Clear();
    void SelectAll();
    void Undo();
    void Redo();
    void TransformLines(const std::function<void(std::vector<std::wstring>&)>& fn, bool wholeDocIfSingleLine);
    void TransformSelection(const std::function<std::wstring(const std::wstring&)>& fn);
    void DuplicateLine();
    void MoveLines(int dir);
    void DeleteLine();
    void JoinLines();
    void Indent(bool outdent);
    void InsertNewline();
    void ToggleComment(int mode); // 0 toggle, 1 comment, 2 uncomment
    void BlockComment(bool uncomment);
    void GoToMatchingBrace(bool select);
    void SelectBetweenBraces();
    void ToggleBookmark(int line);
    void NextBookmark(bool forward);
    void ZoomBy(int delta);
    void ZoomReset();
    void ShowCompletion(bool automatic);
    void ExecuteMacroStep(const MacroStep& s);

    void Invalidate();
    void DocumentChanged(); // after the document was replaced or reloaded

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    Document* doc = nullptr;
    HFONT fonts[4] = {};   // normal, bold, italic, bold italic
    HFONT smallFont = nullptr;
    int charW = 8, lineH = 16, dpi = 96;
    int clientW = 0, clientH = 0;
    int marginBookmarkW = 16, marginNumbersW = 0, marginGapW = 4;
    HBITMAP backBmp = nullptr;
    HDC backDC = nullptr;
    int backW = 0, backH = 0;
    bool bufferValid = false;
    bool focused = false, caretOn = true;
    bool dragging = false;
    int dragMode = 0; // 0 char, 1 word, 2 line
    Pos dragOrigin, dragOriginEnd;
    DWORD lastClickTime = 0;
    POINT lastClickPt{};
    int clickCount = 0;
    int wheelAccum = 0;
    wchar_t pendingHigh = 0;
    int scrollWidth = 0;
    UINT lineSelectFormat = 0;
    HWND acList = nullptr;
    Pos acStart;
    bool lastModified = false;

    // Wrap layout
    std::vector<std::vector<int>> breaks;
    std::vector<int> rowStart;
    int totalRows = 1;
    Document* layoutDoc = nullptr;
    int layoutVersion = -1, layoutCols = -1;
    bool layoutWrap = false;

    // Line style cache
    int styledLine = -1, styledVersion = -1, styledLang = -1;
    Document* styledDoc = nullptr;
    std::vector<uint8_t> styleBuf, lexScratch;

    // Brace match cache
    Document* braceDoc = nullptr;
    int braceVersion = -1;
    Pos braceCaret, braceA, braceB;
    bool braceBad = false, braceFound = false;

    int Scale(int v) const;
    void SetXOffset(int x);
    void Place(Pos p);
    Pos Left(Pos p) const;
    Pos Right(Pos p) const;
    int TextLeft() const { return marginBookmarkW + marginNumbersW + marginGapW; }
    void UpdateMargins();
    void EnsureLayout();
    void ComputeBreaks(const std::wstring& s, int cols, std::vector<int>& out) const;
    int CellWidth(wchar_t c, int cell) const;
    int CellAt(const std::wstring& s, int col) const;
    int LineToRow(int line) const;
    void RowToLine(int row, int& line, int& sub) const;
    int SubOf(Pos p) const;
    void SegRange(int line, int sub, int& a, int& b) const;
    int SegCount(int line) const;
    int ColFromCell(int line, int a, int b, int cellRel) const;
    Pos PosFromPoint(int x, int y) const;
    bool PosToPoint(Pos p, int& x, int& y);
    void EnsureLexed(int line);
    const std::vector<uint8_t>& LineStyles(int line);

    void SetTopRow(int row);
    void UpdateScrollBars();
    void Changed(bool text);
    void MoveTo(Pos p, bool extend, bool keepDesired = false);
    void MoveVertical(int rows, bool extend);
    void Backspace(bool word);
    void DeleteForward(bool word);
    Pos WordLeft(Pos p) const;
    Pos WordRight(Pos p) const;
    bool KeyDown(UINT vk, bool shift, bool ctrl, bool alt);
    void Char(wchar_t ch);
    void InsertTab(bool shift);
    void OnMouseDown(int x, int y, bool shift);
    void OnMouseMove(int x, int y);
    void Paint(HDC hdc, const RECT& rc);
    void Render();
    void RestartBlink();
    void CreateFonts();
    void DestroyFonts();
    bool FindBrace(Pos& a, Pos& b, bool& bad);
    std::wstring CommentToken() const;
    void UpdateImePos();
    void HideCompletion();
    bool CompletionKey(UINT vk);
    void AcceptCompletion();
    void Record(MacroKind k, UINT v, UINT mods = 0);
};
