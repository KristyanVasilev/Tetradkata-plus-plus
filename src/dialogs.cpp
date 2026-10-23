// Dialogs: Find/Replace/Find in Files/Mark, search results panel, Go To, Run, Preferences,
// digests, Windows list, macro repeat and About.
#include "app.h"
#include "fileio.h"
#include "lexer.h"
#include "resource.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <regex>

using namespace App;

namespace {

HWND hFind;
int findTab = 0;

// ---------------------------------------------------------------- search engine

struct SearchSpec {
    std::wstring what, with;
    bool matchCase = false, wholeWord = false, wrap = true, backward = false;
    int mode = 0;
};

struct TextCache {
    const Document* doc = nullptr;
    int version = -1;
    std::wstring text, lower;
    std::vector<size_t> lineStarts;
    void Build(const std::wstring& t) {
        text = t;
        lower.clear();
        lineStarts.assign(1, 0);
        for (size_t i = 0; i < text.size(); i++)
            if (text[i] == L'\n') lineStarts.push_back(i + 1);
    }
    const std::wstring& Lower() {
        if (lower.size() != text.size()) {
            lower = text;
            for (auto& c : lower) c = (wchar_t)towlower(c);
        }
        return lower;
    }
    Pos PosOf(size_t off) const {
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), off);
        int line = (int)(it - lineStarts.begin()) - 1;
        return {line, (int)(off - lineStarts[line])};
    }
    size_t OffOf(Pos p) const { return p.line < (int)lineStarts.size() ? lineStarts[p.line] + p.col : text.size(); }
};

TextCache& CacheFor(const Document* d) {
    static TextCache c;
    if (c.doc != d || c.version != d->version) {
        c.Build(d->GetAllText());
        c.doc = d;
        c.version = d->version;
    }
    return c;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        wchar_t c = s[++i];
        switch (c) {
        case L'n': out += L'\n'; break;
        case L'r': out += L'\r'; break;
        case L't': out += L'\t'; break;
        case L'0': out += L'\0'; break;
        case L'\\': out += L'\\'; break;
        case L'x':
        case L'u': {
            size_t n = c == L'x' ? 2 : 4, k = 0;
            unsigned v = 0;
            while (k < n && i + 1 < s.size() && iswxdigit(s[i + 1])) {
                wchar_t h = s[++i];
                v = v * 16 + (iswdigit(h) ? h - L'0' : (towlower(h) - L'a' + 10));
                k++;
            }
            out += (wchar_t)v;
            break;
        }
        default:
            out += L'\\';
            out += c;
        }
    }
    return out;
}

// Documents store '\n' only, so a typed "\r\n" (or a lone "\r") means a line break.
std::wstring NormalizeBreaks(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\r') {
            out += L'\n';
            if (i + 1 < s.size() && s[i + 1] == L'\n') i++;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::wstring RegexPattern(const SearchSpec& sp) {
    std::wstring p = sp.what;
    std::wstring out;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == L'\\' && i + 3 < p.size() && p.compare(i, 4, L"\\r\\n") == 0) {
            out += L"\\n";
            i += 3;
        } else if (p[i] == L'\\' && i + 1 < p.size() && p[i + 1] == L'r') {
            out += L"\\n";
            i++;
        } else if (p[i] == L'\\' && i + 1 < p.size()) {
            out += p[i];
            out += p[++i];
        } else {
            out += p[i];
        }
    }
    if (sp.wholeWord) out = L"\\b(?:" + out + L")\\b";
    return out;
}

// "\1".."\9" become "$1".."$9" and \n, \t escapes are expanded, matching Notepad++'s replace syntax.
std::wstring RegexFormat(const std::wstring& with) {
    std::wstring out;
    for (size_t i = 0; i < with.size(); i++) {
        if (with[i] == L'\\' && i + 1 < with.size()) {
            wchar_t c = with[++i];
            if (iswdigit(c)) {
                out += L'$';
                out += c;
            } else if (c == L'n') {
                out += L'\n';
            } else if (c == L'r') {
                out += L'\n';
                if (i + 2 < with.size() && with[i + 1] == L'\\' && with[i + 2] == L'n') i += 2;
            } else if (c == L't') {
                out += L'\t';
            } else {
                out += c;
            }
        } else {
            out += with[i];
        }
    }
    return out;
}

struct Matcher {
    SearchSpec sp;
    std::wstring needle;
    std::wregex re;
    bool ok = true;
    std::wstring error;

    explicit Matcher(const SearchSpec& s) : sp(s) {
        if (sp.mode == 2) {
            try {
                auto flags = std::regex_constants::ECMAScript | std::regex_constants::multiline;
                if (!sp.matchCase) flags |= std::regex_constants::icase;
                re.assign(RegexPattern(sp), flags);
            } catch (const std::regex_error& e) {
                ok = false;
                error = L"Invalid regular expression";
            }
        } else {
            needle = NormalizeBreaks(sp.mode == 1 ? Unescape(sp.what) : sp.what);
            if (!sp.matchCase)
                for (auto& c : needle) c = (wchar_t)towlower(c);
            ok = !needle.empty();
        }
    }

    static bool IsWord(wchar_t c) { return c == L'_' || iswalnum(c); }

    bool Whole(const std::wstring& t, size_t pos, size_t len) const {
        return (pos == 0 || !IsWord(t[pos - 1])) && (pos + len >= t.size() || !IsWord(t[pos + len]));
    }

    // Forward: first match starting at or after `from`. Backward: last match ending at or before `from`.
    bool Find(TextCache& c, size_t from, bool backward, size_t& start, size_t& len, std::wstring* repl = nullptr) {
        if (!ok) return false;
        const std::wstring& t = c.text;
        if (sp.mode == 2) {
            try {
                std::wsmatch m;
                if (!backward) {
                    auto flags = from > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
                    if (from > t.size() || !std::regex_search(t.begin() + from, t.end(), m, re, flags)) return false;
                    start = from + m.position(0);
                    len = m.length(0);
                } else {
                    bool found = false;
                    for (auto it = std::wsregex_iterator(t.begin(), t.end(), re); it != std::wsregex_iterator(); ++it) {
                        size_t s = it->position(0), l = it->length(0);
                        if (s + l > from || (s == from && l == 0 && from != 0)) break;
                        if (s + l <= from && !(l == 0 && s == from)) {
                            start = s;
                            len = l;
                            m = *it;
                            found = true;
                        }
                    }
                    if (!found) return false;
                }
                if (repl) *repl = m.format(RegexFormat(sp.with));
                return true;
            } catch (const std::regex_error&) {
                ok = false;
                error = L"The regular expression is too complex";
                return false;
            }
        }
        const std::wstring& hay = sp.matchCase ? t : c.Lower();
        size_t n = needle.size();
        size_t pos;
        if (!backward) {
            pos = hay.find(needle, from);
            while (pos != std::wstring::npos && sp.wholeWord && !Whole(hay, pos, n)) pos = hay.find(needle, pos + 1);
        } else {
            if (from < n) return false;
            pos = hay.rfind(needle, from - n);
            while (pos != std::wstring::npos && sp.wholeWord && !Whole(hay, pos, n))
                pos = pos == 0 ? std::wstring::npos : hay.rfind(needle, pos - 1);
        }
        if (pos == std::wstring::npos) return false;
        start = pos;
        len = n;
        if (repl) *repl = NormalizeBreaks(sp.mode == 1 ? Unescape(sp.with) : sp.with);
        return true;
    }

