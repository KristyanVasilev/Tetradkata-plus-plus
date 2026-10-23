#include "document.h"

#include <algorithm>

Pos EndOf(Pos p, const std::wstring& text) {
    size_t last = text.rfind(L'\n');
    if (last == std::wstring::npos) return {p.line, p.col + (int)text.size()};
    int nl = (int)std::count(text.begin(), text.end(), L'\n');
    return {p.line + nl, (int)(text.size() - last - 1)};
}

Document::Document() : lines(1), bookmarks(1, 0), lexState(2, 0) {}

void Document::Touched(int line) {
    version++;
    marks.clear();
    lexValid = std::min(lexValid, line + 1);
    if (lexValid < 1) lexValid = 1;
    lexState.resize(lines.size() + 1);
    bookmarks.resize(lines.size());
}

Pos Document::Clamp(Pos p) const {
    if (p.line < 0) return {0, 0};
    if (p.line >= (int)lines.size()) return EndPos();
    p.col = std::max(0, std::min(p.col, (int)lines[p.line].size()));
    return p;
}

Pos Document::EndPos() const {
    return {(int)lines.size() - 1, (int)lines.back().size()};
}

Pos Document::RawInsert(Pos p, const std::wstring& text) {
    p = Clamp(p);
    std::wstring& ln = lines[p.line];
    size_t nl = text.find(L'\n');
    if (nl == std::wstring::npos) {
        ln.insert(p.col, text);
        Touched(p.line);
        return {p.line, p.col + (int)text.size()};
    }
    std::wstring tail = ln.substr(p.col);
    ln.erase(p.col);
    ln.append(text, 0, nl);
    std::vector<std::wstring> added;
    size_t start = nl + 1;
    for (;;) {
        size_t e = text.find(L'\n', start);
        if (e == std::wstring::npos) {
            added.push_back(text.substr(start));
            break;
        }
        added.push_back(text.substr(start, e - start));
        start = e + 1;
    }
    Pos end{p.line + (int)added.size(), (int)added.back().size()};
    added.back() += tail;
    lines.insert(lines.begin() + p.line + 1, added.begin(), added.end());
    bookmarks.insert(bookmarks.begin() + p.line + 1, added.size(), 0);
    Touched(p.line);
    return end;
}

void Document::RawErase(Pos a, Pos b) {
    a = Clamp(a);
    b = Clamp(b);
    if (b < a) std::swap(a, b);
    if (a == b) return;
    if (a.line == b.line) {
        lines[a.line].erase(a.col, b.col - a.col);
    } else {
        lines[a.line] = lines[a.line].substr(0, a.col) + lines[b.line].substr(b.col);
        lines.erase(lines.begin() + a.line + 1, lines.begin() + b.line + 1);
        bookmarks.erase(bookmarks.begin() + a.line + 1, bookmarks.begin() + b.line + 1);
    }
    Touched(a.line);
}

std::wstring Document::GetText(Pos a, Pos b) const {
    a = Clamp(a);
    b = Clamp(b);
    if (b < a) std::swap(a, b);
    if (a.line == b.line) return lines[a.line].substr(a.col, b.col - a.col);
    std::wstring out = lines[a.line].substr(a.col);
    for (int i = a.line + 1; i < b.line; i++) {
        out += L'\n';
        out += lines[i];
    }
    out += L'\n';
    out += lines[b.line].substr(0, b.col);
    return out;
}

std::wstring Document::GetAllText() const {
    size_t total = 0;
    for (auto& l : lines) total += l.size() + 1;
    std::wstring out;
    out.reserve(total);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += L'\n';
        out += lines[i];
    }
    return out;
}

void Document::SetAllText(const std::wstring& text) {
    lines.assign(1, L"");
    bookmarks.assign(1, 0);
    lexValid = 1;
    RawInsert({0, 0}, text);
    ClearUndo();
    caret = anchor = {0, 0};
    topRow = 0;
    xOffset = 0;
}

size_t Document::TextLength() const {
    size_t eolLen = eol == Eol::CRLF ? 2 : 1;
    size_t n = 0;
    for (auto& l : lines) n += l.size();
    return n + eolLen * (lines.size() - 1);
}

size_t Document::OffsetOf(Pos p) const {
    p = Clamp(p);
    size_t off = 0;
    for (int i = 0; i < p.line; i++) off += lines[i].size() + 1;
    return off + p.col;
}

Pos Document::PosOfOffset(size_t off) const {
    for (int i = 0; i < (int)lines.size(); i++) {
        if (off <= lines[i].size()) return {i, (int)off};
        off -= lines[i].size() + 1;
    }
    return EndPos();
}

