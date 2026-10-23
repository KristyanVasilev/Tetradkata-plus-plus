// Tetradkata++ main window: tabs, menus, commands, session and settings.
#include "app.h"
#include "fileio.h"
#include "lexer.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <clocale>
#include <cwctype>
#include <random>
#include <unordered_set>

namespace App {
HINSTANCE hInst;
HWND hMain;
Editor editor;
Settings settings;
std::vector<std::unique_ptr<Document>> docs;
int current = -1;
} // namespace App

using namespace App;

void BuildLanguageMenu();

namespace {

const wchar_t kMainClass[] = L"Tetradkata++";
const wchar_t kAppName[] = L"Tetradkata++";
constexpr ULONG_PTR kCopyDataTag = 0x54505031; // "TPP1"
constexpr UINT WM_APP_CHECKFILES = WM_APP + 1;
constexpr UINT_PTR kBackupTimer = 10, kMonitorTimer = 11, kFlashTimer = 12;

HWND hTab, hStatus, hToolbar, hResults;
HACCEL hAccel;
HIMAGELIST tabImages;
HFONT uiFont;
int dpi = 96;
bool resultsVisible = false;
bool fullScreen = false, postIt = false;
LONG savedStyle = 0;
WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
bool noSession = false;
bool checkingFiles = false;
std::wstring flashText;
std::vector<std::wstring> closedFiles;
std::vector<MacroStep> macro;
bool recordingMacro = false;
bool beginSelectActive = false;
Pos beginSelectPos;
std::wstring lastSessionSignature;
int splitterDragY = -1;

struct CmdLine {
    std::vector<std::wstring> files;
    bool multiInst = false, noSession = false, readOnly = false, onTop = false;
    int line = -1, col = -1;
    std::wstring lang;
};

int Scale(int v) { return MulDiv(v, dpi, 96); }

// ---------------------------------------------------------------- small helpers

std::wstring Num(size_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}

std::wstring DirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

std::wstring FileNameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &out[0], nullptr);
    out.resize(n);
    return out;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int Ask(const std::wstring& text, UINT flags) { return MessageBoxW(hMain, text.c_str(), kAppName, flags); }

void Error(const std::wstring& text) { MessageBoxW(hMain, text.c_str(), kAppName, MB_OK | MB_ICONERROR); }

// ---------------------------------------------------------------- INI files (UTF-16 so any path round-trips)

struct IniWriter {
    std::wstring out;
    void Section(const std::wstring& name) { out += L"[" + name + L"]\r\n"; }
    void Put(const std::wstring& key, const std::wstring& value) {
        std::wstring v = value;
        v.erase(std::remove_if(v.begin(), v.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }), v.end());
        out += key + L"=" + v + L"\r\n";
    }
    void Put(const std::wstring& key, int value) { Put(key, std::to_wstring(value)); }
    bool Save(const std::wstring& path) {
        std::string bytes = "\xFF\xFE";
        bytes.append((const char*)out.data(), out.size() * sizeof(wchar_t));
        std::wstring tmp = path + L".tmp", err;
        DeleteFileW(tmp.c_str());
        if (!WriteFileBytes(tmp, bytes, err)) return false;
        return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
};

std::wstring IniStr(const std::wstring& file, const std::wstring& sec, const std::wstring& key,
                    const std::wstring& def = L"") {
    std::wstring buf(1024, L'\0');
    for (;;) {
        DWORD n = GetPrivateProfileStringW(sec.c_str(), key.c_str(), def.c_str(), &buf[0], (DWORD)buf.size(),
                                           file.c_str());
        if (n < buf.size() - 2) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 4);
    }
}

int IniInt(const std::wstring& file, const std::wstring& sec, const std::wstring& key, int def) {
    std::wstring s = IniStr(file, sec, key, L"");
    return s.empty() ? def : _wtoi(s.c_str());
}

bool IniBool(const std::wstring& file, const std::wstring& sec, const std::wstring& key, bool def) {
    return IniInt(file, sec, key, def ? 1 : 0) != 0;
}

void ReadList(const std::wstring& file, const wchar_t* sec, std::vector<std::wstring>& list) {
    list.clear();
    int n = IniInt(file, sec, L"count", 0);
    for (int i = 0; i < n && i < 100; i++) {
        std::wstring v = IniStr(file, sec, L"item" + std::to_wstring(i));
        if (!v.empty()) list.push_back(v);
    }
}

void WriteList(IniWriter& w, const wchar_t* sec, const std::vector<std::wstring>& list) {
    w.Section(sec);
    w.Put(L"count", (int)list.size());
    for (size_t i = 0; i < list.size(); i++) w.Put(L"item" + std::to_wstring(i), list[i]);
}

void LoadSettings() {
    std::wstring f = ConfigDir() + L"\\config.ini";
    Settings& s = settings;
    const wchar_t* E = L"Editor";
    s.tabWidth = std::clamp(IniInt(f, E, L"tabWidth", 4), 1, 16);
    s.useSpaces = IniBool(f, E, L"useSpaces", false);
    s.fontName = IniStr(f, E, L"fontName", L"Courier New");
    s.fontSize = std::clamp(IniInt(f, E, L"fontSize", 10), 4, 48);
    s.dark = IniBool(f, E, L"dark", false);
    s.smartHighlight = IniBool(f, E, L"smartHighlight", true);
    s.autoIndent = IniBool(f, E, L"autoIndent", true);
    s.autoClose = IniBool(f, E, L"autoClose", false);
    s.autoComplete = IniBool(f, E, L"autoComplete", true);
    s.lineNumbers = IniBool(f, E, L"lineNumbers", true);
    s.bookmarkMargin = IniBool(f, E, L"bookmarkMargin", true);
    s.curLine = IniBool(f, E, L"currentLine", true);
    s.wordWrap = IniBool(f, E, L"wordWrap", false);
    s.showSpaces = IniBool(f, E, L"showSpaces", false);
    s.showEol = IniBool(f, E, L"showEol", false);
    s.indentGuides = IniBool(f, E, L"indentGuides", true);
    s.zoom = std::clamp(IniInt(f, E, L"zoom", 0), -10, 20);
    const wchar_t* G = L"General";
    s.rememberSession = IniBool(f, G, L"rememberSession", true);
    s.backup = IniBool(f, G, L"backup", true);
    s.multiInstance = IniBool(f, G, L"multiInstance", false);
    s.alwaysOnTop = IniBool(f, G, L"alwaysOnTop", false);
    s.newEnc = std::clamp(IniInt(f, G, L"newEncoding", (int)Enc::UTF8), 0, 4);
    s.newEol = std::clamp(IniInt(f, G, L"newEol", 0), 0, 2);
    s.resultsHeight = IniInt(f, G, L"resultsHeight", 0);
    const wchar_t* W = L"Window";
    if (IniBool(f, W, L"valid", false)) {
        WINDOWPLACEMENT& p = s.placement;
        p.length = sizeof(p);
        p.showCmd = IniInt(f, W, L"show", SW_SHOWNORMAL);
        p.rcNormalPosition = {IniInt(f, W, L"left", 100), IniInt(f, W, L"top", 100), IniInt(f, W, L"right", 1100),
                              IniInt(f, W, L"bottom", 800)};
        s.hasPlacement = true;
    }
    const wchar_t* F = L"Find";
    s.matchCase = IniBool(f, F, L"matchCase", false);
    s.wholeWord = IniBool(f, F, L"wholeWord", false);
    s.wrapAround = IniBool(f, F, L"wrap", true);
    s.searchMode = std::clamp(IniInt(f, F, L"mode", 0), 0, 2);
    s.subfolders = IniBool(f, F, L"subfolders", true);
    s.hiddenFolders = IniBool(f, F, L"hidden", false);
    s.followCurDir = IniBool(f, F, L"followCurDir", true);
    s.findDir = IniStr(f, F, L"dir");
    s.findFilters = IniStr(f, F, L"filters", L"*.*");
    ReadList(f, L"Recent", s.recent);
    ReadList(f, L"FindHistory", s.findHistory);
    ReadList(f, L"ReplaceHistory", s.replaceHistory);
    ReadList(f, L"RunHistory", s.runHistory);
}

void FillTabIcons();

} // namespace

std::wstring App::ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return DirOf(buf);
}

std::wstring App::ConfigDir() {
    static std::wstring dir;
    if (!dir.empty()) return dir;
    std::wstring exe = ExeDir();
    if (FileExists(exe + L"\\doLocalConf.ini")) {
        dir = exe; // portable mode, like Notepad++'s doLocalConf.xml (finding #8)
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
            dir = std::wstring(p) + L"\\Tetradkata++";
            CoTaskMemFree(p);
        } else {
            dir = exe;
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void App::SaveSettings() {
    Settings& s = settings;
    IniWriter w;
    w.Section(L"Editor");
    w.Put(L"tabWidth", s.tabWidth);
    w.Put(L"useSpaces", s.useSpaces);
    w.Put(L"fontName", s.fontName);
    w.Put(L"fontSize", s.fontSize);
    w.Put(L"dark", s.dark);
    w.Put(L"smartHighlight", s.smartHighlight);
    w.Put(L"autoIndent", s.autoIndent);
    w.Put(L"autoClose", s.autoClose);
    w.Put(L"autoComplete", s.autoComplete);
    w.Put(L"lineNumbers", s.lineNumbers);
    w.Put(L"bookmarkMargin", s.bookmarkMargin);
    w.Put(L"currentLine", s.curLine);
    w.Put(L"wordWrap", s.wordWrap);
    w.Put(L"showSpaces", s.showSpaces);
    w.Put(L"showEol", s.showEol);
    w.Put(L"indentGuides", s.indentGuides);
    w.Put(L"zoom", editor.zoom);
    w.Section(L"General");
    w.Put(L"rememberSession", s.rememberSession);
    w.Put(L"backup", s.backup);
    w.Put(L"multiInstance", s.multiInstance);
    w.Put(L"alwaysOnTop", s.alwaysOnTop);
    w.Put(L"newEncoding", s.newEnc);
    w.Put(L"newEol", s.newEol);
    w.Put(L"resultsHeight", s.resultsHeight);
    w.Section(L"Window");
    WINDOWPLACEMENT p{sizeof(p)};
    if (fullScreen || postIt)
        p = savedPlacement;
    else
        GetWindowPlacement(hMain, &p);
    w.Put(L"valid", 1);
    w.Put(L"show", p.showCmd == SW_SHOWMINIMIZED ? SW_SHOWNORMAL : (int)p.showCmd);
    w.Put(L"left", p.rcNormalPosition.left);
    w.Put(L"top", p.rcNormalPosition.top);
    w.Put(L"right", p.rcNormalPosition.right);
    w.Put(L"bottom", p.rcNormalPosition.bottom);
    w.Section(L"Find");
    w.Put(L"matchCase", s.matchCase);
    w.Put(L"wholeWord", s.wholeWord);
    w.Put(L"wrap", s.wrapAround);
    w.Put(L"mode", s.searchMode);
    w.Put(L"subfolders", s.subfolders);
    w.Put(L"hidden", s.hiddenFolders);
    w.Put(L"followCurDir", s.followCurDir);
    w.Put(L"dir", s.findDir);
    w.Put(L"filters", s.findFilters);
    WriteList(w, L"Recent", s.recent);
    WriteList(w, L"FindHistory", s.findHistory);
    WriteList(w, L"ReplaceHistory", s.replaceHistory);
    WriteList(w, L"RunHistory", s.runHistory);
    w.Save(ConfigDir() + L"\\config.ini");
}

void App::SetClipboardText(const std::wstring& text) {
    if (!OpenClipboard(hMain)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// ---------------------------------------------------------------- documents and tabs

Document* App::Cur() { return current >= 0 && current < (int)docs.size() ? docs[current].get() : nullptr; }

int App::IndexOf(const Document* d) {
    for (size_t i = 0; i < docs.size(); i++)
        if (docs[i].get() == d) return (int)i;
    return -1;
}

namespace {

void AddRecent(const std::wstring& path) {
    auto& r = settings.recent;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), path.c_str()) == 0; }),
            r.end());
    r.insert(r.begin(), path);
    if (r.size() > 15) r.resize(15);
}

void RemoveRecent(const std::wstring& path) {
    auto& r = settings.recent;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), path.c_str()) == 0; }),
            r.end());
}

int TabImage(const Document* d) {
    if (d->readOnly || d->fileReadOnly || d->monitoring) return 2;
    return d->IsModified() ? 1 : 0;
}

void InsertTab(int index) {
    Document* d = docs[index].get();
    std::wstring name = d->DisplayName();
    TCITEMW ti{};
    ti.mask = TCIF_TEXT | TCIF_IMAGE;
    ti.pszText = &name[0];
    ti.iImage = TabImage(d);
    TabCtrl_InsertItem(hTab, index, &ti);
}

int NextUntitledNumber() {
    for (int n = 1;; n++) {
        bool used = false;
        for (auto& d : docs)
            if (d->path.empty() && d->untitledNumber == n) used = true;
        if (!used) return n;
    }
}

int NewDocument() {
    auto d = std::make_unique<Document>();
    d->untitledNumber = NextUntitledNumber();
    d->title = L"new " + std::to_wstring(d->untitledNumber);
    d->enc = (Enc)settings.newEnc;
    d->eol = (Eol)settings.newEol;
    docs.push_back(std::move(d));
    int index = (int)docs.size() - 1;
    InsertTab(index);
    Activate(index);
    return index;
}

bool IsPristine(const Document* d) {
    return d->path.empty() && !d->IsModified() && d->LineCount() == 1 && d->lines[0].empty();
}

void ReloadDocument(Document* d, int forceEnc = -1, UINT cp = 0) {
    Pos caret = d->caret, anchor = d->anchor;
    int top = d->topRow;
    std::wstring err;
    if (!LoadDocument(d->path, *d, err, forceEnc, cp)) {
        Error(L"Cannot reload \"" + d->path + L"\":\n" + err);
        return;
    }
    d->caret = d->Clamp(caret);
    d->anchor = d->Clamp(anchor);
    d->topRow = top;
    int i = IndexOf(d);
    if (i == current) editor.DocumentChanged();
    UpdateTab(i);
    UpdateTitle();
    UpdateStatus();
}