    struct Hit {
        size_t start, len;
        std::wstring repl;
    };

    std::vector<Hit> All(TextCache& c, bool wantRepl, size_t from = 0, size_t to = std::wstring::npos) {
        std::vector<Hit> out;
        size_t s, l;
        std::wstring r;
        while (from <= c.text.size() && Find(c, from, false, s, l, wantRepl ? &r : nullptr)) {
            if (to != std::wstring::npos && s + l > to) break;
            out.push_back({s, l, r});
            from = l ? s + l : s + 1;
            if (out.size() > 2000000) break;
        }
        return out;
    }
};

SearchSpec SpecFromSettings() {
    SearchSpec sp;
    sp.what = settings.findHistory.empty() ? L"" : settings.findHistory[0];
    sp.with = settings.replaceHistory.empty() ? L"" : settings.replaceHistory[0];
    sp.matchCase = settings.matchCase;
    sp.wholeWord = settings.wholeWord;
    sp.wrap = settings.wrapAround;
    sp.backward = settings.backward;
    sp.mode = settings.searchMode;
    return sp;
}

void SetFindStatus(const std::wstring& s, bool bad = false) {
    if (hFind) SetDlgItemTextW(hFind, IDC_FIND_STATUS, s.c_str());
    if (!hFind || !IsWindowVisible(hFind)) FlashStatus(s);
    if (bad) MessageBeep(MB_ICONWARNING);
}

void AddHistory(std::vector<std::wstring>& h, const std::wstring& v) {
    if (v.empty()) return;
    h.erase(std::remove(h.begin(), h.end(), v), h.end());
    h.insert(h.begin(), v);
    if (h.size() > 20) h.resize(20);
}

bool FindWith(SearchSpec sp, bool backward, bool quiet = false) {
    Document* d = Cur();
    Matcher m(sp);
    if (!m.ok) {
        if (!m.error.empty()) SetFindStatus(L"Find: " + m.error, true);
        return false;
    }
    TextCache& c = CacheFor(d);
    Pos a, b;
    editor.GetSel(a, b);
    size_t from = backward ? c.OffOf(a) : c.OffOf(b);
    if (!backward && a == b && sp.mode == 2) {
        // Avoid matching the same empty match again.
        size_t s, l;
        if (m.Find(c, from, false, s, l) && s == from && l == 0) from++;
    }
    size_t s, l;
    bool found = m.Find(c, from, backward, s, l);
    std::wstring status;
    if (!found && sp.wrap) {
        found = m.Find(c, backward ? c.text.size() : 0, backward, s, l);
        if (found)
            status = backward ? L"Find: Found the 1st occurrence from the bottom. The beginning of the document has been "
                                L"reached."
                              : L"Find: Found the 1st occurrence from the top. The end of the document has been reached.";
    }
    if (!found) {
        if (!quiet) SetFindStatus(L"Find: Can't find the text \"" + sp.what + L"\"", true);
        return false;
    }
    Pos ps = c.PosOf(s), pe = c.PosOf(s + l);
    editor.SetSel(ps, pe, true);
    SetFindStatus(status);
    return true;
}

// ---------------------------------------------------------------- results panel

HWND hResultsList;
HFONT resultsFont;
std::vector<SearchHit> resultHits;
struct ResultRow {
    int hit; // -1 for header rows
};
std::vector<ResultRow> resultRows;

WNDPROC resultsOldProc;

LRESULT CALLBACK ResultsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        ResultsNavigate(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        ShowResultsPanel(false);
        SetFocus(editor.hwnd);
        return 0;
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcW(resultsOldProc, h, msg, wp, lp);
}

void OpenHit(const SearchHit& hit) {
    int idx = hit.doc ? IndexOf(hit.doc) : -1;
    if (idx < 0) idx = hit.path.empty() ? -1 : OpenFile(hit.path, false);
    if (idx < 0) return;
    Activate(idx);
    Document* d = docs[idx].get();
    Pos a = d->Clamp({hit.line, hit.col});
    Pos b = d->Clamp({hit.line, hit.col + hit.len});
    editor.SetSel(a, b, true, true);
}

std::vector<SearchHit> HitsInDocument(Document* d, Matcher& m, const std::wstring& label) {
    std::vector<SearchHit> hits;
    TextCache& c = CacheFor(d);
    for (auto& h : m.All(c, false)) {
        Pos p = c.PosOf(h.start), e = c.PosOf(h.start + h.len);
        int len = e.line == p.line ? e.col - p.col : (int)d->lines[p.line].size() - p.col;
        hits.push_back({d, label, p.line, p.col, len, d->lines[p.line]});
    }
    return hits;
}

} // namespace

HWND CreateResultsPanel(HWND parent) {
    hResultsList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                   WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
                                   0, 0, 0, 0, parent, (HMENU)IDC_RESULTS, hInst, nullptr);
    UINT dpi = GetDpiForWindow(parent);
    resultsFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(hResultsList, WM_SETFONT, (WPARAM)resultsFont, FALSE);
    resultsOldProc = (WNDPROC)SetWindowLongPtrW(hResultsList, GWLP_WNDPROC, (LONG_PTR)ResultsProc);
    return hResultsList;
}

HWND ResultsPanel() { return hResultsList; }