std::wstring Document::DisplayName() const {
    if (path.empty()) return title;
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

void Document::Record(UndoStep s) {
    redo.clear();
    if ((long)undo.size() < savePoint) savePoint = -1;
    undo.push_back(std::move(s));
}

void Document::BeginUndoGroup(Pos a, Pos c) {
    if (groupDepth++ == 0) {
        curGroup = nextGroup++;
        groupAnchor = a;
        groupCaret = c;
        typingOpen = false;
    }
}

void Document::EndUndoGroup() {
    if (groupDepth > 0) groupDepth--;
}

Pos Document::Insert(Pos p, const std::wstring& text, bool typing) {
    p = Clamp(p);
    if (text.empty()) return p;
    if (typing && typingOpen && groupDepth == 0 && !undo.empty() && undo.back().insert &&
        (long)undo.size() != savePoint && text.find(L'\n') == std::wstring::npos) {
        UndoStep& last = undo.back();
        if (last.text.find(L'\n') == std::wstring::npos && EndOf(last.pos, last.text) == p &&
            last.text.size() < 256) {
            redo.clear();
            last.text += text;
            return RawInsert(p, text);
        }
    }
    UndoStep s;
    s.insert = true;
    s.pos = p;
    s.text = text;
    if (groupDepth) {
        s.group = curGroup;
        s.selAnchor = groupAnchor;
        s.selCaret = groupCaret;
    } else {
        s.group = nextGroup++;
        s.selAnchor = s.selCaret = p;
    }
    Record(std::move(s));
    typingOpen = typing && groupDepth == 0;
    return RawInsert(p, text);
}

void Document::Erase(Pos a, Pos b, bool typing) {
    a = Clamp(a);
    b = Clamp(b);
    if (b < a) std::swap(a, b);
    if (a == b) return;
    if (typing && typingOpen && groupDepth == 0 && !undo.empty() && !undo.back().insert &&
        (long)undo.size() != savePoint && a.line == b.line) {
        UndoStep& last = undo.back();
        if (last.text.size() < 256 && last.text.find(L'\n') == std::wstring::npos) {
            std::wstring removed = GetText(a, b);
            if (b == last.pos) { // Backspace run
                last.text = removed + last.text;
                last.pos = a;
                last.selCaret = last.selAnchor = EndOf(a, last.text);
                redo.clear();
                RawErase(a, b);
                return;
            }
            if (a == last.pos) { // Delete run
                last.text += removed;
                redo.clear();
                RawErase(a, b);
                return;
            }
        }
    }
    UndoStep s;
    s.insert = false;
    s.pos = a;
    s.text = GetText(a, b);
    if (groupDepth) {
        s.group = curGroup;
        s.selAnchor = groupAnchor;
        s.selCaret = groupCaret;
    } else {
        s.group = nextGroup++;
        s.selAnchor = s.selCaret = b;
    }
    Record(std::move(s));
    typingOpen = typing && groupDepth == 0;
    RawErase(a, b);
}

bool Document::Undo(Pos& a, Pos& c) {
    if (undo.empty()) return false;
    typingOpen = false;
    int g = undo.back().group;
    while (!undo.empty() && undo.back().group == g) {
        UndoStep s = std::move(undo.back());
        undo.pop_back();
        if (s.insert)
            RawErase(s.pos, EndOf(s.pos, s.text));
        else
            RawInsert(s.pos, s.text);
        a = s.selAnchor;
        c = s.selCaret;
        redo.push_back(std::move(s));
    }
    return true;
}

bool Document::Redo(Pos& a, Pos& c) {
    if (redo.empty()) return false;
    typingOpen = false;
    int g = redo.back().group;
    while (!redo.empty() && redo.back().group == g) {
        UndoStep s = std::move(redo.back());
        redo.pop_back();
        if (s.insert)
            c = RawInsert(s.pos, s.text);
        else {
            RawErase(s.pos, EndOf(s.pos, s.text));
            c = s.pos;
        }
        a = c;
        undo.push_back(std::move(s));
    }
    return true;
}

void Document::ClearUndo() {
    undo.clear();
    redo.clear();
    savePoint = 0;
    typingOpen = false;
}

void Document::SetSavePoint() {
    savePoint = (long)undo.size();
    typingOpen = false;
}

void Document::SetDirty() { savePoint = -1; }

bool Document::IsModified() const { return savePoint != (long)undo.size(); }