std::wstring AskSavePath(Document* d, const wchar_t* title) {
    std::wstring buf(32768, L'\0');
    std::wstring name = d->path.empty() ? d->title : FileNameOf(d->path);
    wcsncpy(&buf[0], name.c_str(), buf.size() - 1);
    std::wstring dir = d->path.empty() ? L"" : DirOf(d->path);
    std::wstring defExt = L"txt";
    const Language& L = GetLanguage(d->lang);
    if (L.exts) {
        const wchar_t* sp = wcschr(L.exts, L' ');
        defExt = sp ? std::wstring(L.exts, sp - L.exts) : std::wstring(L.exts);
    }
    std::wstring filter = std::wstring(L.longName) + L" (*." + defExt + L")" + L'\0' + L"*." + defExt + L'\0' +
                          L"All types (*.*)" + L'\0' + L"*.*" + L'\0' + L'\0';
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hMain;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = d->path.empty() ? 1 : 2;
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return L"";
    return buf.c_str();
}

void DeleteBackup(Document* d) {
    if (!d->backupPath.empty()) DeleteFileW(d->backupPath.c_str());
    d->backupPath.clear();
    d->backupVersion = -1;
}

} // namespace

void App::UpdateTab(int index) {
    if (index < 0 || index >= (int)docs.size()) return;
    Document* d = docs[index].get();
    std::wstring name = d->DisplayName();
    TCITEMW ti{};
    ti.mask = TCIF_TEXT | TCIF_IMAGE;
    ti.pszText = &name[0];
    ti.iImage = TabImage(d);
    TabCtrl_SetItem(hTab, index, &ti);
}

void App::UpdateTitle() {
    Document* d = Cur();
    if (!d) return;
    std::wstring t = (d->IsModified() ? L"*" : L"") + (d->path.empty() ? d->title : d->path) + L" - " + kAppName;
    SetWindowTextW(hMain, t.c_str());
}

void App::FlashStatus(const std::wstring& text) {
    flashText = text;
    SetTimer(hMain, kFlashTimer, 4000, nullptr);
    UpdateStatus();
}

void App::UpdateStatus() {
    Document* d = Cur();
    if (!d || !hStatus) return;
    std::wstring parts[6];
    parts[0] = flashText.empty() ? GetLanguage(d->lang).longName : flashText;
    parts[1] = L"length : " + Num(d->TextLength()) + L"    lines : " + Num(d->LineCount());
    Pos c = editor.Caret();
    const std::wstring& line = d->lines[c.line];
    int col = 0;
    for (int i = 0; i < c.col && i < (int)line.size(); i++)
        col += line[i] == L'\t' ? settings.tabWidth - col % settings.tabWidth : 1;
    parts[2] = L"Ln : " + Num(c.line + 1) + L"    Col : " + Num(col + 1);
    if (editor.HasSelection()) {
        Pos a, b;
        editor.GetSel(a, b);
        size_t chars = d->OffsetOf(b) - d->OffsetOf(a);
        parts[2] += L"    Sel : " + Num(chars) + L" | " + Num(b.line - a.line + 1);
    } else {
        size_t off = d->OffsetOf(c) + (d->eol == Eol::CRLF ? c.line : 0);
        parts[2] += L"    Pos : " + Num(off + 1);
    }
    parts[3] = EolName(d->eol);
    parts[4] = EncodingName(d->enc, d->codepage);
    parts[5] = editor.overwrite ? L"OVR" : L"INS";
    if (recordingMacro) parts[5] += L" REC";
    for (int i = 0; i < 6; i++) SendMessageW(hStatus, SB_SETTEXTW, i, (LPARAM)parts[i].c_str());
}

void App::Activate(int index) {
    if (index < 0 || index >= (int)docs.size()) return;
    current = index;
    TabCtrl_SetCurSel(hTab, index);
    editor.SetDocument(docs[index].get());
    beginSelectActive = false;
    UpdateTitle();
    UpdateStatus();
}

int App::OpenFile(const std::wstring& rawPath, bool activate) {
    std::wstring path = FullPath(rawPath);
    for (size_t i = 0; i < docs.size(); i++)
        if (!docs[i]->path.empty() && _wcsicmp(docs[i]->path.c_str(), path.c_str()) == 0) {
            if (activate) Activate((int)i);
            return (int)i;
        }
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        int last = -1, count = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count++ < 200)
                    last = OpenFile(path + L"\\" + fd.cFileName, false);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (last >= 0 && activate) Activate(last);
        return last;
    }
    auto d = std::make_unique<Document>();
    d->eol = (Eol)settings.newEol;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (Ask(L"\"" + path + L"\" doesn't exist. Create it?", MB_YESNO | MB_ICONQUESTION) != IDYES) return -1;
        d->path = path;
        d->enc = (Enc)settings.newEnc;
        d->SetDirty();
    } else {
        std::wstring err;
        if (!LoadDocument(path, *d, err)) {
            Error(L"Cannot open file \"" + path + L"\":\n" + err);
            return -1;
        }
    }
    d->lang = LanguageForPath(path);
    RemoveRecent(path);
    // A single untouched "new 1" tab is replaced by the opened file.
    if (docs.size() == 1 && IsPristine(docs[0].get())) {
        editor.SetDocument(nullptr);
        docs[0] = std::move(d);
        UpdateTab(0);
        current = -1;
        if (activate) Activate(0);
        return 0;
    }
    docs.push_back(std::move(d));
    int index = (int)docs.size() - 1;
    InsertTab(index);
    if (activate) Activate(index);
    return index;
}

bool App::SaveDoc(int index, bool saveAs) {
    if (index < 0 || index >= (int)docs.size()) return false;
    Document* d = docs[index].get();
    std::wstring path = d->path;
    bool named = path.empty();
    if (saveAs || path.empty()) {
        if (index != current) Activate(index);
        path = AskSavePath(d, saveAs ? L"Save As" : L"Save");
        if (path.empty()) return false;
        for (size_t i = 0; i < docs.size(); i++)
            if ((int)i != index && _wcsicmp(docs[i]->path.c_str(), path.c_str()) == 0) {
                Error(L"The file \"" + path + L"\" is already opened in Tetradkata++.\nClose it first.");
                return false;
            }
        named = true;
    } else if (d->fileReadOnly) {
        Error(L"\"" + path + L"\" is read-only.\nUse Save As, or clear the read-only attribute (Edit menu).");
        return false;
    }
    std::wstring err;
    if (!SaveDocument(path, *d, err)) {
        Error(L"Cannot save file \"" + path + L"\":\n" + err);
        return false;
    }
    d->fileReadOnly = false;
    if (named) {
        if (d->lang == 0) {
            d->lang = LanguageForPath(path);
            d->lexValid = 1;
        }
        d->untitledNumber = 0;
        RemoveRecent(path);
    }
    DeleteBackup(d);
    UpdateTab(index);
    if (index == current) {
        editor.Invalidate();
        UpdateTitle();
        UpdateStatus();
    }
    return true;
}

bool App::CloseDoc(int index, bool prompt) {
    if (index < 0 || index >= (int)docs.size()) return false;
    Document* d = docs[index].get();
    if (prompt && d->IsModified()) {
        Activate(index);
        int r = Ask(L"Save file \"" + (d->path.empty() ? d->title : d->path) + L"\" ?",
                    MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES && !SaveDoc(index, false)) return false;
    }
    if (!d->path.empty()) {
        AddRecent(d->path);
        closedFiles.push_back(d->path);
    }
    DeleteBackup(d);
    ForgetDocumentInResults(d);
    if (docs.size() == 1) {
        if (IsPristine(d)) return true;
        NewDocument(); // keeps one "new 1" tab, the way Notepad++ does
    }
    bool wasCurrent = index == current;
    if (wasCurrent) editor.SetDocument(nullptr);
    TabCtrl_DeleteItem(hTab, index);
    docs.erase(docs.begin() + index);
    int next = current;
    if (index < current)
        next = current - 1;
    else if (wasCurrent)
        next = std::min(index, (int)docs.size() - 1);
    current = -1;
    Activate(next);
    return true;
}

