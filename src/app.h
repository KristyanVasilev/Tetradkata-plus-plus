// Shared state of the main window, used by main.cpp and dialogs.cpp.
#pragma once
#include "document.h"
#include "editor.h"

#include <memory>
#include <string>
#include <vector>

struct Settings {
    int tabWidth = 4;
    bool useSpaces = false;
    std::wstring fontName = L"Courier New"; // Notepad++ default style font (finding #9)
    int fontSize = 10;
    bool dark = false, rememberSession = true, backup = true, smartHighlight = true, autoIndent = true;
    bool lineNumbers = true, bookmarkMargin = true, curLine = true, multiInstance = false;
    bool autoClose = false, autoComplete = true;
    int newEnc = (int)Enc::UTF8;
    int newEol = (int)Eol::CRLF;
    bool wordWrap = false, showSpaces = false, showEol = false, indentGuides = true, alwaysOnTop = false;
    int zoom = 0;
    WINDOWPLACEMENT placement{};
    bool hasPlacement = false;
    std::vector<std::wstring> recent, findHistory, replaceHistory, runHistory;
    std::wstring findDir, findFilters = L"*.*";
    bool matchCase = false, wholeWord = false, wrapAround = true, backward = false, bookmarkLine = false;
    int searchMode = 0; // 0 normal, 1 extended, 2 regex
    bool subfolders = true, hiddenFolders = false, followCurDir = true;
    int resultsHeight = 0;
};

struct SearchHit {
    Document* doc;     // open document, or nullptr for a file on disk
    std::wstring path; // file path (or title of an untitled document)
    int line, col, len;
    std::wstring text; // the matching line
};

namespace App {
extern HINSTANCE hInst;
extern HWND hMain;
extern Editor editor;
extern Settings settings;
extern std::vector<std::unique_ptr<Document>> docs;
extern int current;

Document* Cur();
int IndexOf(const Document* d);
void Activate(int index);
int OpenFile(const std::wstring& path, bool activate = true);
bool SaveDoc(int index, bool saveAs);
bool CloseDoc(int index, bool prompt = true);
void UpdateTitle();
void UpdateStatus();
void UpdateTab(int index);
void FlashStatus(const std::wstring& text);
void SetClipboardText(const std::wstring& text);
std::wstring ConfigDir();
std::wstring ExeDir();
void SaveSettings();
void ApplyEditorSettings();
void ExecuteCommand(int id);
void ShowResultsPanel(bool show);
void Relayout();
} // namespace App

// dialogs.cpp
void ShowFindDialog(int tab); // 0 find, 1 replace, 2 find in files, 3 mark
HWND FindDialogHwnd();
bool FindNext(bool backward);
void SelectAndFindNext(bool backward);
void ShowGotoDialog();
void ShowRunDialog();
bool ShowPreferences();
void ShowHashDialog(int algo, bool files); // 0 MD5, 1 SHA-1, 2 SHA-256, 3 SHA-512
std::wstring HashString(int algo, const std::wstring& text);
void ShowWindowsDialog();
bool ShowMacroMultiDialog(int& times, bool& untilEof);
void ShowAbout();

HWND CreateResultsPanel(HWND parent);
HWND ResultsPanel();
void ResultsNavigate(int dir); // +1 next, -1 previous
void SetResults(const std::wstring& what, std::vector<SearchHit> hits, int filesSearched);
void ForgetDocumentInResults(const Document* d);
