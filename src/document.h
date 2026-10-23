// Text buffer for one tab: lines of UTF-16 text plus undo history and view state.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct Pos {
    int line = 0;
    int col = 0;
};
inline bool operator==(Pos a, Pos b) { return a.line == b.line && a.col == b.col; }
inline bool operator!=(Pos a, Pos b) { return !(a == b); }
inline bool operator<(Pos a, Pos b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }
inline bool operator<=(Pos a, Pos b) { return !(b < a); }
inline bool operator>(Pos a, Pos b) { return b < a; }

enum class Eol { CRLF, LF, CR };
enum class Enc { ANSI, UTF8, UTF8BOM, UTF16BE, UTF16LE, CODEPAGE };

struct MarkRange {
    int line, col, len;
};

struct UndoStep {
    bool insert;       // true: text was inserted at pos; false: text was erased from pos
    Pos pos;
    std::wstring text; // '\n' separates lines
    Pos selAnchor, selCaret;
    int group;
};

// End position of `text` when inserted at `p`.
Pos EndOf(Pos p, const std::wstring& text);

class Document {
public:
    Document();

    std::vector<std::wstring> lines;
    std::vector<uint8_t> bookmarks; // parallel to lines
    std::wstring path;
    std::wstring title;
    Eol eol = Eol::CRLF;
    Enc enc = Enc::UTF8;
    UINT codepage = 0;
    int lang = 0;
    bool readOnly = false;     // Tetradkata++ read-only flag
    bool fileReadOnly = false; // read-only attribute on disk
    bool monitoring = false;   // tail -f
    FILETIME fileTime{};
    bool hasFileTime = false;
    int untitledNumber = 0;
    std::wstring backupPath;
    int backupVersion = -1;

    // View state, kept per document so switching tabs restores it.
    Pos caret, anchor;
    int desiredCell = -1;
    int topRow = 0;
    int xOffset = 0;
    std::vector<MarkRange> marks;

    // Lexer cache: lexState[i] is the lexer state at the start of line i, valid for i < lexValid.
    std::vector<int> lexState;
    int lexValid = 1;

    int version = 0; // incremented on every change

    // Editing primitives without undo.
    Pos RawInsert(Pos p, const std::wstring& text);
    void RawErase(Pos a, Pos b);

    std::wstring GetText(Pos a, Pos b) const;
    std::wstring GetAllText() const;
    void SetAllText(const std::wstring& text); // resets undo history
    int LineCount() const { return (int)lines.size(); }
    Pos Clamp(Pos p) const;
    Pos EndPos() const;
    size_t TextLength() const; // counted with the document's EOL
    size_t OffsetOf(Pos p) const; // '\n' counts as one
    Pos PosOfOffset(size_t off) const;
    std::wstring DisplayName() const;

    // Undoable editing.
    Pos Insert(Pos p, const std::wstring& text, bool typing = false);
    void Erase(Pos a, Pos b, bool typing = false);
    void BeginUndoGroup(Pos anchor, Pos caret);
    void EndUndoGroup();
    bool CanUndo() const { return !undo.empty(); }
    bool CanRedo() const { return !redo.empty(); }
    bool Undo(Pos& anchor, Pos& caret);
    bool Redo(Pos& anchor, Pos& caret);
    void ClearUndo();
    void SetSavePoint();
    void SetDirty(); // makes the save point unreachable
    bool IsModified() const;
    void BreakTyping() { typingOpen = false; }

private:
    std::vector<UndoStep> undo, redo;
    int groupDepth = 0, curGroup = 0, nextGroup = 1;
    Pos groupAnchor, groupCaret;
    bool typingOpen = false;
    long savePoint = 0; // undo.size() at the last save, -1 when unreachable
    void Touched(int line);
    void Record(UndoStep s);
};