namespace {

// ---------------------------------------------------------------- session & backup

std::wstring SessionSignature() {
    std::wstring s = std::to_wstring(current);
    for (auto& d : docs) s += L"|" + d->path + L":" + std::to_wstring(d->version) + L":" + std::to_wstring(d->IsModified());
    return s;
}

void WriteSession(const std::wstring& file, bool withBackups) {
    std::wstring backupDir = ConfigDir() + L"\\backup";
    if (withBackups) CreateDirectoryW(backupDir.c_str(), nullptr);
    std::unordered_set<std::wstring> keep;
    IniWriter w;
    int written = 0, active = 0;
    std::wstring body;
    for (size_t i = 0; i < docs.size(); i++) {
        Document* d = docs[i].get();
        if (IsPristine(d)) continue;
        bool needBackup = withBackups && (d->IsModified() || d->path.empty());
        if (!withBackups && d->path.empty()) continue;
        if (needBackup) {
            if (d->backupPath.empty() || d->backupVersion != d->version || !FileExists(d->backupPath)) {
                if (d->backupPath.empty()) {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    wchar_t stamp[64];
                    wsprintfW(stamp, L"@%04d-%02d-%02d_%02d%02d%02d_%d", st.wYear, st.wMonth, st.wDay, st.wHour,
                              st.wMinute, st.wSecond, (int)i);
                    d->backupPath = backupDir + L"\\" + d->DisplayName() + stamp;
                }
                std::wstring err;
                if (WriteFileBytes(d->backupPath + L".tmp", EncodeText(d->GetAllText(), Enc::UTF8, 0), err)) {
                    MoveFileExW((d->backupPath + L".tmp").c_str(), d->backupPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                    d->backupVersion = d->version;
                }
            }
            keep.insert(d->backupPath);
        } else if (withBackups) {
            DeleteBackup(d);
        }
        std::wstring sec = L"File" + std::to_wstring(written);
        w.Section(sec);
        w.Put(L"path", d->path);
        w.Put(L"title", d->title);
        w.Put(L"untitled", d->untitledNumber);
        w.Put(L"backup", needBackup ? d->backupPath : L"");
        w.Put(L"lang", GetLanguage(d->lang).menuId);
        w.Put(L"enc", (int)d->enc);
        w.Put(L"cp", (int)d->codepage);
        w.Put(L"eol", (int)d->eol);
        w.Put(L"caretLine", d->caret.line);
        w.Put(L"caretCol", d->caret.col);
        w.Put(L"anchorLine", d->anchor.line);
        w.Put(L"anchorCol", d->anchor.col);
        w.Put(L"topRow", d->topRow);
        w.Put(L"readOnly", d->readOnly);
        std::wstring bm;
        for (int l = 0; l < d->LineCount(); l++)
            if (d->bookmarks[l]) bm += (bm.empty() ? L"" : L",") + std::to_wstring(l);
        w.Put(L"bookmarks", bm);
        if ((int)i == current) active = written;
        written++;
    }
    IniWriter head;
    head.Section(L"Session");
    head.Put(L"count", written);
    head.Put(L"active", active);
    head.out += w.out;
    head.Save(file);
    if (withBackups) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((backupDir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring p = backupDir + L"\\" + fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !keep.count(p)) DeleteFileW(p.c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
}

void SaveSession() {
    if (!settings.rememberSession || noSession) return;
    WriteSession(ConfigDir() + L"\\session.ini", settings.backup);
    lastSessionSignature = SessionSignature();
}

int ReadSession(const std::wstring& file, bool withBackups) {
    int count = IniInt(file, L"Session", L"count", 0);
    int active = IniInt(file, L"Session", L"active", 0);
    int firstIndex = -1, activeIndex = -1;
    for (int i = 0; i < count; i++) {
        std::wstring sec = L"File" + std::to_wstring(i);
        std::wstring path = IniStr(file, sec, L"path");
        std::wstring backup = withBackups ? IniStr(file, sec, L"backup") : L"";
        bool hasBackup = !backup.empty() && FileExists(backup);
        if (!path.empty() && !hasBackup && FileExists(path)) {
            int idx = OpenFile(path, false);
            if (idx < 0) continue;
            Document* d = docs[idx].get();
            int lang = LanguageByMenuId(IniInt(file, sec, L"lang", 46016));
            if (lang >= 0) d->lang = lang;
            Enc enc = (Enc)IniInt(file, sec, L"enc", (int)d->enc);
            UINT cp = IniInt(file, sec, L"cp", 0);
            if (enc != d->enc || cp != d->codepage) {
                std::wstring err;
                LoadDocument(path, *d, err, (int)enc, cp);
            }
            d->readOnly = IniBool(file, sec, L"readOnly", false);
            d->caret = d->Clamp({IniInt(file, sec, L"caretLine", 0), IniInt(file, sec, L"caretCol", 0)});
            d->anchor = d->Clamp({IniInt(file, sec, L"anchorLine", 0), IniInt(file, sec, L"anchorCol", 0)});
            d->topRow = IniInt(file, sec, L"topRow", 0);
            std::wstring bm = IniStr(file, sec, L"bookmarks");
            for (const wchar_t* p = bm.c_str(); *p;) {
                int l = _wtoi(p);
                if (l >= 0 && l < d->LineCount()) d->bookmarks[l] = 1;
                const wchar_t* comma = wcschr(p, L',');
                if (!comma) break;
                p = comma + 1;
            }
            if (firstIndex < 0) firstIndex = idx;
            if (i == active) activeIndex = idx;
            UpdateTab(idx);
            continue;
        }
        if (!hasBackup) continue;
        // Restore an unsaved document from its backup snapshot.
        std::string bytes;
        std::wstring err;
        if (!ReadFileBytes(backup, bytes, err)) continue;
        auto d = std::make_unique<Document>();
        Enc be = Enc::UTF8;
        UINT bcp = 0;
        d->SetAllText(DecodeBytes(bytes, be, bcp, false));
        d->path = path;
        d->title = IniStr(file, sec, L"title");
        d->untitledNumber = IniInt(file, sec, L"untitled", 0);
        if (d->path.empty() && d->title.empty()) {
            d->untitledNumber = NextUntitledNumber();
            d->title = L"new " + std::to_wstring(d->untitledNumber);
        }
        int lang = LanguageByMenuId(IniInt(file, sec, L"lang", 46016));
        d->lang = lang >= 0 ? lang : 0;
        d->enc = (Enc)std::clamp(IniInt(file, sec, L"enc", (int)Enc::UTF8), 0, 5);
        d->codepage = IniInt(file, sec, L"cp", 0);
        d->eol = (Eol)std::clamp(IniInt(file, sec, L"eol", 0), 0, 2);
        d->backupPath = backup;
        d->backupVersion = d->version;
        d->readOnly = IniBool(file, sec, L"readOnly", false);
        if (!d->path.empty()) d->hasFileTime = GetFileTime(d->path, d->fileTime);
        d->SetDirty();
        d->caret = d->Clamp({IniInt(file, sec, L"caretLine", 0), IniInt(file, sec, L"caretCol", 0)});
        d->anchor = d->Clamp({IniInt(file, sec, L"anchorLine", 0), IniInt(file, sec, L"anchorCol", 0)});
        d->topRow = IniInt(file, sec, L"topRow", 0);
        if (docs.size() == 1 && IsPristine(docs[0].get())) {
            editor.SetDocument(nullptr);
            docs[0] = std::move(d);
            UpdateTab(0);
            current = -1;
            if (firstIndex < 0) firstIndex = 0;
            if (i == active) activeIndex = 0;
            continue;
        }
        docs.push_back(std::move(d));
        int idx = (int)docs.size() - 1;
        InsertTab(idx);
        if (firstIndex < 0) firstIndex = idx;
        if (i == active) activeIndex = idx;
    }
    return activeIndex >= 0 ? activeIndex : firstIndex;
}

// ---------------------------------------------------------------- UI helpers

void CreateUiFont() {
    if (uiFont) DeleteObject(uiFont);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
}

void DrawDiskIcon(HDC dc, int size, COLORREF body) {
    HBRUSH bg = CreateSolidBrush(RGB(255, 0, 255));
    RECT all{0, 0, size, size};
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    int m = std::max(1, size / 8);
    HBRUSH b = CreateSolidBrush(body);
    HPEN p = CreatePen(PS_SOLID, 1, RGB(GetRValue(body) / 2, GetGValue(body) / 2, GetBValue(body) / 2));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, m, m, size - m, size - m, size / 4, size / 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
    RECT label{m + size / 5, m + 1, size - m - size / 5, m + size * 3 / 8};
    HBRUSH w = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &label, w);
    DeleteObject(w);
    RECT slot{size / 2 - size / 8, size - m - size / 3, size / 2 + size / 8, size - m - size / 10};
    HBRUSH s = CreateSolidBrush(RGB(240, 240, 240));
    FillRect(dc, &slot, s);
    DeleteObject(s);
}

void FillTabIcons() {
    int size = Scale(16);
    if (tabImages) ImageList_Destroy(tabImages);
    tabImages = ImageList_Create(size, size, ILC_COLOR24 | ILC_MASK, 3, 0);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    // Saved (blue), modified (red), read-only (grey): the Notepad++ tab icon convention.
    COLORREF colors[3] = {RGB(0x3A, 0x7B, 0xD5), RGB(0xE0, 0x3C, 0x31), RGB(0x9A, 0x9A, 0x9A)};
    for (COLORREF c : colors) {
        HBITMAP bmp = CreateCompatibleBitmap(screen, size, size);
        HGDIOBJ old = SelectObject(dc, bmp);
        DrawDiskIcon(dc, size, c);
        SelectObject(dc, old);
        ImageList_AddMasked(tabImages, bmp, RGB(255, 0, 255));
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    TabCtrl_SetImageList(hTab, tabImages);
}

void SetStatusParts() {
    RECT rc;
    GetClientRect(hStatus, &rc);
    int widths[6] = {Scale(110), Scale(230), Scale(290), Scale(120), Scale(110), Scale(56)};
    int right[6];
    int x = rc.right;
    for (int i = 5; i >= 1; i--) {
        right[i] = x;
        x -= widths[i];
    }
    right[0] = std::max(x, Scale(60));
    for (int i = 1; i < 6; i++) right[i] = std::max(right[i], right[i - 1] + Scale(20));
    right[5] = -1;
    SendMessageW(hStatus, SB_SETPARTS, 6, (LPARAM)right);
}

void ApplyDarkChrome() {
    BOOL dark = settings.dark;
    DwmSetWindowAttribute(hMain, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    SetWindowTheme(editor.hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (hResults) {
        SetWindowTheme(hResults, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(hResults, nullptr, TRUE);
    }
}

} // namespace

void App::ApplyEditorSettings() {
    Settings& s = settings;
    editor.tabWidth = s.tabWidth;
    editor.useSpaces = s.useSpaces;
    editor.fontName = s.fontName;
    editor.fontSize = s.fontSize;
    editor.dark = s.dark;
    editor.smartHighlight = s.smartHighlight;
    editor.autoIndent = s.autoIndent;
    editor.autoClose = s.autoClose;
    editor.autoComplete = s.autoComplete;
    editor.lineNumbers = s.lineNumbers;
    editor.bookmarkMargin = s.bookmarkMargin;
    editor.highlightCurrentLine = s.curLine;
    editor.wordWrap = s.wordWrap;
    editor.showSpaces = s.showSpaces;
    editor.showEol = s.showEol;
    editor.showIndentGuides = s.indentGuides;
    editor.ApplyOptions();
    ApplyDarkChrome();
}

void App::Relayout() {
    if (!hMain || !hTab) return;
    // IsWindowVisible() is false for every child until the main window is shown, so test the style bit.
    auto IsWindowVisible = [](HWND w) { return (GetWindowLongW(w, GWL_STYLE) & WS_VISIBLE) != 0; };
    RECT rc;
    GetClientRect(hMain, &rc);
    int top = 0, bottom = rc.bottom, w = rc.right;
    if (IsWindowVisible(hToolbar)) {
        SendMessageW(hToolbar, TB_AUTOSIZE, 0, 0);
        RECT tr;
        GetWindowRect(hToolbar, &tr);
        top += tr.bottom - tr.top;
    }
    if (IsWindowVisible(hStatus)) {
        SendMessageW(hStatus, WM_SIZE, 0, 0);
        SetStatusParts();
        RECT sr;
        GetWindowRect(hStatus, &sr);
        bottom -= sr.bottom - sr.top;
    }
    if (IsWindowVisible(hTab)) {
        RECT tr{0, 0, w, 200};
        TabCtrl_AdjustRect(hTab, FALSE, &tr);
        int tabH = tr.top + Scale(1);
        MoveWindow(hTab, 0, top, w, tabH, TRUE);
        top += tabH;
    }
    if (resultsVisible && hResults) {
        int h = settings.resultsHeight > 0 ? settings.resultsHeight : Scale(170);
        h = std::clamp(h, Scale(60), std::max(Scale(60), (bottom - top) - Scale(80)));
        MoveWindow(hResults, 0, bottom - h, w, h, TRUE);
        bottom -= h + Scale(5);
    }
    MoveWindow(editor.hwnd, 0, top, w, std::max(0, bottom - top), TRUE);
}

void App::ShowResultsPanel(bool show) {
    resultsVisible = show;
    ShowWindow(hResults, show ? SW_SHOW : SW_HIDE);
    Relayout();
    InvalidateRect(hMain, nullptr, TRUE);
}

namespace {

// ---------------------------------------------------------------- printing

void PrintDocument(Document* d, bool showDialog) {
    PRINTDLGW pd{sizeof(pd)};
    pd.hwndOwner = hMain;
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | (editor.HasSelection() ? 0 : PD_NOSELECTION);
    if (!showDialog) pd.Flags |= PD_RETURNDEFAULT;
    if (!PrintDlgW(&pd) || !pd.hDC) return;
    HDC dc = pd.hDC;
    int lineFrom = 0, lineTo = d->LineCount() - 1;
    if (pd.Flags & PD_SELECTION) {
        Pos a, b;
        editor.GetSel(a, b);
        lineFrom = a.line;
        lineTo = b.line;
    }
    int dpiY = GetDeviceCaps(dc, LOGPIXELSY), dpiX = GetDeviceCaps(dc, LOGPIXELSX);
    int pageW = GetDeviceCaps(dc, HORZRES), pageH = GetDeviceCaps(dc, VERTRES);
    int mx = dpiX * 6 / 10, my = dpiY * 6 / 10;
    HFONT fonts[2];
    for (int i = 0; i < 2; i++)
        fonts[i] = CreateFontW(-MulDiv(settings.fontSize, dpiY, 72), 0, 0, 0, i ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               FIXED_PITCH | FF_MODERN, settings.fontName.c_str());
    HFONT small = CreateFontW(-MulDiv(8, dpiY, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    SelectObject(dc, fonts[0]);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int lineH = tm.tmHeight, charW = std::max(1, (int)tm.tmAveCharWidth);
    int cols = std::max(10, (pageW - 2 * mx) / charW);
    int bodyTop = my + lineH * 2, bodyBottom = pageH - my - lineH * 2;
    std::wstring docName = d->path.empty() ? d->title : d->path;
    DOCINFOW di{sizeof(di)};
    di.lpszDocName = docName.c_str();
    if (StartDocW(dc, &di) > 0) {
        const Theme& th = GetTheme(false);
        int state = 0;
        std::vector<uint8_t> st;
        for (int l = 0; l < lineFrom; l++) state = LexLine(d->lang, d->lines[l], state, st);
        int page = 0, y = bodyBottom;
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t date[64];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &now, nullptr, date, 64, nullptr);
        auto startPage = [&]() {
            if (page) EndPage(dc);
            StartPage(dc);
            page++;
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, small);
            SetTextColor(dc, RGB(0, 0, 0));
            RECT hr{mx, my, pageW - mx, my + lineH};
            DrawTextW(dc, docName.c_str(), -1, &hr, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            DrawTextW(dc, date, -1, &hr, DT_RIGHT | DT_SINGLELINE);
            std::wstring pn = L"Page " + std::to_wstring(page);
            RECT fr{mx, pageH - my - lineH, pageW - mx, pageH - my};
            DrawTextW(dc, pn.c_str(), -1, &fr, DT_CENTER | DT_SINGLELINE);
            y = bodyTop;
        };
        for (int l = lineFrom; l <= lineTo; l++) {
            const std::wstring& src = d->lines[l];
            state = LexLine(d->lang, src, state, st);
            // Expand tabs, keeping a style per output cell.
            std::wstring text;
            std::vector<uint8_t> styles;
            for (size_t i = 0; i < src.size(); i++) {
                if (src[i] == L'\t') {
                    int n = settings.tabWidth - (int)text.size() % settings.tabWidth;
                    text.append(n, L' ');
                    styles.insert(styles.end(), n, ST_DEFAULT);
                } else {
                    text += src[i];
                    styles.push_back(i < st.size() ? st[i] : ST_DEFAULT);
                }
            }
            size_t pos = 0;
            do {
                if (y + lineH > bodyBottom) startPage();
                size_t n = std::min(text.size() - pos, (size_t)cols);
                size_t k = pos;
                while (k < pos + n) {
                    size_t j = k + 1;
                    while (j < pos + n && styles[j] == styles[k]) j++;
                    const StyleDef& sd = th.styles[styles[k] < ST_COUNT ? styles[k] : 0];
                    SelectObject(dc, fonts[sd.bold ? 1 : 0]);
                    SetTextColor(dc, sd.fg);
                    std::vector<INT> dx(j - k, charW);
                    ExtTextOutW(dc, mx + (int)(k - pos) * charW, y, 0, nullptr, text.c_str() + k, (UINT)(j - k),
                                dx.data());
                    k = j;
                }
                pos += n;
                y += lineH;
            } while (pos < text.size());
        }
        if (page) EndPage(dc);
        EndDoc(dc);
    }
    for (HFONT f : fonts) DeleteObject(f);
    DeleteObject(small);
    DeleteDC(dc);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
}

// ---------------------------------------------------------------- text transforms

wchar_t Up(wchar_t c) {
    CharUpperBuffW(&c, 1);
    return c;
}
wchar_t Lo(wchar_t c) {
    CharLowerBuffW(&c, 1);
    return c;
}

std::wstring MapCase(const std::wstring& s, DWORD flags) {
    if (s.empty()) return s;
    int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(),
                          nullptr, 0, nullptr, nullptr, 0);
    std::wstring out(n, L'\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(), &out[0], n,
                  nullptr, nullptr, 0);
    return out;
}

std::wstring ProperCase(const std::wstring& s, bool blend) {
    std::wstring r = s;
    bool start = true;
    for (auto& c : r) {
        if (IsCharAlphaW(c)) {
            c = start ? Up(c) : (blend ? c : Lo(c));
            start = false;
        } else {
            start = !IsCharAlphaNumericW(c) && c != L'\'';
        }
    }
    return r;
}

std::wstring SentenceCase(const std::wstring& s, bool blend) {
    std::wstring r = s;
    bool start = true;
    for (size_t i = 0; i < r.size(); i++) {
        wchar_t& c = r[i];
        if (IsCharAlphaW(c)) {
            c = start ? Up(c) : (blend ? c : Lo(c));
            start = false;
        } else if (c == L'.' || c == L'!' || c == L'?' || c == L'\n') {
            start = true;
        }
    }
    return r;
}

std::wstring InvertCase(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = IsCharUpperW(c) ? Lo(c) : (IsCharLowerW(c) ? Up(c) : c);
    return r;
}

std::wstring RandomCase(const std::wstring& s) {
    static std::mt19937 rng{std::random_device{}()};
    std::wstring r = s;
    for (auto& c : r) c = (rng() & 1) ? Up(c) : Lo(c);
    return r;
}

std::wstring TrimRight(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}
std::wstring TrimLeft(const std::wstring& s) {
    size_t i = s.find_first_not_of(L" \t");
    return i == std::wstring::npos ? L"" : s.substr(i);
}

std::wstring ExpandTabs(const std::wstring& s, int tab) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\t')
            out.append(tab - (int)out.size() % tab, L' ');
        else
            out += c;
    }
    return out;
}

std::wstring SpacesToTabs(const std::wstring& s, int tab, bool leadingOnly) {
    std::wstring out;
    int cell = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L' ' && s[i] != L'\t') {
            if (leadingOnly) {
                out += s.substr(i);
                return out;
            }
            out += s[i++];
            cell++;
            continue;
        }
        // A run of blanks: rewrite it with tabs at tab stops.
        int startCell = cell;
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) {
            cell += s[i] == L'\t' ? tab - cell % tab : 1;
            i++;
        }
        int c = startCell;
        if (cell - startCell == 1 && !leadingOnly) {
            out += L' ';
            continue;
        }
        while ((c / tab + 1) * tab <= cell) {
            out += L'\t';
            c = (c / tab + 1) * tab;
        }
        out.append(cell - c, L' ');
    }
    return out;
}

bool ParseInteger(const std::wstring& s, long long& v) {
    const wchar_t* p = s.c_str();
    while (*p == L' ' || *p == L'\t') p++;
    wchar_t* end;
    v = wcstoll(p, &end, 10);
    return end != p;
}

bool ParseDecimal(const std::wstring& s, double& v) {
    const wchar_t* p = s.c_str();
    while (*p == L' ' || *p == L'\t') p++;
    wchar_t* end;
    v = wcstod(p, &end);
    return end != p;
}

template <class Key, class Get>
void SortByKey(std::vector<std::wstring>& v, bool desc, Get get) {
    // Lines that do not parse keep their order and go after the sorted ones.
    std::vector<std::pair<Key, std::wstring>> ok;
    std::vector<std::wstring> rest;
    for (auto& l : v) {
        Key k;
        if (get(l, k))
            ok.emplace_back(k, l);
        else
            rest.push_back(l);
    }
    std::stable_sort(ok.begin(), ok.end(), [desc](auto& a, auto& b) { return desc ? b.first < a.first : a.first < b.first; });
    v.clear();
    for (auto& p : ok) v.push_back(p.second);
    v.insert(v.end(), rest.begin(), rest.end());
}

void ReplaceWholeDocument(Document* d, const std::vector<std::wstring>& lines, const std::vector<uint8_t>& marks) {
    std::wstring text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += L'\n';
        text += lines[i];
    }
    d->BeginUndoGroup(d->anchor, d->caret);
    d->Erase({0, 0}, d->EndPos());
    d->Insert({0, 0}, text);
    d->EndUndoGroup();
    for (size_t i = 0; i < marks.size() && i < d->bookmarks.size(); i++) d->bookmarks[i] = marks[i];
    d->caret = d->anchor = d->Clamp(d->caret);
    editor.DocumentChanged();
}

std::wstring UrlEncode(const std::wstring& s) {
    std::string u = EncodeText(s, Enc::UTF8, 0), out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return std::wstring(out.begin(), out.end());
}

void ShellOpen(const std::wstring& file, const std::wstring& params = L"", const std::wstring& dir = L"") {
    HINSTANCE r = ShellExecuteW(hMain, nullptr, file.c_str(), params.empty() ? nullptr : params.c_str(),
                                dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) Error(L"Cannot run \"" + file + L"\".");
}

std::wstring DateTime(bool longFormat) {
    wchar_t date[128], time[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, longFormat ? DATE_LONGDATE : DATE_SHORTDATE, nullptr, nullptr, date,
                    128, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, 64);
    return std::wstring(time) + L" " + date;
}

// ---------------------------------------------------------------- file monitoring

void CheckFiles() {
    if (checkingFiles) return;
    checkingFiles = true;
    for (size_t i = 0; i < docs.size(); i++) {
        Document* d = docs[i].get();
        if (d->path.empty() || !d->hasFileTime || d->monitoring) continue;
        FILETIME ft;
        if (!GetFileTime(d->path, ft)) {
            d->hasFileTime = false;
            Activate((int)i);
            if (Ask(L"\"" + d->path + L"\"\n\nThe file doesn't exist anymore.\nKeep this file in the editor?",
                    MB_YESNO | MB_ICONWARNING) == IDYES) {
                d->SetDirty();
                UpdateTab((int)i);
                UpdateTitle();
            } else {
                CloseDoc((int)i, false);
                i--;
            }
            continue;
        }
        if (CompareFileTime(&ft, &d->fileTime) != 0) {
            d->fileTime = ft;
            Activate((int)i);
            std::wstring msg = L"\"" + d->path + L"\"\n\nThis file has been modified by another program.\n";
            msg += d->IsModified() ? L"Do you want to reload it and lose the changes made in Tetradkata++?"
                                   : L"Do you want to reload it?";
            if (Ask(msg, MB_YESNO | MB_ICONQUESTION) == IDYES) ReloadDocument(d);
        }
    }
    checkingFiles = false;
}

void MonitorTick() {
    for (auto& up : docs) {
        Document* d = up.get();
        if (!d->monitoring || d->path.empty()) continue;
        FILETIME ft;
        if (!GetFileTime(d->path, ft) || CompareFileTime(&ft, &d->fileTime) == 0) continue;
        ReloadDocument(d);
        d->caret = d->anchor = d->EndPos();
        if (d == Cur()) editor.SetSel(d->EndPos(), d->EndPos());
    }
}

// ---------------------------------------------------------------- view modes

void ShowChrome(bool menu, bool toolbar, bool tabs, bool status) {
    SetMenu(hMain, menu ? LoadMenuW(hInst, MAKEINTRESOURCEW(IDR_MAINMENU)) : nullptr);
    if (menu) BuildLanguageMenu(); // the menu was reloaded: rebuild the dynamic Language entries
    ShowWindow(hToolbar, toolbar ? SW_SHOW : SW_HIDE);
    ShowWindow(hTab, tabs ? SW_SHOW : SW_HIDE);
    ShowWindow(hStatus, status ? SW_SHOW : SW_HIDE);
    Relayout();
}

void ToggleFullScreen() {
    if (postIt) return;
    if (!fullScreen) {
        savedPlacement.length = sizeof(savedPlacement);
        GetWindowPlacement(hMain, &savedPlacement);
        savedStyle = GetWindowLongW(hMain, GWL_STYLE);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hMain, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hMain, GWL_STYLE, savedStyle & ~(WS_CAPTION | WS_THICKFRAME));
        fullScreen = true;
        ShowChrome(false, false, true, false);
        SetWindowPos(hMain, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullScreen = false;
        SetWindowLongW(hMain, GWL_STYLE, savedStyle);
        ShowChrome(true, true, true, true);
        SetWindowPlacement(hMain, &savedPlacement);
        SetWindowPos(hMain, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
}

void TogglePostIt() {
    if (fullScreen) return;
    if (!postIt) {
        savedPlacement.length = sizeof(savedPlacement);
        GetWindowPlacement(hMain, &savedPlacement);
        savedStyle = GetWindowLongW(hMain, GWL_STYLE);
        SetWindowLongW(hMain, GWL_STYLE, savedStyle & ~WS_CAPTION);
        postIt = true;
        ShowChrome(false, false, false, false);
    } else {
        postIt = false;
        SetWindowLongW(hMain, GWL_STYLE, savedStyle);
        ShowChrome(true, true, true, true);
    }
    SetWindowPos(hMain, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

// ---------------------------------------------------------------- menus

void UpdateMenus(HMENU m) {
    Document* d = Cur();
    if (!d) return;
    auto check = [m](UINT id, bool on) { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto enable = [m](UINT id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    check(IDM_VIEW_WRAP, editor.wordWrap);
    check(IDM_VIEW_TAB_SPACE, editor.showSpaces && !editor.showEol);
    check(IDM_VIEW_EOL, editor.showEol && !editor.showSpaces);
    check(IDM_VIEW_ALL_CHARS, editor.showEol && editor.showSpaces);
    check(IDM_VIEW_INDENT_GUIDE, editor.showIndentGuides);
    check(IDM_VIEW_ALWAYSONTOP, settings.alwaysOnTop);
    check(IDM_VIEW_FULLSCREEN, fullScreen);
    check(IDM_VIEW_POSTIT, postIt);
    check(IDM_VIEW_DARKMODE, settings.dark);
    check(IDM_VIEW_MONITORING, d->monitoring);
    check(IDM_SEARCH_RESULTS_WINDOW, resultsVisible);
    check(IDM_EDIT_READONLY, d->readOnly);
    check(IDM_EDIT_SETREADONLY_ATTR, d->fileReadOnly);
    enable(IDM_EDIT_SETREADONLY_ATTR, !d->path.empty());
    enable(IDM_VIEW_MONITORING, !d->path.empty());
    enable(IDM_EDIT_UNDO, d->CanUndo());
    enable(IDM_EDIT_REDO, d->CanRedo());
    bool sel = editor.HasSelection();
    enable(IDM_EDIT_CUT, editor.Editable());
    enable(IDM_EDIT_PASTE, editor.Editable() && IsClipboardFormatAvailable(CF_UNICODETEXT));
    enable(IDM_EDIT_OPEN_SELECTION, sel);
    enable(IDM_EDIT_OPEN_SEL_FOLDER, sel);
    enable(IDM_EDIT_SEARCH_INTERNET, sel);
    enable(IDM_EDIT_REDACT, sel);
    enable(IDM_FILE_RELOAD, !d->path.empty());
    enable(IDM_FILE_DELETE, !d->path.empty());
    enable(IDM_FILE_OPEN_FOLDER, !d->path.empty());
    enable(IDM_FILE_OPEN_CMD, !d->path.empty());
    enable(IDM_FILE_OPEN_POWERSHELL, !d->path.empty());
    enable(IDM_FILE_OPEN_DEFAULT_VIEWER, !d->path.empty());
    enable(IDM_MACRO_START, !recordingMacro);
    enable(IDM_MACRO_STOP, recordingMacro);
    enable(IDM_MACRO_PLAYBACK, !recordingMacro && !macro.empty());
    enable(IDM_MACRO_RUN_MULTI, !recordingMacro && !macro.empty());
    check(IDM_FORMAT_TODOS, d->eol == Eol::CRLF);
    check(IDM_FORMAT_TOUNIX, d->eol == Eol::LF);
    check(IDM_FORMAT_TOMAC, d->eol == Eol::CR);
    check(IDM_FORMAT_ANSI, d->enc == Enc::ANSI);
    check(IDM_FORMAT_UTF8, d->enc == Enc::UTF8);
    check(IDM_FORMAT_UTF8BOM, d->enc == Enc::UTF8BOM);
    check(IDM_FORMAT_UTF16BE, d->enc == Enc::UTF16BE);
    check(IDM_FORMAT_UTF16LE, d->enc == Enc::UTF16LE);
    static const std::pair<UINT, UINT> cps[] = {
        {IDM_FORMAT_WIN1250, 1250}, {IDM_FORMAT_WIN1251, 1251}, {IDM_FORMAT_WIN1252, 1252}, {IDM_FORMAT_WIN1253, 1253},
        {IDM_FORMAT_WIN1254, 1254}, {IDM_FORMAT_ISO8859_1, 28591}, {IDM_FORMAT_ISO8859_2, 28592},
        {IDM_FORMAT_ISO8859_5, 28595}, {IDM_FORMAT_ISO8859_7, 28597}, {IDM_FORMAT_OEM437, 437},
        {IDM_FORMAT_OEM850, 850}, {IDM_FORMAT_OEM855, 855}, {IDM_FORMAT_OEM866, 866}, {IDM_FORMAT_KOI8U, 21866},
        {IDM_FORMAT_KOI8R, 20866}};
    for (auto& [id, cp] : cps) check(id, d->enc == Enc::CODEPAGE && d->codepage == cp);
    for (int i = 0; i < LanguageCount(); i++) check(GetLanguage(i).menuId, d->lang == i);
}

void RebuildRecentMenu(HMENU file) {
    for (int i = GetMenuItemCount(file) - 1; i >= 0; i--) {
        MENUITEMINFOW mii{sizeof(mii), MIIM_ID};
        GetMenuItemInfoW(file, i, TRUE, &mii);
        if ((mii.wID >= IDM_RECENT_BASE && mii.wID < IDM_RECENT_BASE + 32) || mii.wID == IDM_RECENT_OPENALL ||
            mii.wID == IDM_RECENT_CLEAR || mii.wID == IDM_FILE_RESTORELASTCLOSED)
            DeleteMenu(file, i, MF_BYPOSITION);
    }
    int exitPos = -1;
    for (int i = 0; i < GetMenuItemCount(file); i++)
        if (GetMenuItemID(file, i) == IDM_FILE_EXIT) exitPos = i;
    if (exitPos < 0) return;
    int pos = exitPos;
    auto add = [&](UINT id, const std::wstring& text, UINT flags = MF_STRING) {
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_ID | MIIM_FTYPE | (flags == MF_SEPARATOR ? 0 : MIIM_STRING);
        mii.fType = flags == MF_SEPARATOR ? MFT_SEPARATOR : MFT_STRING;
        mii.wID = id;
        mii.dwTypeData = (LPWSTR)text.c_str();
        InsertMenuItemW(file, pos++, TRUE, &mii);
    };
    add(IDM_FILE_RESTORELASTCLOSED, L"Restore Recent Closed File\tCtrl+Shift+T");
    EnableMenuItem(file, IDM_FILE_RESTORELASTCLOSED, MF_BYCOMMAND | (closedFiles.empty() ? MF_GRAYED : MF_ENABLED));
    if (!settings.recent.empty()) {
        add(IDM_RECENT_OPENALL, L"Open All Recent Files");
        add(IDM_RECENT_CLEAR, L"Empty Recent Files List");
        add(IDM_RECENT_BASE + 30, L"", MF_SEPARATOR);
        for (size_t i = 0; i < settings.recent.size() && i < 15; i++) {
            std::wstring label = (i < 9 ? L"&" : L"") + std::to_wstring(i + 1) + L": " + settings.recent[i];
            std::wstring esc;
            for (wchar_t c : label) {
                if (c == L'&' && &c != &label[0]) esc += L'&';
                esc += c;
            }
            add(IDM_RECENT_BASE + (UINT)i, i < 9 ? esc : label);
        }
    }
    add(IDM_RECENT_BASE + 31, L"", MF_SEPARATOR);
}

void ShowEditorContextMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    bool sel = editor.HasSelection();
    AppendMenuW(m, MF_STRING, IDM_EDIT_CUT, L"Cut");
    AppendMenuW(m, MF_STRING, IDM_EDIT_COPY, L"Copy");
    AppendMenuW(m, MF_STRING, IDM_EDIT_PASTE, L"Paste");
    AppendMenuW(m, MF_STRING, IDM_EDIT_DELETE, L"Delete");
    AppendMenuW(m, MF_STRING, IDM_EDIT_SELECTALL, L"Select All");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_EDIT_UPPERCASE, L"UPPERCASE");
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_EDIT_LOWERCASE, L"lowercase");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_EDIT_OPEN_SELECTION, L"Open File");
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_EDIT_SEARCH_INTERNET, L"Search on Internet");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EDIT_TOGGLE_COMMENT, L"Toggle Single Line Comment");
    AppendMenuW(m, MF_STRING, IDM_EDIT_BLOCK_COMMENT, L"Block Comment");
    AppendMenuW(m, MF_STRING, IDM_EDIT_BLOCK_UNCOMMENT, L"Block Uncomment");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SEARCH_TOGGLE_BOOKMARK, L"Toggle Bookmark");
    UpdateMenus(m);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hMain, nullptr);
    DestroyMenu(m);
}