void SetResults(const std::wstring& what, std::vector<SearchHit> hits, int filesSearched) {
    resultHits = std::move(hits);
    resultRows.clear();
    SendMessageW(hResultsList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hResultsList, LB_RESETCONTENT, 0, 0);
    int files = 0;
    std::wstring lastFile;
    for (auto& h : resultHits)
        if (h.path != lastFile) {
            files++;
            lastFile = h.path;
        }
    std::wstring head = L"Search \"" + what + L"\" (" + std::to_wstring(resultHits.size()) + L" hits in " +
                        std::to_wstring(files) + L" files of " + std::to_wstring(filesSearched) + L" searched)";
    SendMessageW(hResultsList, LB_ADDSTRING, 0, (LPARAM)head.c_str());
    resultRows.push_back({-1});
    lastFile.clear();
    int longest = (int)head.size();
    for (size_t i = 0; i < resultHits.size(); i++) {
        const SearchHit& h = resultHits[i];
        if (i == 0 || h.path != lastFile) {
            int count = 0;
            for (size_t k = i; k < resultHits.size() && resultHits[k].path == h.path; k++) count++;
            std::wstring fh = L"  " + h.path + L" (" + std::to_wstring(count) + L" hits)";
            SendMessageW(hResultsList, LB_ADDSTRING, 0, (LPARAM)fh.c_str());
            resultRows.push_back({-1});
            lastFile = h.path;
        }
        std::wstring text = h.text;
        if (text.size() > 400) text = text.substr(0, 400) + L"...";
        for (auto& c : text)
            if (c == L'\t') c = L' ';
        std::wstring row = L"    Line " + std::to_wstring(h.line + 1) + L": " + text;
        longest = std::max(longest, (int)row.size());
        SendMessageW(hResultsList, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        resultRows.push_back({(int)i});
    }
    SendMessageW(hResultsList, LB_SETHORIZONTALEXTENT, longest * 8, 0);
    SendMessageW(hResultsList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hResultsList, nullptr, TRUE);
    ShowResultsPanel(true);
}

void ForgetDocumentInResults(const Document* d) {
    for (auto& h : resultHits)
        if (h.doc == d) h.doc = nullptr;
}

void ResultsNavigate(int dir) {
    if (resultRows.empty()) return;
    int sel = (int)SendMessageW(hResultsList, LB_GETCURSEL, 0, 0);
    if (dir != 0) {
        int n = (int)resultRows.size();
        int i = sel < 0 ? (dir > 0 ? -1 : n) : sel;
        do {
            i += dir;
        } while (i >= 0 && i < n && resultRows[i].hit < 0);
        if (i < 0 || i >= n) return;
        sel = i;
        SendMessageW(hResultsList, LB_SETCURSEL, sel, 0);
    }
    if (sel < 0 || sel >= (int)resultRows.size() || resultRows[sel].hit < 0) return;
    OpenHit(resultHits[resultRows[sel].hit]);
    if (dir == 0) SetFocus(editor.hwnd);
}

// ---------------------------------------------------------------- Find dialog

namespace {

const int kButtons[] = {IDC_FIND_NEXT, IDC_COUNT,      IDC_FIND_ALL_CUR,  IDC_FIND_ALL_OPEN, IDC_REPLACE, IDC_REPLACE_ALL,
                        IDC_REPLACE_ALL_OPEN, IDC_FIND_IN_FILES, IDC_MARK_ALL, IDC_CLEAR_MARKS, IDCANCEL};

void FillCombo(HWND dlg, int id, const std::vector<std::wstring>& items, const std::wstring& text) {
    HWND cb = GetDlgItem(dlg, id);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (auto& s : items) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SetWindowTextW(cb, text.c_str());
}

std::wstring GetText(HWND dlg, int id) {
    HWND w = GetDlgItem(dlg, id);
    int n = GetWindowTextLengthW(w);
    std::wstring s(n, L'\0');
    GetWindowTextW(w, &s[0], n + 1);
    return s;
}

void LayoutFindTab(HWND dlg) {
    findTab = TabCtrl_GetCurSel(GetDlgItem(dlg, IDC_FIND_TABS));
    bool replace = findTab == 1 || findTab == 2;
    bool files = findTab == 2;
    bool mark = findTab == 3;
    auto show = [dlg](int id, bool on) { ShowWindow(GetDlgItem(dlg, id), on ? SW_SHOW : SW_HIDE); };
    show(IDC_REPLACE_LABEL, replace && !files);
    show(IDC_REPLACE_WITH, replace && !files);
    for (int id : {IDC_FILTERS_LABEL, IDC_FILTERS, IDC_DIR_LABEL, IDC_DIR, IDC_BROWSE, IDC_SUBFOLDERS, IDC_HIDDEN, IDC_CUR_DIR})
        show(id, files);
    show(IDC_BOOKMARK_LINE, mark);
    show(IDC_BACKWARD, findTab <= 1);
    show(IDC_WRAP, !files);
    std::vector<int> visible;
    switch (findTab) {
    case 0: visible = {IDC_FIND_NEXT, IDC_COUNT, IDC_FIND_ALL_CUR, IDC_FIND_ALL_OPEN, IDCANCEL}; break;
    case 1: visible = {IDC_FIND_NEXT, IDC_REPLACE, IDC_REPLACE_ALL, IDC_REPLACE_ALL_OPEN, IDCANCEL}; break;
    case 2: visible = {IDC_FIND_IN_FILES, IDCANCEL}; break;
    case 3: visible = {IDC_MARK_ALL, IDC_CLEAR_MARKS, IDCANCEL}; break;
    }
    for (int id : kButtons) show(id, std::find(visible.begin(), visible.end(), id) != visible.end());
    RECT r{284, 26, 398, 40};
    MapDialogRect(dlg, &r);
    int step = MulDiv(r.bottom - r.top, 17, 14);
    for (size_t i = 0; i < visible.size(); i++)
        SetWindowPos(GetDlgItem(dlg, visible[i]), nullptr, r.left, r.top + (int)i * step, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(dlg, DM_SETDEFID, visible[0], 0);
    static const wchar_t* titles[] = {L"Find", L"Replace", L"Find in Files", L"Mark"};
    SetWindowTextW(dlg, titles[findTab]);
}

void ReadFindOptions(HWND dlg, SearchSpec& sp) {
    sp.what = GetText(dlg, IDC_FIND_WHAT);
    sp.with = GetText(dlg, IDC_REPLACE_WITH);
    settings.matchCase = sp.matchCase = IsDlgButtonChecked(dlg, IDC_MATCH_CASE) == BST_CHECKED;
    settings.wholeWord = sp.wholeWord = IsDlgButtonChecked(dlg, IDC_MATCH_WORD) == BST_CHECKED;
    settings.wrapAround = sp.wrap = IsDlgButtonChecked(dlg, IDC_WRAP) == BST_CHECKED;
    settings.backward = sp.backward = IsDlgButtonChecked(dlg, IDC_BACKWARD) == BST_CHECKED;
    settings.bookmarkLine = IsDlgButtonChecked(dlg, IDC_BOOKMARK_LINE) == BST_CHECKED;
    settings.searchMode = sp.mode = IsDlgButtonChecked(dlg, IDC_MODE_REGEX) == BST_CHECKED      ? 2
                                    : IsDlgButtonChecked(dlg, IDC_MODE_EXTENDED) == BST_CHECKED ? 1
                                                                                                : 0;
    settings.subfolders = IsDlgButtonChecked(dlg, IDC_SUBFOLDERS) == BST_CHECKED;
    settings.hiddenFolders = IsDlgButtonChecked(dlg, IDC_HIDDEN) == BST_CHECKED;
    settings.followCurDir = IsDlgButtonChecked(dlg, IDC_CUR_DIR) == BST_CHECKED;
    settings.findDir = GetText(dlg, IDC_DIR);
    settings.findFilters = GetText(dlg, IDC_FILTERS);
    AddHistory(settings.findHistory, sp.what);
    if (findTab == 1) AddHistory(settings.replaceHistory, sp.with);
    FillCombo(dlg, IDC_FIND_WHAT, settings.findHistory, sp.what);
    if (findTab == 1) FillCombo(dlg, IDC_REPLACE_WITH, settings.replaceHistory, sp.with);
}

int ReplaceAllIn(Document* d, const SearchSpec& sp) {
    if (d->readOnly || d->fileReadOnly || d->monitoring) return 0;
    Matcher m(sp);
    if (!m.ok) return -1;
    TextCache& c = CacheFor(d);
    auto hits = m.All(c, true);
    if (hits.empty()) return 0;
    std::vector<std::pair<Pos, Pos>> ranges;
    for (auto& h : hits) ranges.push_back({c.PosOf(h.start), c.PosOf(h.start + h.len)});
    d->BeginUndoGroup(d->anchor, d->caret);
    for (size_t i = hits.size(); i-- > 0;) {
        d->Erase(ranges[i].first, ranges[i].second);
        d->Insert(ranges[i].first, hits[i].repl);
    }
    d->EndUndoGroup();
    d->caret = d->anchor = d->Clamp(d->caret);
    if (d == Cur()) editor.DocumentChanged();
    UpdateTab(IndexOf(d));
    return (int)hits.size();
}

void DoReplace(const SearchSpec& sp) {
    Document* d = Cur();
    if (!editor.Editable()) {
        SetFindStatus(L"Replace: The document is read-only", true);
        return;
    }
    Matcher m(sp);
    if (!m.ok) {
        SetFindStatus(L"Replace: " + (m.error.empty() ? std::wstring(L"Nothing to find") : m.error), true);
        return;
    }
    TextCache& c = CacheFor(d);
    Pos a, b;
    editor.GetSel(a, b);
    size_t s, l;
    std::wstring repl;
    if (a != b && m.Find(c, c.OffOf(a), false, s, l, &repl) && s == c.OffOf(a) && s + l == c.OffOf(b)) {
        editor.ReplaceRange(a, b, repl);
        FindWith(sp, false, true);
        SetFindStatus(L"Replace: 1 occurrence was replaced. The next occurrence found.");
        return;
    }
    if (!FindWith(sp, false)) SetFindStatus(L"Replace: no occurrence was found.", true);
}

void SearchInFiles(const SearchSpec& sp) {
    namespace fs = std::filesystem;
    std::wstring dir = settings.findDir;
    if (dir.empty() || !fs::is_directory(dir)) {
        SetFindStatus(L"Find in Files: Invalid directory", true);
        return;
    }
    std::vector<std::wstring> filters;
    {
        std::wstring f = settings.findFilters.empty() ? L"*.*" : settings.findFilters;
        size_t s = 0;
        for (size_t i = 0; i <= f.size(); i++)
            if (i == f.size() || f[i] == L' ' || f[i] == L';' || f[i] == L',') {
                if (i > s) filters.push_back(f.substr(s, i - s));
                s = i + 1;
            }
    }
    Matcher m(sp);
    if (!m.ok) {
        SetFindStatus(L"Find in Files: " + (m.error.empty() ? std::wstring(L"Nothing to find") : m.error), true);
        return;
    }
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::vector<SearchHit> hits;
    int searched = 0;
    std::error_code ec;
    auto opts = fs::directory_options::skip_permission_denied;
    auto handleFile = [&](const fs::path& p) {
        std::wstring name = p.filename().wstring();
        bool match = false;
        for (auto& f : filters)
            if (PathMatchSpecW(name.c_str(), f.c_str())) match = true;
        if (!match) return;
        std::error_code e2;
        if (fs::file_size(p, e2) > 64ull * 1024 * 1024 || e2) return;
        std::string bytes;
        std::wstring err;
        if (!ReadFileBytes(p.wstring(), bytes, err) || LooksBinary(bytes)) return;
        searched++;
        Enc enc = Enc::UTF8;
        UINT cp = 0;
        Document tmp;
        Eol eol = Eol::CRLF;
        bool found = false;
        SplitLines(DecodeBytes(bytes, enc, cp, true), tmp.lines, eol, found);
        tmp.bookmarks.assign(tmp.lines.size(), 0);
        tmp.version = -1000 - searched; // distinct cache key per file
        auto fileHits = HitsInDocument(&tmp, m, p.wstring());
        for (auto& h : fileHits) h.doc = nullptr;
        hits.insert(hits.end(), fileHits.begin(), fileHits.end());
    };
    if (settings.subfolders) {
        for (fs::recursive_directory_iterator it(dir, opts, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                DWORD a = GetFileAttributesW(it->path().c_str());
                if (!settings.hiddenFolders && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_HIDDEN))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec)) handleFile(it->path());
        }
    } else {
        for (fs::directory_iterator it(dir, opts, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) handleFile(it->path());
        }
    }
    SetCursor(old);
    SetResults(sp.what, hits, searched);
    SetFindStatus(L"Find in Files: " + std::to_wstring(hits.size()) + L" hits in " + std::to_wstring(searched) +
                  L" files searched");
}

INT_PTR CALLBACK FindProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND tabs = GetDlgItem(dlg, IDC_FIND_TABS);
        const wchar_t* names[] = {L"Find", L"Replace", L"Find in Files", L"Mark"};
        for (int i = 0; i < 4; i++) {
            TCITEMW ti{TCIF_TEXT};
            ti.pszText = (LPWSTR)names[i];
            TabCtrl_InsertItem(tabs, i, &ti);
        }
        CheckDlgButton(dlg, IDC_MATCH_CASE, settings.matchCase);
        CheckDlgButton(dlg, IDC_MATCH_WORD, settings.wholeWord);
        CheckDlgButton(dlg, IDC_WRAP, settings.wrapAround);
        CheckDlgButton(dlg, IDC_BACKWARD, settings.backward);
        CheckDlgButton(dlg, IDC_SUBFOLDERS, settings.subfolders);
        CheckDlgButton(dlg, IDC_HIDDEN, settings.hiddenFolders);
        CheckDlgButton(dlg, IDC_CUR_DIR, settings.followCurDir);
        CheckRadioButton(dlg, IDC_MODE_NORMAL, IDC_MODE_REGEX, IDC_MODE_NORMAL + settings.searchMode);
        SetDlgItemTextW(dlg, IDC_FILTERS, settings.findFilters.c_str());
        SetDlgItemTextW(dlg, IDC_DIR, settings.findDir.c_str());
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == IDC_FIND_TABS && ((NMHDR*)lp)->code == TCN_SELCHANGE) LayoutFindTab(dlg);
        return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) SetDlgItemTextW(dlg, IDC_FIND_STATUS, L"");
        return FALSE;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL) {
            ShowWindow(dlg, SW_HIDE);
            SetFocus(editor.hwnd);
            return TRUE;
        }
        if (HIWORD(wp) != BN_CLICKED) return FALSE;
        if (id == IDC_BROWSE) {
            BROWSEINFOW bi{};
            bi.hwndOwner = dlg;
            bi.lpszTitle = L"Select a folder to search from";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, path)) SetDlgItemTextW(dlg, IDC_DIR, path);
                CoTaskMemFree(pidl);
            }
            return TRUE;
        }
        bool action = std::find(std::begin(kButtons), std::end(kButtons), id) != std::end(kButtons);
        if (!action) return FALSE;
        SearchSpec sp;
        ReadFindOptions(dlg, sp);
        if (sp.what.empty() && id != IDC_CLEAR_MARKS) {
            SetFindStatus(L"Find: Nothing to search for", true);
            return TRUE;
        }
        Document* d = Cur();
        switch (id) {
        case IDC_FIND_NEXT: FindWith(sp, sp.backward); break;
        case IDC_COUNT: {
            Matcher m(sp);
            int n = m.ok ? (int)m.All(CacheFor(d), false).size() : 0;
            SetFindStatus(L"Count: " + std::to_wstring(n) + L" matches in entire file");
            break;
        }
        case IDC_FIND_ALL_CUR:
        case IDC_FIND_ALL_OPEN: {
            Matcher m(sp);
            std::vector<SearchHit> hits;
            int searched = 0;
            for (auto& up : docs) {
                Document* x = up.get();
                if (id == IDC_FIND_ALL_CUR && x != d) continue;
                searched++;
                auto h = HitsInDocument(x, m, x->path.empty() ? x->title : x->path);
                hits.insert(hits.end(), h.begin(), h.end());
            }
            SetResults(sp.what, hits, searched);
            SetFindStatus(L"Find All: " + std::to_wstring(hits.size()) + L" hits");
            break;
        }
        case IDC_REPLACE: DoReplace(sp); break;
        case IDC_REPLACE_ALL: {
            int n = ReplaceAllIn(d, sp);
            SetFindStatus(n < 0 ? L"Replace All: invalid expression"
                                : L"Replace All: " + std::to_wstring(n) + L" occurrences were replaced in entire file.");
            break;
        }
        case IDC_REPLACE_ALL_OPEN: {
            if (MessageBoxW(dlg, L"Are you sure you want to replace all occurrences in all opened documents?",
                            L"Replace All", MB_YESNO | MB_ICONQUESTION) != IDYES)
                break;
            int total = 0;
            for (auto& up : docs) total += std::max(0, ReplaceAllIn(up.get(), sp));
            SetFindStatus(L"Replace in Opened Files: " + std::to_wstring(total) + L" occurrences were replaced.");
            break;
        }
        case IDC_FIND_IN_FILES: SearchInFiles(sp); break;
        case IDC_MARK_ALL: {
            Matcher m(sp);
            if (!m.ok) break;
            TextCache& c = CacheFor(d);
            d->marks.clear();
            int n = 0;
            for (auto& h : m.All(c, false)) {
                Pos p = c.PosOf(h.start), e = c.PosOf(h.start + h.len);
                int len = e.line == p.line ? e.col - p.col : (int)d->lines[p.line].size() - p.col;
                d->marks.push_back({p.line, p.col, std::max(len, 1)});
                if (settings.bookmarkLine) d->bookmarks[p.line] = 1;
                n++;
            }
            editor.Invalidate();
            SetFindStatus(L"Mark: " + std::to_wstring(n) + L" matches in entire file");
            break;
        }
        case IDC_CLEAR_MARKS:
            d->marks.clear();
            editor.Invalidate();
            SetFindStatus(L"");
            break;
        }
        return TRUE;
    }
    case WM_CLOSE:
        ShowWindow(dlg, SW_HIDE);
        SetFocus(editor.hwnd);
        return TRUE;
    }
    return FALSE;
}

} // namespace

