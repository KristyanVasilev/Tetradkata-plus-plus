// Syntax highlighting: language table, line lexers and colour themes.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum Style : uint8_t {
    ST_DEFAULT, ST_COMMENT, ST_COMMENTDOC, ST_KEYWORD, ST_TYPE, ST_NUMBER, ST_STRING, ST_CHAR,
    ST_OPERATOR, ST_PREPROC, ST_TRIPLE, ST_DEFNAME, ST_CLASSNAME, ST_BUILTIN, ST_DECORATOR,
    ST_TAG, ST_ATTR, ST_ATTRVAL, ST_ENTITY, ST_SECTION, ST_KEY, ST_VARIABLE, ST_LABEL,
    ST_HEADING, ST_EMPHASIS, ST_CODE, ST_DIFFADD, ST_DIFFDEL, ST_DIFFHDR, ST_COUNT
};

enum class LexKind { None, CLike, Markup, Ini, Batch, Diff, Markdown };

enum LangFlags : unsigned {
    LF_CASEI = 1,       // keywords are case-insensitive
    LF_HASHPREPROC = 2, // '#' at line start starts a preprocessor line
    LF_BACKTICK = 4,    // `template strings` (may span lines)
    LF_TRIPLE = 8,      // """triple""" strings (may span lines)
    LF_DOLLARVAR = 16,  // $variables
    LF_DECORATOR = 32,  // @decorators / annotations
    LF_SQSTRING = 64,   // single quotes delimit strings, not chars
    LF_DEFNAME = 128,   // colour the name after def/class/fn/function/func
    LF_NOSQ = 256,      // single quote is not a string delimiter
    LF_HTML = 512,      // markup with <script>/<style> blocks
    LF_JSONKEY = 1024,  // "key": strings get key colour
    LF_NOESCAPE = 2048, // backslash does not escape inside strings
    LF_CSSHEX = 4096,   // #rrggbb colours are numbers
    LF_DASHID = 8192,   // identifiers may contain '-' (CSS properties, PowerShell cmdlets)
    LF_COLONKEY = 16384, // `name:` at a key position gets key colour (YAML, Makefile targets)
};

struct Language {
    int menuId;
    const wchar_t* name;     // menu entry
    const wchar_t* longName; // status bar
    const wchar_t* exts;     // space separated, lower case, no dots
    LexKind kind;
    const wchar_t* keywords;
    const wchar_t* types;
    const wchar_t* lineComment;
    const wchar_t* lineComment2;
    const wchar_t* blockStart;
    const wchar_t* blockEnd;
    unsigned flags;
};

int LanguageCount();
const Language& GetLanguage(int index);
int LanguageByMenuId(int id); // -1 when unknown
int LanguageByName(const std::wstring& name);
int LanguageForPath(const std::wstring& path);

// Lexes one line. Fills styles (one per UTF-16 unit) and returns the state at the start of the next line.
int LexLine(int lang, const std::wstring& text, int state, std::vector<uint8_t>& styles);

struct StyleDef {
    COLORREF fg;
    bool bold, italic;
};

struct Theme {
    COLORREF bg, fg, curLine, selBg, caret, lineNumFg, lineNumBg, bookmarkBg, bookmark, whitespace,
        indentGuide, smart, findMark, braceFg, badBraceFg, eolBoxBg, eolBoxFg;
    StyleDef styles[ST_COUNT];
};

const Theme& GetTheme(bool dark);