void ShowTabContextMenu(int index, int x, int y) {
    if (index != current) Activate(index);
    HMENU m = CreatePopupMenu();
    Document* d = Cur();
    UINT file = d->path.empty() ? MF_GRAYED : 0;
    AppendMenuW(m, MF_STRING, IDM_FILE_CLOSE, L"Close");
    AppendMenuW(m, MF_STRING, IDM_FILE_CLOSEALL_BUT_CURRENT, L"Close All BUT This");
    AppendMenuW(m, MF_STRING, IDM_FILE_CLOSEALL_TOLEFT, L"Close All to the Left");
    AppendMenuW(m, MF_STRING, IDM_FILE_CLOSEALL_TORIGHT, L"Close All to the Right");
    AppendMenuW(m, MF_STRING, IDM_FILE_CLOSEALL_UNCHANGED, L"Close All Unchanged");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_FILE_SAVE, L"Save");
    AppendMenuW(m, MF_STRING, IDM_FILE_SAVEAS, L"Save As...");
    AppendMenuW(m, MF_STRING, IDM_FILE_RENAME, L"Rename...");
    AppendMenuW(m, MF_STRING | file, IDM_FILE_DELETE, L"Move to Recycle Bin");
    AppendMenuW(m, MF_STRING | file, IDM_FILE_RELOAD, L"Reload");
    AppendMenuW(m, MF_STRING, IDM_FILE_PRINT, L"Print...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | file, IDM_FILE_OPEN_FOLDER, L"Open Containing Folder in Explorer");
    AppendMenuW(m, MF_STRING | file, IDM_FILE_OPEN_CMD, L"Open Containing Folder in cmd");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EDIT_READONLY, L"Read-Only");
    AppendMenuW(m, MF_STRING | file, IDM_EDIT_SETREADONLY_ATTR, L"Read-Only Attribute in Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | file, IDM_EDIT_FULLPATH_TO_CLIP, L"Copy Full File Path");
    AppendMenuW(m, MF_STRING, IDM_EDIT_FILENAME_TO_CLIP, L"Copy Filename");
    AppendMenuW(m, MF_STRING | file, IDM_EDIT_DIRPATH_TO_CLIP, L"Copy Current Dir. Path");
    UpdateMenus(m);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hMain, nullptr);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- macro