HWND FindDialogHwnd() { return hFind && IsWindowVisible(hFind) ? hFind : nullptr; }

void ShowFindDialog(int tab) {
    if (!hFind) {
        hFind = CreateDialogW(hInst, MAKEINTRESOURCEW(IDD_FIND), hMain, FindProc);
        RECT mr, dr;
        GetWindowRect(hMain, &mr);
        GetWindowRect(hFind, &dr);
        SetWindowPos(hFind, nullptr, mr.left + ((mr.right - mr.left) - (dr.right - dr.left)) / 2, mr.top + 120, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
    }
    TabCtrl_SetCurSel(GetDlgItem(hFind, IDC_FIND_TABS), tab);
    LayoutFindTab(hFind);
    // Prefill with the selection, or the word at the caret, like Notepad++.
    std::wstring seed = editor.CurrentWord();
    if (seed.empty() && !settings.findHistory.empty()) seed = settings.findHistory[0];
    FillCombo(hFind, IDC_FIND_WHAT, settings.findHistory, seed);
    FillCombo(hFind, IDC_REPLACE_WITH, settings.replaceHistory,
              settings.replaceHistory.empty() ? L"" : settings.replaceHistory[0]);
    Document* d = Cur();
    if (tab == 2 && settings.followCurDir && d && !d->path.empty()) {
        size_t s = d->path.find_last_of(L"\\/");
        SetDlgItemTextW(hFind, IDC_DIR, d->path.substr(0, s).c_str());
    }
    SetDlgItemTextW(hFind, IDC_FIND_STATUS, L"");
    ShowWindow(hFind, SW_SHOW);
    SetForegroundWindow(hFind);
    HWND what = GetDlgItem(hFind, IDC_FIND_WHAT);
    SetFocus(what);
    SendMessageW(what, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
}

bool FindNext(bool backward) {
    SearchSpec sp = SpecFromSettings();
    if (sp.what.empty()) {
        ShowFindDialog(0);
        return false;
    }
    return FindWith(sp, backward);
}

void SelectAndFindNext(bool backward) {
    std::wstring w = editor.CurrentWord();
    if (w.empty()) return;
    AddHistory(settings.findHistory, w);
    SearchSpec sp = SpecFromSettings();
    sp.what = w;
    sp.mode = 0;
    if (!editor.HasSelection()) {
        // Select the word first so the search continues after it.
        Pos c = editor.Caret();
        Pos s, e;
        if (editor.WordAt(c, s, e)) editor.SetSel(s, e, false);
    }
    FindWith(sp, backward);
}

// ---------------------------------------------------------------- Go To

namespace {

void FillGoto(HWND dlg) {
    Document* d = Cur();
    bool line = IsDlgButtonChecked(dlg, IDC_GOTO_LINE) == BST_CHECKED;
    Pos c = editor.Caret();
    size_t eolExtra = d->eol == Eol::CRLF ? 1 : 0;
    size_t here = line ? c.line + 1 : d->OffsetOf(c) + eolExtra * c.line;
    size_t max = line ? d->LineCount() : d->TextLength();
    SetDlgItemTextW(dlg, IDC_GOTO_HERE, std::to_wstring(here).c_str());
    SetDlgItemTextW(dlg, IDC_GOTO_MAX, std::to_wstring(max).c_str());
}

INT_PTR CALLBACK GotoProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        CheckRadioButton(dlg, IDC_GOTO_LINE, IDC_GOTO_OFFSET, IDC_GOTO_LINE);
        FillGoto(dlg);
        SetFocus(GetDlgItem(dlg, IDC_GOTO_VALUE));
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_GOTO_LINE:
        case IDC_GOTO_OFFSET: FillGoto(dlg); return TRUE;
        case IDOK: {
            BOOL ok;
            UINT v = GetDlgItemInt(dlg, IDC_GOTO_VALUE, &ok, FALSE);
            if (!ok) {
                MessageBeep(MB_OK);
                return TRUE;
            }
            Document* d = Cur();
            if (IsDlgButtonChecked(dlg, IDC_GOTO_LINE) == BST_CHECKED) {
                editor.GotoLine(std::max(1u, v) - 1);
            } else {
                // Offsets count line endings with the document's EOL length.
                size_t eolLen = d->eol == Eol::CRLF ? 2 : 1, off = v;
                Pos p = d->EndPos();
                for (int l = 0; l < d->LineCount(); l++) {
                    size_t len = d->lines[l].size();
                    if (off <= len) {
                        p = {l, (int)off};
                        break;
                    }
                    off -= std::min(off, len + eolLen);
                }
                editor.SetSel(p, p, true, true);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------- Run

std::wstring ExpandRunVariables(std::wstring cmd) {
    Document* d = Cur();
    std::wstring full = d->path;
    std::wstring dir = full.empty() ? L"" : full.substr(0, full.find_last_of(L"\\/"));
    std::wstring name = d->DisplayName();
    size_t dot = name.rfind(L'.');
    Pos c = editor.Caret();
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::pair<const wchar_t*, std::wstring> vars[] = {
        {L"$(FULL_CURRENT_PATH)", full},
        {L"$(CURRENT_DIRECTORY)", dir},
        {L"$(FILE_NAME)", name},
        {L"$(NAME_PART)", dot == std::wstring::npos ? name : name.substr(0, dot)},
        {L"$(EXT_PART)", dot == std::wstring::npos ? L"" : name.substr(dot)},
        {L"$(CURRENT_WORD)", editor.CurrentWord()},
        {L"$(NPP_DIRECTORY)", ExeDir()},
        {L"$(NPP_FULL_FILE_PATH)", exe},
        {L"$(CURRENT_LINE)", std::to_wstring(c.line + 1)},
        {L"$(CURRENT_COLUMN)", std::to_wstring(c.col + 1)},
        {L"$(CURRENT_LINESTR)", d->lines[c.line]},
    };
    for (auto& [k, v] : vars) {
        size_t p;
        size_t klen = wcslen(k);
        while ((p = cmd.find(k)) != std::wstring::npos) cmd.replace(p, klen, v);
    }
    return cmd;
}

INT_PTR CALLBACK RunProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        FillCombo(dlg, IDC_RUN_CMD, settings.runHistory, settings.runHistory.empty() ? L"" : settings.runHistory[0]);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            std::wstring raw = GetText(dlg, IDC_RUN_CMD);
            if (raw.empty()) return TRUE;
            AddHistory(settings.runHistory, raw);
            std::wstring cmd = ExpandRunVariables(raw);
            std::wstring prog, args;
            size_t i = cmd.find_first_not_of(L' ');
            if (i == std::wstring::npos) return TRUE;
            if (cmd[i] == L'"') {
                size_t e = cmd.find(L'"', i + 1);
                prog = cmd.substr(i + 1, e == std::wstring::npos ? std::wstring::npos : e - i - 1);
                args = e == std::wstring::npos ? L"" : cmd.substr(e + 1);
            } else {
                size_t e = cmd.find(L' ', i);
                prog = cmd.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i);
                args = e == std::wstring::npos ? L"" : cmd.substr(e + 1);
            }
            Document* d = Cur();
            std::wstring dir = d->path.empty() ? L"" : d->path.substr(0, d->path.find_last_of(L"\\/"));
            HINSTANCE r = ShellExecuteW(hMain, nullptr, prog.c_str(), args.c_str(), dir.empty() ? nullptr : dir.c_str(),
                                        SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) {
                MessageBoxW(dlg, (L"Cannot run \"" + prog + L"\".").c_str(), L"Run...", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------- Preferences

int CALLBACK EnumFixedFonts(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    if ((lf->lfPitchAndFamily & 3) == FIXED_PITCH && lf->lfFaceName[0] != L'@') {
        HWND cb = (HWND)lp;
        if (SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)lf->lfFaceName) == CB_ERR)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)lf->lfFaceName);
    }
    return 1;
}

INT_PTR CALLBACK PrefsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    Settings& s = settings;
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemInt(dlg, IDC_PREF_TABSIZE, s.tabWidth, FALSE);
        SetDlgItemInt(dlg, IDC_PREF_FONTSIZE, s.fontSize, FALSE);
        HWND fonts = GetDlgItem(dlg, IDC_PREF_FONT);
        HDC dc = GetDC(dlg);
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, EnumFixedFonts, (LPARAM)fonts, 0);
        ReleaseDC(dlg, dc);
        SetWindowTextW(fonts, s.fontName.c_str());
        CheckDlgButton(dlg, IDC_PREF_USESPACES, s.useSpaces);
        CheckDlgButton(dlg, IDC_PREF_AUTOINDENT, s.autoIndent);
        CheckDlgButton(dlg, IDC_PREF_SMART, s.smartHighlight);
        CheckDlgButton(dlg, IDC_PREF_AUTOCLOSE, s.autoClose);
        CheckDlgButton(dlg, IDC_PREF_AUTOCOMPLETE, s.autoComplete);
        CheckDlgButton(dlg, IDC_PREF_CURLINE, s.curLine);
        CheckDlgButton(dlg, IDC_PREF_LINENUM, s.lineNumbers);
        CheckDlgButton(dlg, IDC_PREF_BOOKMARKS, s.bookmarkMargin);
        CheckDlgButton(dlg, IDC_PREF_DARK, s.dark);
        CheckDlgButton(dlg, IDC_PREF_SESSION, s.rememberSession);
        CheckDlgButton(dlg, IDC_PREF_BACKUP, s.backup);
        CheckDlgButton(dlg, IDC_PREF_MULTIINST, s.multiInstance);
        HWND enc = GetDlgItem(dlg, IDC_PREF_ENC);
        for (const wchar_t* e : {L"ANSI", L"UTF-8", L"UTF-8 with BOM", L"UTF-16 BE BOM", L"UTF-16 LE BOM"})
            SendMessageW(enc, CB_ADDSTRING, 0, (LPARAM)e);
        SendMessageW(enc, CB_SETCURSEL, s.newEnc, 0);
        HWND eol = GetDlgItem(dlg, IDC_PREF_EOL);
        for (const wchar_t* e : {L"Windows (CR LF)", L"Unix (LF)", L"Macintosh (CR)"})
            SendMessageW(eol, CB_ADDSTRING, 0, (LPARAM)e);
        SendMessageW(eol, CB_SETCURSEL, s.newEol, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            BOOL ok;
            UINT tab = GetDlgItemInt(dlg, IDC_PREF_TABSIZE, &ok, FALSE);
            if (ok) s.tabWidth = std::clamp((int)tab, 1, 16);
            UINT size = GetDlgItemInt(dlg, IDC_PREF_FONTSIZE, &ok, FALSE);
            if (ok) s.fontSize = std::clamp((int)size, 4, 48);
            std::wstring font = GetText(dlg, IDC_PREF_FONT);
            if (!font.empty()) s.fontName = font;
            s.useSpaces = IsDlgButtonChecked(dlg, IDC_PREF_USESPACES);
            s.autoIndent = IsDlgButtonChecked(dlg, IDC_PREF_AUTOINDENT);
            s.smartHighlight = IsDlgButtonChecked(dlg, IDC_PREF_SMART);
            s.autoClose = IsDlgButtonChecked(dlg, IDC_PREF_AUTOCLOSE);
            s.autoComplete = IsDlgButtonChecked(dlg, IDC_PREF_AUTOCOMPLETE);
            s.curLine = IsDlgButtonChecked(dlg, IDC_PREF_CURLINE);
            s.lineNumbers = IsDlgButtonChecked(dlg, IDC_PREF_LINENUM);
            s.bookmarkMargin = IsDlgButtonChecked(dlg, IDC_PREF_BOOKMARKS);
            s.dark = IsDlgButtonChecked(dlg, IDC_PREF_DARK);
            s.rememberSession = IsDlgButtonChecked(dlg, IDC_PREF_SESSION);
            s.backup = IsDlgButtonChecked(dlg, IDC_PREF_BACKUP);
            s.multiInstance = IsDlgButtonChecked(dlg, IDC_PREF_MULTIINST);
            s.newEnc = (int)SendDlgItemMessageW(dlg, IDC_PREF_ENC, CB_GETCURSEL, 0, 0);
            s.newEol = (int)SendDlgItemMessageW(dlg, IDC_PREF_EOL, CB_GETCURSEL, 0, 0);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------- digests

const wchar_t* kAlgoNames[] = {L"MD5", L"SHA-1", L"SHA-256", L"SHA-512"};
const wchar_t* kAlgoIds[] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA512_ALGORITHM};

struct Hasher {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD len = 0;
    explicit Hasher(int algo) {
        if (BCryptOpenAlgorithmProvider(&alg, kAlgoIds[algo], nullptr, 0) != 0) return;
        DWORD cb;
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof(len), &cb, 0);
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    }
    void Update(const void* p, size_t n) {
        if (hash) BCryptHashData(hash, (PUCHAR)p, (ULONG)n, 0);
    }
    std::wstring Hex() {
        std::vector<UCHAR> out(len);
        if (!hash || BCryptFinishHash(hash, out.data(), len, 0) != 0) return L"";
        static const wchar_t* digits = L"0123456789abcdef";
        std::wstring s;
        for (UCHAR b : out) {
            s += digits[b >> 4];
            s += digits[b & 15];
        }
        return s;
    }
    ~Hasher() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

struct HashDialogState {
    int algo;
    bool files;
};

void UpdateTextHash(HWND dlg, int algo) {
    std::wstring in = GetText(dlg, IDC_HASH_INPUT);
    std::wstring out;
    if (IsDlgButtonChecked(dlg, IDC_HASH_PERLINE) == BST_CHECKED) {
        size_t s = 0;
        for (size_t i = 0; i <= in.size(); i++)
            if (i == in.size() || in[i] == L'\n') {
                std::wstring line = in.substr(s, i - s);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                out += (line.empty() ? L"" : HashString(algo, line)) + L"\r\n";
                s = i + 1;
            }
    } else if (!in.empty()) {
        std::wstring norm;
        for (wchar_t c : in)
            if (c != L'\r') norm += c;
        out = HashString(algo, norm);
    }
    SetDlgItemTextW(dlg, IDC_HASH_OUTPUT, out.c_str());
}

INT_PTR CALLBACK HashProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (HashDialogState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        st = (HashDialogState*)lp;
        std::wstring title = std::wstring(L"Generate ") + kAlgoNames[st->algo] + L" digest" + (st->files ? L" from files" : L"");
        SetWindowTextW(dlg, title.c_str());
        ShowWindow(GetDlgItem(dlg, IDC_HASH_INPUT), st->files ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(dlg, IDC_HASH_PERLINE), st->files ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(dlg, IDC_HASH_FILES), st->files ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(dlg, IDC_HASH_LABEL, st->files ? L"" : L"Text to hash :");
        HFONT mono = CreateFontW(-MulDiv(9, GetDpiForWindow(dlg), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        SendDlgItemMessageW(dlg, IDC_HASH_OUTPUT, WM_SETFONT, (WPARAM)mono, FALSE);
        SetPropW(dlg, L"monoFont", mono);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HASH_INPUT:
            if (HIWORD(wp) == EN_CHANGE) UpdateTextHash(dlg, st->algo);
            return TRUE;
        case IDC_HASH_PERLINE: UpdateTextHash(dlg, st->algo); return TRUE;
        case IDC_HASH_FILES: {
            std::wstring buf(65536, L'\0');
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = dlg;
            ofn.lpstrFilter = L"All types (*.*)\0*.*\0";
            ofn.lpstrFile = &buf[0];
            ofn.nMaxFile = (DWORD)buf.size();
            ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return TRUE;
            std::vector<std::wstring> paths;
            const wchar_t* p = buf.c_str();
            std::wstring first = p;
            p += first.size() + 1;
            if (!*p) paths.push_back(first);
            while (*p) {
                std::wstring n = p;
                paths.push_back(first + L"\\" + n);
                p += n.size() + 1;
            }
            std::wstring out;
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            for (auto& path : paths) {
                HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) continue;
                Hasher h(st->algo);
                std::vector<char> chunk(1 << 20);
                DWORD got;
                while (ReadFile(f, chunk.data(), (DWORD)chunk.size(), &got, nullptr) && got) h.Update(chunk.data(), got);
                CloseHandle(f);
                out += h.Hex() + L"  " + path + L"\r\n";
            }
            SetCursor(old);
            SetDlgItemTextW(dlg, IDC_HASH_OUTPUT, out.c_str());
            return TRUE;
        }
        case IDC_HASH_COPY: SetClipboardText(GetText(dlg, IDC_HASH_OUTPUT)); return TRUE;
        case IDCANCEL:
            if (HFONT f = (HFONT)RemovePropW(dlg, L"monoFont")) DeleteObject(f);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------- Windows

void FillWindowsList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_WIN_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    int tabs[] = {110};
    SendMessageW(list, LB_SETTABSTOPS, 1, (LPARAM)tabs);
    for (auto& d : docs) {
        std::wstring row = (d->IsModified() ? L"*" : L"") + d->DisplayName() + L"\t" + (d->path.empty() ? L"" : d->path);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    SendMessageW(list, LB_SETHORIZONTALEXTENT, 2000, 0);
    SendMessageW(list, LB_SETSEL, TRUE, current);
}

std::vector<int> SelectedWindows(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_WIN_LIST);
    int n = (int)SendMessageW(list, LB_GETSELCOUNT, 0, 0);
    std::vector<int> v(std::max(0, n));
    if (n > 0) SendMessageW(list, LB_GETSELITEMS, n, (LPARAM)v.data());
    return v;
}

INT_PTR CALLBACK WindowsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: FillWindowsList(dlg); return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_WIN_LIST:
            if (HIWORD(wp) != LBN_DBLCLK) return FALSE;
            [[fallthrough]];
        case IDC_WIN_ACTIVATE: {
            auto v = SelectedWindows(dlg);
            if (!v.empty()) Activate(v[0]);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDC_WIN_SAVE:
            for (int i : SelectedWindows(dlg)) SaveDoc(i, false);
            FillWindowsList(dlg);
            return TRUE;
        case IDC_WIN_CLOSE: {
            auto v = SelectedWindows(dlg);
            std::sort(v.rbegin(), v.rend());
            for (int i : v)
                if (!CloseDoc(i)) break;
            FillWindowsList(dlg);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
    }
    return FALSE;
}

struct MacroMulti {
    int times;
    bool eof;
};

INT_PTR CALLBACK MacroProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* mm = (MacroMulti*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        CheckRadioButton(dlg, IDC_MACRO_TIMES_RADIO, IDC_MACRO_EOF_RADIO, IDC_MACRO_TIMES_RADIO);
        SetDlgItemInt(dlg, IDC_MACRO_TIMES, 1, FALSE);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            BOOL ok;
            mm->times = std::max(1, (int)GetDlgItemInt(dlg, IDC_MACRO_TIMES, &ok, FALSE));
            mm->eof = IsDlgButtonChecked(dlg, IDC_MACRO_EOF_RADIO) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_ABOUT_TEXT,
                        L"Tetradkata++ 1.0\n\n"
                        L"A native Windows text editor rebuilt from scratch after a REA reverse-engineering study "
                        L"of Notepad++ 8.9.8.1: its menus, command IDs, dialogs, default styles and file layout.\n\n"
                        L"No Notepad++ code is used. Notepad++ is by Don Ho and contributors (GPL).");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
    }
    return FALSE;
}

} // namespace

void ShowGotoDialog() { DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_GOTO), hMain, GotoProc); }

void ShowRunDialog() { DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_RUN), hMain, RunProc); }

bool ShowPreferences() { return DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_PREFERENCES), hMain, PrefsProc) == IDOK; }

std::wstring HashString(int algo, const std::wstring& text) {
    std::string utf8 = EncodeText(text, Enc::UTF8, 0);
    Hasher h(algo);
    h.Update(utf8.data(), utf8.size());
    return h.Hex();
}

void ShowHashDialog(int algo, bool files) {
    HashDialogState st{algo, files};
    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_HASH), hMain, HashProc, (LPARAM)&st);
}

void ShowWindowsDialog() { DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_WINDOWS), hMain, WindowsProc); }

bool ShowMacroMultiDialog(int& times, bool& untilEof) {
    MacroMulti mm{1, false};
    if (DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_MACRO_MULTI), hMain, MacroProc, (LPARAM)&mm) != IDOK) return false;
    times = mm.times;
    untilEof = mm.eof;
    return true;
}

void ShowAbout() { DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_ABOUT), hMain, AboutProc); }