bool IsRecordable(int id) {
    if (id == IDM_MACRO_START || id == IDM_MACRO_STOP || id == IDM_MACRO_PLAYBACK || id == IDM_MACRO_RUN_MULTI)
        return false;
    return (id >= 42000 && id < 43000) || id == IDM_SEARCH_FINDNEXT || id == IDM_SEARCH_FINDPREV ||
           id == IDM_SEARCH_SELECT_FIND_NEXT || id == IDM_SEARCH_SELECT_FIND_PREV || id == IDM_SEARCH_GOTO_BRACE ||
           id == IDM_SEARCH_TOGGLE_BOOKMARK || id == IDM_SEARCH_NEXT_BOOKMARK || id == IDM_SEARCH_PREV_BOOKMARK;
}

void PlayMacro(int times, bool untilEof) {
    if (macro.empty()) return;
    Document* d = Cur();
    d->BeginUndoGroup(d->anchor, d->caret);
    for (int n = 0; untilEof || n < times; n++) {
        Pos before = editor.Caret();
        for (const MacroStep& s : macro) {
            if (s.kind == MacroKind::Command)
                ExecuteCommand((int)s.value);
            else
                editor.ExecuteMacroStep(s);
        }
        if (untilEof) {
            Pos after = editor.Caret();
            if (after == before || after == d->EndPos() || n > 100000) break;
        }
    }
    d->EndUndoGroup();
}

// ---------------------------------------------------------------- open/save dialogs

std::vector<std::wstring> AskOpenPaths() {
    std::vector<std::wstring> out;
    std::wstring buf(65536, L'\0');
    Document* d = Cur();
    std::wstring dir = d && !d->path.empty() ? DirOf(d->path) : L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hMain;
    ofn.lpstrFilter = L"All types (*.*)\0*.*\0Normal text file (*.txt)\0*.txt\0";
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return out;
    const wchar_t* p = buf.c_str();
    std::wstring first = p;
    p += first.size() + 1;
    if (!*p) {
        out.push_back(first);
        return out;
    }
    while (*p) {
        std::wstring name = p;
        out.push_back(first + L"\\" + name);
        p += name.size() + 1;
    }
    return out;
}

void SaveSessionAs() {
    std::wstring buf(32768, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hMain;
    ofn.lpstrFilter = L"Session file (*.session)\0*.session\0All types (*.*)\0*.*\0";
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrDefExt = L"session";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) WriteSession(buf.c_str(), false);
}

void LoadSessionFrom() {
    std::wstring buf(32768, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hMain;
    ofn.lpstrFilter = L"Session file (*.session)\0*.session\0All types (*.*)\0*.*\0";
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    int a = ReadSession(buf.c_str(), false);
    if (a >= 0) Activate(a);
}

void CloseRange(std::vector<int> indices) {
    // Close from the back so earlier indices stay valid.
    std::sort(indices.rbegin(), indices.rend());
    for (int i : indices)
        if (!CloseDoc(i)) break;
}

void SortTabs(bool byPath, bool desc) {
    Document* cur = Cur();
    std::stable_sort(docs.begin(), docs.end(), [&](const std::unique_ptr<Document>& a, const std::unique_ptr<Document>& b) {
        std::wstring ka = byPath ? (a->path.empty() ? a->title : a->path) : a->DisplayName();
        std::wstring kb = byPath ? (b->path.empty() ? b->title : b->path) : b->DisplayName();
        int c = _wcsicmp(ka.c_str(), kb.c_str());
        return desc ? c > 0 : c < 0;
    });
    TabCtrl_DeleteAllItems(hTab);
    for (size_t i = 0; i < docs.size(); i++) InsertTab((int)i);
    current = -1;
    Activate(IndexOf(cur));
}

void MoveTab(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= (int)docs.size() || to >= (int)docs.size()) return;
    Document* cur = Cur();
    auto d = std::move(docs[from]);
    docs.erase(docs.begin() + from);
    docs.insert(docs.begin() + to, std::move(d));
    for (int i = std::min(from, to); i <= std::max(from, to); i++) UpdateTab(i);
    current = IndexOf(cur);
    TabCtrl_SetCurSel(hTab, current);
}

void ShowSummary() {
    Document* d = Cur();
    size_t chars = 0, words = 0;
    for (auto& l : d->lines) {
        chars += l.size();
        bool in = false;
        for (wchar_t c : l) {
            bool w = !iswspace(c);
            if (w && !in) words++;
            in = w;
        }
    }
    std::wstring s;
    if (!d->path.empty()) {
        s += L"Full file path: " + d->path + L"\n";
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(d->path.c_str(), GetFileExInfoStandard, &fad)) {
            auto fmt = [](FILETIME ft) {
                FILETIME lt;
                SYSTEMTIME st;
                FileTimeToLocalFileTime(&ft, &lt);
                FileTimeToSystemTime(&lt, &st);
                wchar_t d[64], t[64];
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64, nullptr);
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, t, 64);
                return std::wstring(d) + L" " + t;
            };
            s += L"Created: " + fmt(fad.ftCreationTime) + L"\nModified: " + fmt(fad.ftLastWriteTime) + L"\n";
        }
        s += L"\n";
    }
    s += L"Characters (without line endings): " + Num(chars) + L"\n";
    s += L"Words: " + Num(words) + L"\n";
    s += L"Lines: " + Num(d->LineCount()) + L"\n";
    s += L"Document length: " + Num(d->TextLength()) + L"\n";
    std::string bytes = EncodeText(JoinLines(*d), d->enc, d->codepage);
    s += L"Size on save (" + EncodingName(d->enc, d->codepage) + L"): " + Num(bytes.size()) + L" bytes\n";
    if (editor.HasSelection()) {
        Pos a, b;
        editor.GetSel(a, b);
        s += L"Selected: " + Num(d->GetText(a, b).size()) + L" characters in " + Num(b.line - a.line + 1) +
             L" lines\n";
    }
    MessageBoxW(hMain, s.c_str(), L"Summary", MB_OK | MB_ICONINFORMATION);
}

void SetEncoding(Enc enc, UINT cp, bool convert) {
    Document* d = Cur();
    if (!convert && !d->path.empty() && !d->IsModified() && FileExists(d->path)) {
        ReloadDocument(d, (int)enc, cp); // reinterpret the file bytes
        return;
    }
    d->enc = enc;
    d->codepage = cp;
    if (convert) d->SetDirty();
    UpdateTab(current);
    UpdateTitle();
    UpdateStatus();
}

void HashSelectionToClipboard(int algo) {
    if (!editor.HasSelection()) {
        MessageBeep(MB_OK);
        return;
    }
    SetClipboardText(HashString(algo, editor.GetSelText()));
    FlashStatus(L"Digest copied to clipboard");
}

} // namespace

void BuildLanguageMenu() {
    HMENU bar = GetMenu(hMain);
    if (!bar) return;
    HMENU lang = GetSubMenu(bar, 5);
    while (GetMenuItemCount(lang) > 0) DeleteMenu(lang, 0, MF_BYPOSITION);
    AppendMenuW(lang, MF_STRING, GetLanguage(0).menuId, GetLanguage(0).name);
    AppendMenuW(lang, MF_SEPARATOR, 0, nullptr);
    for (int i = 1; i < LanguageCount(); i++) AppendMenuW(lang, MF_STRING, GetLanguage(i).menuId, GetLanguage(i).name);
}

void App::ExecuteCommand(int id) {
    Document* d = Cur();
    if (!d) return;
    if (recordingMacro && IsRecordable(id)) macro.push_back({MacroKind::Command, (UINT)id, 0});
    Editor& ed = editor;

    if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + 30) {
        size_t i = id - IDM_RECENT_BASE;
        if (i < settings.recent.size()) {
            std::wstring p = settings.recent[i];
            if (FileExists(p))
                OpenFile(p);
            else if (Ask(L"\"" + p + L"\" doesn't exist anymore.\nRemove it from the recent files list?",
                         MB_YESNO | MB_ICONQUESTION) == IDYES)
                RemoveRecent(p);
        }
        return;
    }
    if (id >= IDM_VIEW_TAB1 && id <= IDM_VIEW_TAB1 + 8) {
        Activate(std::min(id - IDM_VIEW_TAB1, (int)docs.size() - 1));
        return;
    }
    if (int lang = LanguageByMenuId(id); lang >= 0) {
        d->lang = lang;
        d->lexValid = 1;
        ed.Invalidate();
        UpdateStatus();
        return;
    }

    switch (id) {
    // File
    case IDM_FILE_NEW: NewDocument(); break;
    case IDM_FILE_OPEN:
        for (auto& p : AskOpenPaths()) OpenFile(p);
        break;
    case IDM_FILE_OPEN_FOLDER:
        if (!d->path.empty()) ShellOpen(L"explorer.exe", L"/select,\"" + d->path + L"\"");
        break;
    case IDM_FILE_OPEN_CMD:
        if (!d->path.empty()) ShellOpen(L"cmd.exe", L"", DirOf(d->path));
        break;
    case IDM_FILE_OPEN_POWERSHELL:
        if (!d->path.empty()) ShellOpen(L"powershell.exe", L"", DirOf(d->path));
        break;
    case IDM_FILE_OPEN_DEFAULT_VIEWER:
        if (!d->path.empty()) ShellOpen(d->path);
        break;
    case IDM_FILE_RELOAD:
        if (d->path.empty()) break;
        if (d->IsModified() && Ask(L"Are you sure you want to reload the current file and lose the changes made in "
                                   L"Tetradkata++?",
                                   MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
        ReloadDocument(d);
        break;
    case IDM_FILE_SAVE: SaveDoc(current, false); break;
    case IDM_FILE_SAVEAS: SaveDoc(current, true); break;
    case IDM_FILE_SAVECOPYAS: {
        std::wstring p = AskSavePath(d, L"Save a Copy As");
        std::wstring err;
        if (!p.empty() && !SaveDocument(p, *d, err, false)) Error(L"Cannot save file \"" + p + L"\":\n" + err);
        break;
    }
    case IDM_FILE_SAVEALL: {
        int keep = current;
        for (size_t i = 0; i < docs.size(); i++)
            if (docs[i]->IsModified() && !SaveDoc((int)i, false)) break;
        Activate(std::min(keep, (int)docs.size() - 1));
        break;
    }
    case IDM_FILE_RENAME: {
        if (d->path.empty()) {
            SaveDoc(current, true);
            break;
        }
        std::wstring p = AskSavePath(d, L"Rename");
        if (p.empty() || _wcsicmp(p.c_str(), d->path.c_str()) == 0) break;
        if (!MoveFileExW(d->path.c_str(), p.c_str(), MOVEFILE_COPY_ALLOWED)) {
            Error(L"Cannot rename \"" + d->path + L"\".");
            break;
        }
        d->path = p;
        d->hasFileTime = GetFileTime(p, d->fileTime);
        UpdateTab(current);
        UpdateTitle();
        break;
    }
    case IDM_FILE_CLOSE: CloseDoc(current); break;
    case IDM_FILE_CLOSEALL: {
        std::vector<int> all;
        for (size_t i = 0; i < docs.size(); i++) all.push_back((int)i);
        CloseRange(all);
        break;
    }
    case IDM_FILE_CLOSEALL_BUT_CURRENT:
    case IDM_FILE_CLOSEALL_TOLEFT:
    case IDM_FILE_CLOSEALL_TORIGHT:
    case IDM_FILE_CLOSEALL_UNCHANGED: {
        std::vector<int> v;
        for (int i = 0; i < (int)docs.size(); i++) {
            bool take = (id == IDM_FILE_CLOSEALL_BUT_CURRENT && i != current) ||
                        (id == IDM_FILE_CLOSEALL_TOLEFT && i < current) ||
                        (id == IDM_FILE_CLOSEALL_TORIGHT && i > current) ||
                        (id == IDM_FILE_CLOSEALL_UNCHANGED && !docs[i]->IsModified());
            if (take) v.push_back(i);
        }
        CloseRange(v);
        break;
    }
    case IDM_FILE_DELETE: {
        if (d->path.empty()) break;
        if (Ask(L"Are you sure you want to move \"" + d->path + L"\" to the Recycle Bin?",
                MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
        std::wstring from = d->path + L'\0';
        SHFILEOPSTRUCTW op{};
        op.hwnd = hMain;
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
            std::wstring p = d->path;
            CloseDoc(current, false);
            RemoveRecent(p);
        }
        break;
    }
    case IDM_FILE_LOADSESSION: LoadSessionFrom(); break;
    case IDM_FILE_SAVESESSION: SaveSessionAs(); break;
    case IDM_FILE_PRINT: PrintDocument(d, true); break;
    case IDM_FILE_PRINTNOW: PrintDocument(d, false); break;
    case IDM_FILE_EXIT: PostMessageW(hMain, WM_CLOSE, 0, 0); break;
    case IDM_FILE_RESTORELASTCLOSED:
        while (!closedFiles.empty()) {
            std::wstring p = closedFiles.back();
            closedFiles.pop_back();
            if (FileExists(p)) {
                OpenFile(p);
                break;
            }
        }
        break;
    case IDM_RECENT_OPENALL: {
        auto list = settings.recent;
        for (auto& p : list)
            if (FileExists(p)) OpenFile(p);
        break;
    }
    case IDM_RECENT_CLEAR: settings.recent.clear(); break;

    // Edit
    case IDM_EDIT_UNDO: ed.Undo(); break;
    case IDM_EDIT_REDO: ed.Redo(); break;
    case IDM_EDIT_CUT: ed.Cut(); break;
    case IDM_EDIT_COPY: ed.Copy(); break;
    case IDM_EDIT_PASTE: ed.Paste(); break;
    case IDM_EDIT_DELETE: ed.Clear(); break;
    case IDM_EDIT_SELECTALL: ed.SelectAll(); break;
    case IDM_EDIT_BEGINEND_SELECT:
        if (!beginSelectActive) {
            beginSelectPos = ed.Caret();
            beginSelectActive = true;
            FlashStatus(L"Begin select: move the caret and choose Begin/End Select again");
        } else {
            ed.SetSel(beginSelectPos, ed.Caret());
            beginSelectActive = false;
        }
        break;
    case IDM_EDIT_DATETIME_SHORT: ed.ReplaceSel(DateTime(false)); break;
    case IDM_EDIT_DATETIME_LONG: ed.ReplaceSel(DateTime(true)); break;
    case IDM_EDIT_FULLPATH_TO_CLIP: SetClipboardText(d->path.empty() ? d->title : d->path); break;
    case IDM_EDIT_FILENAME_TO_CLIP: SetClipboardText(d->DisplayName()); break;
    case IDM_EDIT_DIRPATH_TO_CLIP: SetClipboardText(DirOf(d->path)); break;
    case IDM_EDIT_ALL_NAMES_TO_CLIP:
    case IDM_EDIT_ALL_PATHS_TO_CLIP: {
        std::wstring t;
        for (auto& x : docs)
            t += (id == IDM_EDIT_ALL_NAMES_TO_CLIP ? x->DisplayName() : (x->path.empty() ? x->title : x->path)) + L"\r\n";
        SetClipboardText(t);
        break;
    }
    case IDM_EDIT_INS_TAB: ed.Indent(false); break;
    case IDM_EDIT_RMV_TAB: ed.Indent(true); break;
    case IDM_EDIT_UPPERCASE: ed.TransformSelection([](const std::wstring& s) { return MapCase(s, LCMAP_UPPERCASE); }); break;
    case IDM_EDIT_LOWERCASE: ed.TransformSelection([](const std::wstring& s) { return MapCase(s, LCMAP_LOWERCASE); }); break;
    case IDM_EDIT_PROPERCASE: ed.TransformSelection([](const std::wstring& s) { return ProperCase(s, false); }); break;
    case IDM_EDIT_PROPERCASE_BLEND: ed.TransformSelection([](const std::wstring& s) { return ProperCase(s, true); }); break;
    case IDM_EDIT_SENTENCECASE: ed.TransformSelection([](const std::wstring& s) { return SentenceCase(s, false); }); break;
    case IDM_EDIT_SENTENCECASE_BLEND: ed.TransformSelection([](const std::wstring& s) { return SentenceCase(s, true); }); break;
    case IDM_EDIT_INVERTCASE: ed.TransformSelection(InvertCase); break;
    case IDM_EDIT_RANDOMCASE: ed.TransformSelection(RandomCase); break;
    case IDM_EDIT_DUP_LINE: ed.DuplicateLine(); break;
    case IDM_EDIT_JOIN_LINES: ed.JoinLines(); break;
    case IDM_EDIT_LINE_UP: ed.MoveLines(-1); break;
    case IDM_EDIT_LINE_DOWN: ed.MoveLines(1); break;
    case IDM_EDIT_DELETE_LINE: ed.DeleteLine(); break;
    case IDM_EDIT_REMOVE_DUP:
        ed.TransformLines([](std::vector<std::wstring>& v) {
            std::unordered_set<std::wstring> seen;
            std::vector<std::wstring> out;
            for (auto& l : v)
                if (seen.insert(l).second) out.push_back(l);
            v = out;
        }, true);
        break;
    case IDM_EDIT_REMOVE_CONSEC_DUP:
        ed.TransformLines([](std::vector<std::wstring>& v) { v.erase(std::unique(v.begin(), v.end()), v.end()); }, true);
        break;
    case IDM_EDIT_REMOVE_EMPTY:
        ed.TransformLines([](std::vector<std::wstring>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [](const std::wstring& l) { return l.empty(); }), v.end());
        }, true);
        break;
    case IDM_EDIT_REMOVE_EMPTY_BLANK:
        ed.TransformLines([](std::vector<std::wstring>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [](const std::wstring& l) { return TrimLeft(l).empty(); }), v.end());
        }, true);
        break;
    case IDM_EDIT_BLANK_ABOVE: {
        Pos c = ed.Caret();
        ed.ReplaceRange({c.line, 0}, {c.line, 0}, L"\n");
        ed.SetSel({c.line, 0}, {c.line, 0});
        break;
    }
    case IDM_EDIT_BLANK_BELOW: {
        Pos c = ed.Caret();
        Pos e{c.line, (int)d->lines[c.line].size()};
        ed.ReplaceRange(e, e, L"\n");
        break;
    }
    case IDM_EDIT_REVERSE_LINES:
        ed.TransformLines([](std::vector<std::wstring>& v) { std::reverse(v.begin(), v.end()); }, true);
        break;
    case IDM_EDIT_RANDOMIZE_LINES:
        ed.TransformLines([](std::vector<std::wstring>& v) {
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(v.begin(), v.end(), rng);
        }, true);
        break;
    case IDM_EDIT_SORT_LEX_ASC:
    case IDM_EDIT_SORT_LEX_DESC: {
        bool desc = id == IDM_EDIT_SORT_LEX_DESC;
        ed.TransformLines([desc](std::vector<std::wstring>& v) {
            std::stable_sort(v.begin(), v.end(), [desc](auto& a, auto& b) { return desc ? b < a : a < b; });
        }, true);
        break;
    }
    case IDM_EDIT_SORT_LEXI_ASC:
    case IDM_EDIT_SORT_LEXI_DESC: {
        bool desc = id == IDM_EDIT_SORT_LEXI_DESC;
        ed.TransformLines([desc](std::vector<std::wstring>& v) {
            std::stable_sort(v.begin(), v.end(), [desc](auto& a, auto& b) {
                int c = _wcsicmp(a.c_str(), b.c_str());
                return desc ? c > 0 : c < 0;
            });
        }, true);
        break;
    }
    case IDM_EDIT_SORT_INT_ASC:
    case IDM_EDIT_SORT_INT_DESC: {
        bool desc = id == IDM_EDIT_SORT_INT_DESC;
        ed.TransformLines([desc](std::vector<std::wstring>& v) { SortByKey<long long>(v, desc, ParseInteger); }, true);
        break;
    }
    case IDM_EDIT_SORT_DEC_ASC:
    case IDM_EDIT_SORT_DEC_DESC: {
        bool desc = id == IDM_EDIT_SORT_DEC_DESC;
        ed.TransformLines([desc](std::vector<std::wstring>& v) { SortByKey<double>(v, desc, ParseDecimal); }, true);
        break;
    }
    case IDM_EDIT_SORT_LEN_ASC:
    case IDM_EDIT_SORT_LEN_DESC: {
        bool desc = id == IDM_EDIT_SORT_LEN_DESC;
        ed.TransformLines([desc](std::vector<std::wstring>& v) {
            std::stable_sort(v.begin(), v.end(), [desc](auto& a, auto& b) { return desc ? b.size() < a.size() : a.size() < b.size(); });
        }, true);
        break;
    }
    case IDM_EDIT_TOGGLE_COMMENT: ed.ToggleComment(0); break;
    case IDM_EDIT_STREAM_COMMENT_SINGLE: ed.ToggleComment(1); break;
    case IDM_EDIT_STREAM_UNCOMMENT_SINGLE: ed.ToggleComment(2); break;
    case IDM_EDIT_BLOCK_COMMENT: ed.BlockComment(false); break;
    case IDM_EDIT_BLOCK_UNCOMMENT: ed.BlockComment(true); break;
    case IDM_EDIT_WORD_COMPLETE: ed.ShowCompletion(false); break;
    case IDM_FORMAT_TODOS:
    case IDM_FORMAT_TOUNIX:
    case IDM_FORMAT_TOMAC: {
        Eol e = id == IDM_FORMAT_TODOS ? Eol::CRLF : id == IDM_FORMAT_TOUNIX ? Eol::LF : Eol::CR;
        if (e != d->eol) {
            d->eol = e;
            d->SetDirty();
            UpdateTab(current);
            UpdateTitle();
            ed.Invalidate();
            UpdateStatus();
        }
        break;
    }
    case IDM_EDIT_TRIM_TRAILING:
        ed.TransformLines([](std::vector<std::wstring>& v) { for (auto& l : v) l = TrimRight(l); }, true);
        break;
    case IDM_EDIT_TRIM_LEADING:
        ed.TransformLines([](std::vector<std::wstring>& v) { for (auto& l : v) l = TrimLeft(l); }, true);
        break;
    case IDM_EDIT_TRIM_BOTH:
        ed.TransformLines([](std::vector<std::wstring>& v) { for (auto& l : v) l = TrimRight(TrimLeft(l)); }, true);
        break;
    case IDM_EDIT_EOL_TO_SPACE:
    case IDM_EDIT_TRIM_ALL: {
        bool trim = id == IDM_EDIT_TRIM_ALL;
        ed.TransformLines([trim](std::vector<std::wstring>& v) {
            std::wstring out;
            for (size_t i = 0; i < v.size(); i++) {
                std::wstring l = trim ? TrimRight(TrimLeft(v[i])) : v[i];
                if (trim && l.empty()) continue;
                if (!out.empty() || (i && !trim)) out += L' ';
                out += l;
            }
            v.assign(1, out);
        }, true);
        break;
    }
    case IDM_EDIT_TAB_TO_SPACE: {
        int tab = settings.tabWidth;
        ed.TransformLines([tab](std::vector<std::wstring>& v) { for (auto& l : v) l = ExpandTabs(l, tab); }, true);
        break;
    }
    case IDM_EDIT_SPACE_TO_TAB_ALL:
    case IDM_EDIT_SPACE_TO_TAB_LEADING: {
        int tab = settings.tabWidth;
        bool lead = id == IDM_EDIT_SPACE_TO_TAB_LEADING;
        ed.TransformLines([tab, lead](std::vector<std::wstring>& v) {
            for (auto& l : v) l = SpacesToTabs(l, tab, lead);
        }, true);
        break;
    }
    case IDM_EDIT_OPEN_SELECTION:
    case IDM_EDIT_OPEN_SEL_FOLDER: {
        std::wstring p = TrimRight(TrimLeft(ed.GetSelText()));
        if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
        if (PathIsRelativeW(p.c_str()) && !d->path.empty()) p = DirOf(d->path) + L"\\" + p;
        p = FullPath(p);
        if (id == IDM_EDIT_OPEN_SELECTION) {
            if (FileExists(p))
                OpenFile(p);
            else
                Error(L"\"" + p + L"\" doesn't exist.");
        } else if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ShellOpen(L"explorer.exe", L"/select,\"" + p + L"\"");
        }
        break;
    }
    case IDM_EDIT_SEARCH_INTERNET:
        if (ed.HasSelection()) ShellOpen(L"https://duckduckgo.com/?q=" + UrlEncode(ed.GetSelText()));
        break;
    case IDM_EDIT_REDACT: {
        wchar_t mark = GetKeyState(VK_SHIFT) < 0 ? L'\x25CF' : L'\x2588';
        ed.TransformSelection([mark](const std::wstring& s) {
            std::wstring r = s;
            for (auto& c : r)
                if (c != L'\n' && c != L' ' && c != L'\t') c = mark;
            return r;
        });
        break;
    }
    case IDM_EDIT_READONLY:
        d->readOnly = !d->readOnly;
        UpdateTab(current);
        break;
    case IDM_EDIT_SETREADONLY_ATTR: {
        DWORD a = GetFileAttributesW(d->path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) break;
        a ^= FILE_ATTRIBUTE_READONLY;
        if (SetFileAttributesW(d->path.c_str(), a)) d->fileReadOnly = (a & FILE_ATTRIBUTE_READONLY) != 0;
        UpdateTab(current);
        break;
    }

    // Search
    case IDM_SEARCH_FIND: ShowFindDialog(0); break;
    case IDM_SEARCH_REPLACE: ShowFindDialog(1); break;
    case IDM_SEARCH_FINDINFILES: ShowFindDialog(2); break;
    case IDM_SEARCH_MARK: ShowFindDialog(3); break;
    case IDM_SEARCH_FINDNEXT: FindNext(false); break;
    case IDM_SEARCH_FINDPREV: FindNext(true); break;
    case IDM_SEARCH_SELECT_FIND_NEXT: SelectAndFindNext(false); break;
    case IDM_SEARCH_SELECT_FIND_PREV: SelectAndFindNext(true); break;
    case IDM_SEARCH_GOTOLINE: ShowGotoDialog(); break;
    case IDM_SEARCH_GOTO_BRACE: ed.GoToMatchingBrace(false); break;
    case IDM_SEARCH_SELECT_BRACES: ed.SelectBetweenBraces(); break;
    case IDM_SEARCH_CLEAR_MARKS:
        d->marks.clear();
        ed.Invalidate();
        break;
    case IDM_SEARCH_RESULTS_WINDOW: ShowResultsPanel(!resultsVisible); break;
    case IDM_SEARCH_NEXT_RESULT: ResultsNavigate(1); break;
    case IDM_SEARCH_PREV_RESULT: ResultsNavigate(-1); break;
    case IDM_SEARCH_TOGGLE_BOOKMARK: ed.ToggleBookmark(ed.Caret().line); break;
    case IDM_SEARCH_NEXT_BOOKMARK: ed.NextBookmark(true); break;
    case IDM_SEARCH_PREV_BOOKMARK: ed.NextBookmark(false); break;
    case IDM_SEARCH_CLEAR_BOOKMARKS:
        std::fill(d->bookmarks.begin(), d->bookmarks.end(), 0);
        ed.Invalidate();
        break;
    case IDM_SEARCH_INVERSE_MARKS:
        for (auto& b : d->bookmarks) b = !b;
        ed.Invalidate();
        break;
    case IDM_SEARCH_COPY_MARKED:
    case IDM_SEARCH_CUT_MARKED: {
        std::wstring t;
        for (int l = 0; l < d->LineCount(); l++)
            if (d->bookmarks[l]) t += d->lines[l] + L"\r\n";
        if (t.empty()) break;
        SetClipboardText(t);
        if (id == IDM_SEARCH_COPY_MARKED) break;
        [[fallthrough]];
    }
    case IDM_SEARCH_DELETE_MARKED:
    case IDM_SEARCH_DELETE_UNMARKED: {
        if (!ed.Editable()) break;
        bool keepMarked = id == IDM_SEARCH_DELETE_UNMARKED;
        std::vector<std::wstring> lines;
        std::vector<uint8_t> marks;
        for (int l = 0; l < d->LineCount(); l++)
            if ((d->bookmarks[l] != 0) == keepMarked) {
                lines.push_back(d->lines[l]);
                marks.push_back(d->bookmarks[l]);
            }
        if (lines.empty()) {
            lines.push_back(L"");
            marks.push_back(0);
        }
        ReplaceWholeDocument(d, lines, marks);
        break;
    }

    // View
    case IDM_VIEW_ALWAYSONTOP:
        settings.alwaysOnTop = !settings.alwaysOnTop;
        SetWindowPos(hMain, settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        break;
    case IDM_VIEW_FULLSCREEN: ToggleFullScreen(); break;
    case IDM_VIEW_POSTIT: TogglePostIt(); break;
    case IDM_VIEW_DARKMODE:
        settings.dark = !settings.dark;
        ApplyEditorSettings();
        break;
    case IDM_VIEW_IN_FIREFOX:
    case IDM_VIEW_IN_CHROME:
    case IDM_VIEW_IN_EDGE:
        if (d->path.empty()) {
            Error(L"Save the document first.");
            break;
        }
        ShellOpen(id == IDM_VIEW_IN_FIREFOX ? L"firefox.exe" : id == IDM_VIEW_IN_CHROME ? L"chrome.exe" : L"msedge.exe",
                  L"\"" + d->path + L"\"");
        break;
    case IDM_VIEW_TAB_SPACE:
        settings.showSpaces = !(settings.showSpaces && !settings.showEol);
        settings.showEol = false;
        ApplyEditorSettings();
        break;
    case IDM_VIEW_EOL:
        settings.showEol = !(settings.showEol && !settings.showSpaces);
        settings.showSpaces = false;
        ApplyEditorSettings();
        break;
    case IDM_VIEW_ALL_CHARS: {
        bool on = !(settings.showEol && settings.showSpaces);
        settings.showEol = settings.showSpaces = on;
        ApplyEditorSettings();
        break;
    }
    case IDM_VIEW_INDENT_GUIDE:
        settings.indentGuides = !settings.indentGuides;
        ApplyEditorSettings();
        break;
    case IDM_VIEW_WRAP:
        settings.wordWrap = !settings.wordWrap;
        ApplyEditorSettings();
        break;
    case IDM_VIEW_ZOOMIN: ed.ZoomBy(1); break;
    case IDM_VIEW_ZOOMOUT: ed.ZoomBy(-1); break;
    case IDM_VIEW_ZOOMRESTORE: ed.ZoomReset(); break;
    case IDM_VIEW_TAB_NEXT: Activate((current + 1) % (int)docs.size()); break;
    case IDM_VIEW_TAB_PREV: Activate((current + (int)docs.size() - 1) % (int)docs.size()); break;
    case IDM_VIEW_TAB_FIRST: Activate(0); break;
    case IDM_VIEW_TAB_LAST: Activate((int)docs.size() - 1); break;
    case IDM_VIEW_TAB_MOVEFORWARD: MoveTab(current, std::min(current + 1, (int)docs.size() - 1)); break;
    case IDM_VIEW_TAB_MOVEBACKWARD: MoveTab(current, std::max(current - 1, 0)); break;
    case IDM_VIEW_SUMMARY: ShowSummary(); break;
    case IDM_VIEW_MONITORING:
        if (d->path.empty()) break;
        if (!d->monitoring && d->IsModified()) {
            if (Ask(L"The document is modified. Save it before monitoring?", MB_YESNO | MB_ICONQUESTION) != IDYES)
                break;
            if (!SaveDoc(current, false)) break;
        }
        d->monitoring = !d->monitoring;
        if (d->monitoring) ed.SetSel(d->EndPos(), d->EndPos());
        UpdateTab(current);
        break;

    // Encoding
    case IDM_FORMAT_ANSI: SetEncoding(Enc::ANSI, 0, false); break;
    case IDM_FORMAT_UTF8: SetEncoding(Enc::UTF8, 0, false); break;
    case IDM_FORMAT_UTF8BOM: SetEncoding(Enc::UTF8BOM, 0, false); break;
    case IDM_FORMAT_UTF16BE: SetEncoding(Enc::UTF16BE, 0, false); break;
    case IDM_FORMAT_UTF16LE: SetEncoding(Enc::UTF16LE, 0, false); break;
    case IDM_FORMAT_CONV_ANSI: SetEncoding(Enc::ANSI, 0, true); break;
    case IDM_FORMAT_CONV_UTF8: SetEncoding(Enc::UTF8, 0, true); break;
    case IDM_FORMAT_CONV_UTF8BOM: SetEncoding(Enc::UTF8BOM, 0, true); break;
    case IDM_FORMAT_CONV_UTF16BE: SetEncoding(Enc::UTF16BE, 0, true); break;
    case IDM_FORMAT_CONV_UTF16LE: SetEncoding(Enc::UTF16LE, 0, true); break;
    case IDM_FORMAT_WIN1250: SetEncoding(Enc::CODEPAGE, 1250, false); break;
    case IDM_FORMAT_WIN1251: SetEncoding(Enc::CODEPAGE, 1251, false); break;
    case IDM_FORMAT_WIN1252: SetEncoding(Enc::CODEPAGE, 1252, false); break;
    case IDM_FORMAT_WIN1253: SetEncoding(Enc::CODEPAGE, 1253, false); break;
    case IDM_FORMAT_WIN1254: SetEncoding(Enc::CODEPAGE, 1254, false); break;
    case IDM_FORMAT_ISO8859_1: SetEncoding(Enc::CODEPAGE, 28591, false); break;
    case IDM_FORMAT_ISO8859_2: SetEncoding(Enc::CODEPAGE, 28592, false); break;
    case IDM_FORMAT_ISO8859_5: SetEncoding(Enc::CODEPAGE, 28595, false); break;
    case IDM_FORMAT_ISO8859_7: SetEncoding(Enc::CODEPAGE, 28597, false); break;
    case IDM_FORMAT_OEM437: SetEncoding(Enc::CODEPAGE, 437, false); break;
    case IDM_FORMAT_OEM850: SetEncoding(Enc::CODEPAGE, 850, false); break;
    case IDM_FORMAT_OEM855: SetEncoding(Enc::CODEPAGE, 855, false); break;
    case IDM_FORMAT_OEM866: SetEncoding(Enc::CODEPAGE, 866, false); break;
    case IDM_FORMAT_KOI8U: SetEncoding(Enc::CODEPAGE, 21866, false); break;
    case IDM_FORMAT_KOI8R: SetEncoding(Enc::CODEPAGE, 20866, false); break;

    // Settings & tools
    case IDM_SETTING_PREFERENCES:
        if (ShowPreferences()) {
            ApplyEditorSettings();
            SaveSettings();
        }
        break;
    case IDM_TOOL_MD5_GENERATE: ShowHashDialog(0, false); break;
    case IDM_TOOL_MD5_FILES: ShowHashDialog(0, true); break;
    case IDM_TOOL_MD5_SELECTION: HashSelectionToClipboard(0); break;
    case IDM_TOOL_SHA1_GENERATE: ShowHashDialog(1, false); break;
    case IDM_TOOL_SHA1_FILES: ShowHashDialog(1, true); break;
    case IDM_TOOL_SHA1_SELECTION: HashSelectionToClipboard(1); break;
    case IDM_TOOL_SHA256_GENERATE: ShowHashDialog(2, false); break;
    case IDM_TOOL_SHA256_FILES: ShowHashDialog(2, true); break;
    case IDM_TOOL_SHA256_SELECTION: HashSelectionToClipboard(2); break;
    case IDM_TOOL_SHA512_GENERATE: ShowHashDialog(3, false); break;
    case IDM_TOOL_SHA512_FILES: ShowHashDialog(3, true); break;
    case IDM_TOOL_SHA512_SELECTION: HashSelectionToClipboard(3); break;

    // Macro
    case IDM_MACRO_START:
        if (recordingMacro) {
            ExecuteCommand(IDM_MACRO_STOP);
            break;
        }
        macro.clear();
        recordingMacro = true;
        ed.recording = true;
        ed.onMacroRecord = [](const MacroStep& s) { macro.push_back(s); };
        UpdateStatus();
        break;
    case IDM_MACRO_STOP:
        recordingMacro = false;
        ed.recording = false;
        UpdateStatus();
        break;
    case IDM_MACRO_PLAYBACK:
        if (!recordingMacro) PlayMacro(1, false);
        break;
    case IDM_MACRO_RUN_MULTI: {
        int times = 1;
        bool eof = false;
        if (!recordingMacro && ShowMacroMultiDialog(times, eof)) PlayMacro(times, eof);
        break;
    }

    case IDM_RUN: ShowRunDialog(); break;
    case IDM_WINDOW_WINDOWS: ShowWindowsDialog(); break;
    case IDM_WINDOW_SORT_NAME_ASC: SortTabs(false, false); break;
    case IDM_WINDOW_SORT_NAME_DESC: SortTabs(false, true); break;
    case IDM_WINDOW_SORT_PATH_ASC: SortTabs(true, false); break;
    case IDM_WINDOW_SORT_PATH_DESC: SortTabs(true, true); break;
    case IDM_ABOUT: ShowAbout(); break;
    case IDM_CMDLINE_HELP:
        MessageBoxW(hMain,
                    L"Usage:\n\nTetradkata++.exe [-multiInst] [-nosession] [-ro] [-alwaysOnTop] [-n<line>] "
                    L"[-c<column>] [-l<Language>] [file1 file2 ...]\n\n"
                    L"-multiInst\tStart another instance instead of reusing the open window\n"
                    L"-nosession\tDo not restore or save the session\n"
                    L"-ro\t\tOpen the files read-only\n"
                    L"-alwaysOnTop\tKeep the window above other windows\n"
                    L"-n<line>\tGo to the given line\n"
                    L"-c<column>\tGo to the given column\n"
                    L"-l<Language>\tSet the language, e.g. -lPython",
                    L"Command Line Arguments", MB_OK | MB_ICONINFORMATION);
        break;
    case IDM_DEBUGINFO: {
        std::wstring info = L"Tetradkata++ v1.0 (64-bit)\r\nBuild time: " __DATE__ L" " __TIME__ L"\r\nPath: " + ExeDir() +
                            L"\\Tetradkata++.exe\r\nSettings: " + ConfigDir() + L"\r\nPortable mode: " +
                            (ConfigDir() == ExeDir() ? L"ON" : L"OFF") + L"\r\nOpen documents: " +
                            std::to_wstring(docs.size()) + L"\r\n";
        if (MessageBoxW(hMain, (info + L"\r\nCopy to clipboard?").c_str(), L"Debug Info", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            SetClipboardText(info);
        break;
    }
    }
}

namespace {

// ---------------------------------------------------------------- command line & single instance

CmdLine ParseCommandLine(const std::vector<std::wstring>& args, const std::wstring& cwd) {
    CmdLine c;
    for (const auto& a : args) {
        if (a.empty()) continue;
        if (a[0] == L'-') {
            if (_wcsicmp(a.c_str(), L"-multiInst") == 0) c.multiInst = true;
            else if (_wcsicmp(a.c_str(), L"-nosession") == 0) c.noSession = true;
            else if (_wcsicmp(a.c_str(), L"-ro") == 0) c.readOnly = true;
            else if (_wcsicmp(a.c_str(), L"-alwaysOnTop") == 0) c.onTop = true;
            else if (a.size() > 2 && a[1] == L'n') c.line = _wtoi(a.c_str() + 2);
            else if (a.size() > 2 && a[1] == L'c') c.col = _wtoi(a.c_str() + 2);
            else if (a.size() > 2 && a[1] == L'l') c.lang = a.substr(2);
            continue;
        }
        std::wstring p = a;
        if (PathIsRelativeW(p.c_str()) && !cwd.empty()) p = cwd + L"\\" + p;
        c.files.push_back(FullPath(p));
    }
    return c;
}

void ApplyCommandLine(const CmdLine& c) {
    int last = -1;
    for (const auto& f : c.files) {
        int i = OpenFile(f, false);
        if (i < 0) continue;
        last = i;
        if (c.readOnly) docs[i]->readOnly = true;
        if (!c.lang.empty()) {
            int l = LanguageByName(c.lang);
            if (l >= 0) docs[i]->lang = l;
        }
        UpdateTab(i);
    }
    if (last >= 0) {
        Activate(last);
        if (c.line > 0) {
            Pos p = docs[last]->Clamp({c.line - 1, std::max(0, c.col - 1)});
            editor.SetSel(p, p, true, true);
        }
    }
    if (c.onTop && !settings.alwaysOnTop) ExecuteCommand(IDM_VIEW_ALWAYSONTOP);
}

// ---------------------------------------------------------------- tab control subclass (drag to reorder, middle-click to close)

int TabHit(int x, int y) {
    TCHITTESTINFO ht{{x, y}, 0};
    return TabCtrl_HitTest(hTab, &ht);
}

int dragTab = -1;

LRESULT CALLBACK TabProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        dragTab = TabHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_MOUSEMOVE:
        if (dragTab >= 0 && (wp & MK_LBUTTON)) {
            int over = TabHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (over >= 0 && over != dragTab) {
                MoveTab(dragTab, over);
                dragTab = over;
            }
        }
        break;
    case WM_LBUTTONUP: dragTab = -1; break;
    case WM_MBUTTONUP: {
        int i = TabHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i >= 0) CloseDoc(i);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (TabHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) < 0) {
            NewDocument();
            return 0;
        }
        break;
    case WM_RBUTTONUP: {
        int i = TabHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i >= 0) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(h, &pt);
            ShowTabContextMenu(i, pt.x, pt.y);
        }
        return 0;
    }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void CreateChildren(HWND h) {
    hToolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP | CCS_NODIVIDER, 0, 0,
                               0, 0, h, (HMENU)IDC_TOOLBAR, hInst, nullptr);
    SendMessageW(hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    TBADDBITMAP tbab{HINST_COMMCTRL, (UINT_PTR)(dpi >= 144 ? IDB_STD_LARGE_COLOR : IDB_STD_SMALL_COLOR)};
    SendMessageW(hToolbar, TB_ADDBITMAP, 0, (LPARAM)&tbab);
    struct B {
        int image, cmd;
        const wchar_t* tip;
    } buttons[] = {{STD_FILENEW, IDM_FILE_NEW, L"New"},           {STD_FILEOPEN, IDM_FILE_OPEN, L"Open"},
                   {STD_FILESAVE, IDM_FILE_SAVE, L"Save"},        {-1, 0, nullptr},
                   {STD_PRINT, IDM_FILE_PRINT, L"Print"},         {-1, 0, nullptr},
                   {STD_CUT, IDM_EDIT_CUT, L"Cut"},               {STD_COPY, IDM_EDIT_COPY, L"Copy"},
                   {STD_PASTE, IDM_EDIT_PASTE, L"Paste"},         {-1, 0, nullptr},
                   {STD_UNDO, IDM_EDIT_UNDO, L"Undo"},            {STD_REDOW, IDM_EDIT_REDO, L"Redo"},
                   {-1, 0, nullptr},                              {STD_FIND, IDM_SEARCH_FIND, L"Find"},
                   {STD_REPLACE, IDM_SEARCH_REPLACE, L"Replace"}, {-1, 0, nullptr},
                   {STD_PROPERTIES, IDM_SETTING_PREFERENCES, L"Preferences"}};
    std::vector<TBBUTTON> tb;
    for (auto& b : buttons) {
        TBBUTTON t{};
        if (b.image < 0) {
            t.fsStyle = BTNS_SEP;
        } else {
            t.iBitmap = b.image;
            t.idCommand = b.cmd;
            t.fsState = TBSTATE_ENABLED;
            t.fsStyle = BTNS_BUTTON;
            t.iString = (INT_PTR)b.tip;
        }
        tb.push_back(t);
    }
    SendMessageW(hToolbar, TB_SETMAXTEXTROWS, 0, 0);
    SendMessageW(hToolbar, TB_ADDBUTTONSW, tb.size(), (LPARAM)tb.data());
    SendMessageW(hToolbar, TB_AUTOSIZE, 0, 0);

    hTab = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_TOOLTIPS, 0, 0, 0, 0, h,
                           (HMENU)IDC_TABS, hInst, nullptr);
    SendMessageW(hTab, WM_SETFONT, (WPARAM)uiFont, FALSE);
    SetWindowSubclass(hTab, TabProc, 1, 0);
    FillTabIcons();

    editor.Create(h, IDC_EDITOR);
    editor.onUpdateUI = [] { UpdateStatus(); };
    editor.onSavePointChanged = [] {
        UpdateTab(current);
        UpdateTitle();
    };
    editor.onContextMenu = ShowEditorContextMenu;

    hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h,
                              (HMENU)IDC_STATUS, hInst, nullptr);
    SendMessageW(hStatus, WM_SETFONT, (WPARAM)uiFont, FALSE);

    hResults = CreateResultsPanel(h);
    ShowWindow(hResults, SW_HIDE);
}

bool OnExit() {
    if (settings.rememberSession && !noSession && settings.backup) {
        SaveSession();
    } else {
        for (size_t i = 0; i < docs.size(); i++) {
            if (!docs[i]->IsModified()) continue;
            Activate((int)i);
            Document* d = docs[i].get();
            int r = Ask(L"Save file \"" + (d->path.empty() ? d->title : d->path) + L"\" ?",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return false;
            if (r == IDYES && !SaveDoc((int)i, false)) return false;
        }
        SaveSession();
    }
    SaveSettings();
    return true;
}

HBRUSH resultsDarkBrush;

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        hMain = h;
        dpi = GetDpiForWindow(h);
        CreateUiFont();
        CreateChildren(h);
        BuildLanguageMenu();
        DragAcceptFiles(h, TRUE);
        SetTimer(h, kBackupTimer, 7000, nullptr);
        SetTimer(h, kMonitorTimer, 1000, nullptr);
        return 0;
    case WM_SIZE:
        Relayout();
        UpdateStatus();
        return 0;
    case WM_SETFOCUS: SetFocus(editor.hwnd); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RESULTS) {
            if (HIWORD(wp) == LBN_DBLCLK) ResultsNavigate(0);
            return 0;
        }
        if (lp == 0 || (HWND)lp == hToolbar) ExecuteCommand(LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->hwndFrom == hTab && nm->code == TCN_SELCHANGE) {
            Activate(TabCtrl_GetCurSel(hTab));
            SetFocus(editor.hwnd);
        } else if (nm->code == TTN_GETDISPINFOW && nm->hwndFrom == TabCtrl_GetToolTips(hTab)) {
            auto* di = (NMTTDISPINFOW*)lp;
            static std::wstring tip;
            int i = (int)nm->idFrom;
            if (i >= 0 && i < (int)docs.size()) {
                tip = docs[i]->path.empty() ? docs[i]->title : docs[i]->path;
                di->lpszText = &tip[0];
            }
        } else if (nm->hwndFrom == hStatus && nm->code == NM_DBLCLK) {
            int part = (int)((NMMOUSE*)lp)->dwItemSpec;
            if (part == 2) ShowGotoDialog();
            else if (part == 1) ShowSummary();
            else if (part == 5) {
                editor.overwrite = !editor.overwrite;
                UpdateStatus();
            }
        }
        return 0;
    }
    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        if (GetMenu(h) && m == GetSubMenu(GetMenu(h), 0)) RebuildRecentMenu(m);
        UpdateMenus(m);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, &p[0], len + 1);
            OpenFile(p);
        }
        DragFinish(drop);
        SetForegroundWindow(h);
        return 0;
    }
    case WM_COPYDATA: {
        auto* cds = (COPYDATASTRUCT*)lp;
        if (cds->dwData != kCopyDataTag) return FALSE;
        std::wstring data((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
        std::vector<std::wstring> parts;
        size_t s = 0;
        for (size_t i = 0; i <= data.size(); i++)
            if (i == data.size() || data[i] == L'\n') {
                parts.push_back(data.substr(s, i - s));
                s = i + 1;
            }
        std::wstring cwd = parts.empty() ? L"" : parts[0];
        if (!parts.empty()) parts.erase(parts.begin());
        ApplyCommandLine(ParseCommandLine(parts, cwd));
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return TRUE;
    }
    case WM_ACTIVATEAPP:
        if (wp) PostMessageW(h, WM_APP_CHECKFILES, 0, 0);
        return 0;
    case WM_APP_CHECKFILES: CheckFiles(); return 0;
    case WM_TIMER:
        if (wp == kBackupTimer) {
            if (settings.backup && settings.rememberSession && !noSession && SessionSignature() != lastSessionSignature)
                SaveSession();
        } else if (wp == kMonitorTimer) {
            MonitorTick();
        } else if (wp == kFlashTimer) {
            KillTimer(h, kFlashTimer);
            flashText.clear();
            UpdateStatus();
        }
        return 0;
    case WM_CTLCOLORLISTBOX:
        if ((HWND)lp == hResults && settings.dark) {
            if (!resultsDarkBrush) resultsDarkBrush = CreateSolidBrush(RGB(0x2B, 0x2B, 0x2B));
            SetTextColor((HDC)wp, RGB(0xDC, 0xDC, 0xCC));
            SetBkColor((HDC)wp, RGB(0x2B, 0x2B, 0x2B));
            return (LRESULT)resultsDarkBrush;
        }
        break;
    case WM_SETCURSOR:
        if ((HWND)wp == h && resultsVisible) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            RECT er, rr;
            GetWindowRect(editor.hwnd, &er);
            GetWindowRect(hResults, &rr);
            MapWindowPoints(nullptr, h, (POINT*)&er, 2);
            MapWindowPoints(nullptr, h, (POINT*)&rr, 2);
            if (pt.y >= er.bottom && pt.y < rr.top) {
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (resultsVisible) {
            splitterDragY = GET_Y_LPARAM(lp);
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (splitterDragY >= 0 && (wp & MK_LBUTTON)) {
            RECT rc, sr;
            GetClientRect(h, &rc);
            GetWindowRect(hStatus, &sr);
            int statusH = IsWindowVisible(hStatus) ? sr.bottom - sr.top : 0;
            settings.resultsHeight = std::max(Scale(60), (int)rc.bottom - statusH - GET_Y_LPARAM(lp) - Scale(3));
            Relayout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (splitterDragY >= 0) {
            splitterDragY = -1;
            ReleaseCapture();
        }
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        CreateUiFont();
        SendMessageW(hTab, WM_SETFONT, (WPARAM)uiFont, TRUE);
        SendMessageW(hStatus, WM_SETFONT, (WPARAM)uiFont, TRUE);
        FillTabIcons();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        return 0;
    }
    case WM_CLOSE:
        if (!OnExit()) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    hInst = hi;
    setlocale(LC_CTYPE, "");
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    LocalFree(argv);
    wchar_t cwdBuf[MAX_PATH * 4];
    GetCurrentDirectoryW((DWORD)std::size(cwdBuf), cwdBuf);
    std::wstring cwd = cwdBuf;
    CmdLine cmd = ParseCommandLine(args, cwd);

    LoadSettings();
    noSession = cmd.noSession;

    if (!cmd.multiInst && !settings.multiInstance) {
        if (HWND other = FindWindowW(kMainClass, nullptr)) {
            std::wstring data = cwd;
            for (auto& a : args) data += L"\n" + a;
            COPYDATASTRUCT cds{kCopyDataTag, (DWORD)(data.size() * sizeof(wchar_t)), (void*)data.c_str()};
            DWORD_PTR result;
            SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result);
            SetForegroundWindow(other);
            return 0;
        }
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);
    Editor::Register(hi);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hi;
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, nullptr, nullptr, hi, nullptr);
    if (!h) return 1;
    hAccel = LoadAcceleratorsW(hi, MAKEINTRESOURCEW(IDR_ACCEL));

    editor.zoom = settings.zoom;
    NewDocument();
    ApplyEditorSettings();
    if (settings.rememberSession && !noSession) {
        int a = ReadSession(ConfigDir() + L"\\session.ini", settings.backup);
        if (a >= 0) Activate(a);
        lastSessionSignature = SessionSignature();
    }
    ApplyCommandLine(cmd);
    if (settings.alwaysOnTop) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    if (settings.hasPlacement) {
        WINDOWPLACEMENT p = settings.placement;
        if (show == SW_SHOWMINNOACTIVE || show == SW_SHOWMINIMIZED) p.showCmd = show;
        SetWindowPlacement(h, &p);
    } else {
        ShowWindow(h, show);
    }
    UpdateWindow(h);
    SetFocus(editor.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        HWND fd = FindDialogHwnd();
        if (fd && (msg.hwnd == fd || IsChild(fd, msg.hwnd))) {
            if (msg.message == WM_KEYDOWN && (msg.wParam == VK_F3 || msg.wParam == VK_F4) &&
                TranslateAcceleratorW(hMain, hAccel, &msg))
                continue;
            if (IsDialogMessageW(fd, &msg)) continue;
        } else if (TranslateAcceleratorW(hMain, hAccel, &msg)) {
            continue;
        }
        HWND active = GetActiveWindow();
        if (active && active != hMain && IsDialogMessageW(active, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return 0;
}
