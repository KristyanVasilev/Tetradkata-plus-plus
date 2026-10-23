#include "lexer.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <unordered_set>

namespace {

constexpr size_t npos = std::wstring::npos;

// Menu IDs are the Notepad++ Language menu IDs (docs/REVERSE_ENGINEERING.md, finding #3).
// Keyword lists are written for Tetradkata++, not copied from langs.model.xml.
const Language kLanguages[] = {
    {46016, L"None (Normal Text)", L"Normal text file", L"txt log", LexKind::None, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, 0},
    {46022, L"Batch", L"Batch file", L"bat cmd nt", LexKind::Batch,
     L"echo set if else goto call for in do exist not errorlevel defined setlocal endlocal enabledelayedexpansion "
     L"enableextensions pushd popd cd chdir del erase copy xcopy move mkdir md rmdir rd exit shift pause cls title "
     L"start choice equ neq lss leq gtr geq nul off on type ren rename dir path prompt ver vol assoc break color date "
     L"time findstr find sort more timeout where",
     nullptr, nullptr, nullptr, nullptr, nullptr, LF_CASEI},
    {46002, L"C", L"C source file", L"c lex", LexKind::CLike,
     L"auto break case char const continue default do double else enum extern float for goto if inline int long "
     L"register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while "
     L"_Bool _Complex _Atomic _Generic _Noreturn _Static_assert _Thread_local _Alignas _Alignof bool true false "
     L"nullptr constexpr static_assert thread_local typeof alignas alignof",
     L"size_t ssize_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t intptr_t "
     L"uintptr_t wchar_t FILE NULL va_list time_t BOOL DWORD WORD BYTE HANDLE HWND HINSTANCE LPARAM WPARAM LRESULT",
     L"//", nullptr, L"/*", L"*/", LF_HASHPREPROC},
    {46023, L"C#", L"C# source file", L"cs csx", LexKind::CLike,
     L"abstract as base bool break byte case catch char checked class const continue decimal default delegate do "
     L"double else enum event explicit extern false finally fixed float for foreach goto if implicit in int "
     L"interface internal is lock long namespace new null object operator out override params private protected "
     L"public readonly ref return sbyte sealed short sizeof stackalloc static string struct switch this throw true "
     L"try typeof uint ulong unchecked unsafe ushort using virtual void volatile while add alias ascending async "
     L"await by descending dynamic equals from get global group init into join let nameof notnull on orderby "
     L"partial record remove required select set unmanaged value var when where with yield file scoped",
     L"String Int32 Int64 Boolean Object List Dictionary HashSet IEnumerable IList Task Console Math DateTime "
     L"TimeSpan Guid Exception Action Func Span Memory StringBuilder Array Enumerable",
     L"//", nullptr, L"/*", L"*/", LF_HASHPREPROC | LF_DECORATOR},
    {46003, L"C++", L"C++ source file", L"cpp cxx cc c++ h hh hpp hxx ino ipp inl tpp", LexKind::CLike,
     L"auto break case char const continue default do double else enum extern float for goto if inline int long "
     L"register return short signed sizeof static struct switch typedef union unsigned void volatile while bool "
     L"true false nullptr constexpr static_assert thread_local alignas alignof and and_eq asm bitand bitor catch "
     L"char8_t char16_t char32_t class co_await co_return co_yield compl concept consteval constinit const_cast "
     L"decltype delete dynamic_cast explicit export friend mutable namespace new noexcept not not_eq operator or "
     L"or_eq override final private protected public reinterpret_cast requires static_cast template this throw "
     L"try typeid typename using virtual xor xor_eq wchar_t",
     L"std string wstring string_view vector map unordered_map set unordered_set list deque array pair tuple "
     L"optional variant any unique_ptr shared_ptr weak_ptr function size_t ptrdiff_t int8_t int16_t int32_t "
     L"int64_t uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t thread mutex atomic span cout cin cerr endl "
     L"BOOL DWORD WORD BYTE HANDLE HWND HINSTANCE LPARAM WPARAM LRESULT NULL",
     L"//", nullptr, L"/*", L"*/", LF_HASHPREPROC},
    {46048, L"CMake", L"CMake file", L"cmake", LexKind::CLike,
     L"if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro set unset option "
     L"project cmake_minimum_required add_executable add_library add_custom_command add_custom_target "
     L"add_definitions add_compile_options add_dependencies target_link_libraries target_include_directories "
     L"target_compile_definitions target_compile_options target_sources include_directories link_directories "
     L"find_package find_library find_path include message install add_subdirectory list string file "
     L"configure_file return break continue enable_testing add_test",
     L"PUBLIC PRIVATE INTERFACE REQUIRED STATUS WARNING FATAL_ERROR ON OFF TRUE FALSE AND OR NOT DEFINED "
     L"STREQUAL EQUAL MATCHES CACHE PARENT_SCOPE",
     L"#", nullptr, nullptr, nullptr, LF_CASEI | LF_DOLLARVAR | LF_NOSQ},
    {46010, L"CSS", L"Cascade Style Sheets File", L"css scss less", LexKind::CLike,
     L"align-content align-items align-self animation background background-color background-image "
     L"background-position background-repeat background-size border border-bottom border-collapse border-color "
     L"border-left border-radius border-right border-style border-top border-width bottom box-shadow box-sizing "
     L"clear color column-gap content cursor display filter flex flex-basis flex-direction flex-grow flex-shrink "
     L"flex-wrap float font font-family font-size font-style font-weight gap grid grid-area grid-column grid-row "
     L"grid-template-areas grid-template-columns grid-template-rows height inset justify-content left "
     L"letter-spacing line-height list-style margin margin-bottom margin-left margin-right margin-top max-height "
     L"max-width min-height min-width object-fit opacity order outline overflow overflow-x overflow-y padding "
     L"padding-bottom padding-left padding-right padding-top pointer-events position right row-gap text-align "
     L"text-decoration text-overflow text-shadow text-transform top transform transition user-select "
     L"vertical-align visibility white-space width word-break word-wrap z-index",
     L"auto none inherit initial unset block inline inline-block flex grid absolute relative fixed sticky hidden "
     L"visible scroll solid dashed dotted bold normal italic center transparent important px em rem vh vw rgb "
     L"rgba hsl var calc url media import keyframes",
     nullptr, nullptr, L"/*", L"*/", LF_CSSHEX | LF_DASHID | LF_SQSTRING},
    {46034, L"Diff", L"Diff file", L"diff patch", LexKind::Diff, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, 0},
    {46089, L"Go", L"Go source file", L"go", LexKind::CLike,
     L"break case chan const continue default defer else fallthrough for func go goto if import interface map "
     L"package range return select struct switch type var true false nil iota",
     L"bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 "
     L"uint16 uint32 uint64 uintptr any comparable append cap clear close copy delete len make max min new panic "
     L"print println recover",
     L"//", nullptr, L"/*", L"*/", LF_BACKTICK | LF_DEFNAME},
    {46005, L"HTML", L"Hyper Text Markup Language file", L"html htm shtml shtm xhtml xht hta", LexKind::Markup,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, LF_HTML},
    {46019, L"INI file", L"MS ini file", L"ini inf url wer cfg conf editorconfig", LexKind::Ini, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, 0},
    {46004, L"Java", L"Java source file", L"java", LexKind::CLike,
     L"abstract assert boolean break byte case catch char class const continue default do double else enum "
     L"extends final finally float for goto if implements import instanceof int interface long native new "
     L"package private protected public return short static strictfp super switch synchronized this throw "
     L"throws transient try void volatile while true false null var record sealed permits yield",
     L"String Object Integer Long Double Float Boolean Character Byte Short List ArrayList Map HashMap Set "
     L"HashSet System Math Exception RuntimeException Thread Runnable Optional Stream StringBuilder",
     L"//", nullptr, L"/*", L"*/", LF_DECORATOR},
    {46007, L"JavaScript", L"JavaScript file", L"js jsm jsx mjs cjs", LexKind::CLike,
     L"break case catch class const continue debugger default delete do else export extends false finally for "
     L"function if import in instanceof let new null return super switch this throw true try typeof var void "
     L"while with yield async await of static get set undefined NaN Infinity",
     L"Array Boolean Date Error Function JSON Map Math Number Object Promise Proxy Reflect RegExp Set String "
     L"Symbol WeakMap WeakSet BigInt console window document globalThis require module exports",
     L"//", nullptr, L"/*", L"*/", LF_BACKTICK | LF_SQSTRING},
    {46057, L"JSON", L"JSON file", L"json jsonc json5 webmanifest", LexKind::CLike, L"true false null", nullptr,
     L"//", nullptr, L"/*", L"*/", LF_JSONKEY | LF_SQSTRING},
    {46024, L"Lua", L"Lua source File", L"lua", LexKind::CLike,
     L"and break do else elseif end false for function goto if in local nil not or repeat return then true until "
     L"while",
     L"print pairs ipairs type tostring tonumber require setmetatable getmetatable rawget rawset table string "
     L"math io os coroutine error assert pcall xpcall select next unpack self",
     L"--", nullptr, L"--[[", L"]]", LF_SQSTRING | LF_DEFNAME},
    {46018, L"Makefile", L"Makefile", L"mak mk", LexKind::CLike,
     L"ifeq ifneq ifdef ifndef else endif include define endef export override vpath",
     L"wildcard patsubst subst filter filter-out foreach shell notdir basename addprefix addsuffix dir",
     L"#", nullptr, nullptr, nullptr, LF_DOLLARVAR | LF_NOSQ | LF_COLONKEY | LF_NOESCAPE},
    {46400, L"Markdown", L"Markdown file", L"md markdown mdown mkd", LexKind::Markdown, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, 0},
    {46011, L"Pascal", L"Pascal source file", L"pas pp p inc lpr dpr", LexKind::CLike,
     L"and array as asm begin case class const constructor destructor div do downto else end except exports file "
     L"finalization finally for function goto if implementation in inherited initialization inline interface is "
     L"label library mod nil not object of on or out packed procedure program property raise record repeat "
     L"resourcestring set shl shr string then threadvar to try type unit until uses var while with xor true false",
     L"integer cardinal byte word longint int64 real double single extended boolean char pchar string writeln "
     L"readln write read",
     L"//", nullptr, L"{", L"}", LF_CASEI | LF_SQSTRING | LF_NOESCAPE},
    {46013, L"Perl", L"Perl source file", L"pl pm plx", LexKind::CLike,
     L"my our local sub if elsif else unless while until for foreach do last next redo return use no require "
     L"package print printf say die warn eval defined undef ref scalar shift push pop unshift splice keys values "
     L"each exists delete join split map grep sort reverse open close binmode chomp chop lc uc length substr "
     L"index sprintf qw qq and or not eq ne lt gt le ge cmp",
     nullptr, L"#", nullptr, nullptr, nullptr, LF_DOLLARVAR | LF_SQSTRING | LF_DEFNAME},
    {46008, L"PHP", L"PHP Hypertext Preprocessor file", L"php php3 php4 php5 phps phpt phtml", LexKind::CLike,
     L"abstract and array as break callable case catch class clone const continue declare default do echo else "
     L"elseif empty enddeclare endfor endforeach endif endswitch endwhile enum eval exit extends final finally fn "
     L"for foreach function global goto if implements include include_once instanceof insteadof interface isset "
     L"list match namespace new or print private protected public readonly require require_once return static "
     L"switch throw trait try unset use var while xor yield true false null php",
     L"int float bool string void mixed never object iterable self parent",
     L"//", L"#", L"/*", L"*/", LF_SQSTRING | LF_DOLLARVAR | LF_CASEI | LF_DEFNAME},
    {46053, L"PowerShell", L"Windows PowerShell", L"ps1 psm1 psd1", LexKind::CLike,
     L"begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally for "
     L"foreach from function hidden if in param process return static switch throw trap try until using var while "
     L"workflow -and -or -not -xor -eq -ne -gt -ge -lt -le -like -notlike -match -notmatch -contains -notcontains "
     L"-in -notin -replace -split -join -is -isnot -as -band -bor -bxor -f",
     L"write-host write-output write-error write-warning get-childitem get-content set-content add-content "
     L"get-item set-item remove-item copy-item move-item new-item test-path join-path split-path get-process "
     L"stop-process start-process invoke-webrequest invoke-restmethod select-object where-object foreach-object "
     L"sort-object measure-object group-object out-file out-null convertto-json convertfrom-json get-command",
     L"#", nullptr, L"<#", L"#>", LF_CASEI | LF_DOLLARVAR | LF_SQSTRING | LF_DASHID | LF_NOESCAPE | LF_DEFNAME},
    {46035, L"Properties", L"Properties file", L"properties", LexKind::Ini, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, 0},
    {46012, L"Python", L"Python file", L"py pyw pyi pyx", LexKind::CLike,
     L"False None True and as assert async await break class continue def del elif else except finally for from "
     L"global if import in is lambda nonlocal not or pass raise return try while with yield match case",
     L"abs all any bin bool bytearray bytes callable chr classmethod compile complex dict dir divmod enumerate "
     L"eval exec filter float format frozenset getattr globals hasattr hash help hex id input int isinstance "
     L"issubclass iter len list locals map max min next object oct open ord pow print property range repr "
     L"reversed round set setattr slice sorted staticmethod str sum super tuple type vars zip self cls",
     L"#", nullptr, nullptr, nullptr, LF_TRIPLE | LF_SQSTRING | LF_DECORATOR | LF_DEFNAME},
    {46079, L"Registry", L"Registry file", L"reg", LexKind::Ini, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, 0},
    {46037, L"Ruby", L"Ruby file", L"rb rbw rake gemspec", LexKind::CLike,
     L"BEGIN END alias and begin break case class def defined do else elsif end ensure false for if in module next "
     L"nil not or redo rescue retry return self super then true undef unless until when while yield",
     L"require require_relative include extend attr_accessor attr_reader attr_writer puts print p lambda proc "
     L"raise loop",
     L"#", nullptr, L"=begin", L"=end", LF_SQSTRING | LF_DEFNAME | LF_DOLLARVAR},
    {46080, L"Rust", L"Rust file", L"rs", LexKind::CLike,
     L"as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod "
     L"move mut pub ref return self Self static struct super trait true type unsafe use where while macro_rules "
     L"union",
     L"bool char str u8 u16 u32 u64 u128 usize i8 i16 i32 i64 i128 isize f32 f64 String Vec Option Some None "
     L"Result Ok Err Box Rc Arc RefCell HashMap HashSet BTreeMap",
     L"//", nullptr, L"/*", L"*/", LF_DEFNAME},
    {46027, L"Shell", L"Unix script file", L"sh bsh bash zsh ksh", LexKind::CLike,
     L"if then else elif fi case esac for while until do done in function select time return exit break continue "
     L"local export readonly declare typeset unset shift source alias echo printf read cd pwd test eval exec trap "
     L"set true false",
     nullptr, L"#", nullptr, nullptr, nullptr, LF_DOLLARVAR | LF_SQSTRING | LF_BACKTICK},
    {46020, L"SQL", L"Structured Query Language file", L"sql", LexKind::CLike,
     L"add all alter and any as asc begin between by cascade case check close column commit constraint contains "
     L"continue convert create cross current_date current_time current_timestamp cursor database deallocate "
     L"declare default delete desc distinct drop else end escape except exec execute exists exit fetch for "
     L"foreign from full function goto grant group having identity if in index inner insert intersect into is "
     L"join key left like limit merge not null nullif of off offset on open or order outer over primary print "
     L"procedure references replace return returns revoke right rollback select set some table then to top "
     L"transaction trigger truncate union unique update use values view when where while with true false",
     L"int integer bigint smallint tinyint bit decimal numeric float real money date datetime datetime2 time "
     L"timestamp char varchar nchar nvarchar text ntext binary varbinary uniqueidentifier xml boolean serial blob "
     L"clob count sum avg min max coalesce cast",
     L"--", nullptr, L"/*", L"*/", LF_CASEI | LF_SQSTRING | LF_NOESCAPE},
    {46063, L"Swift", L"Swift file", L"swift", LexKind::CLike,
     L"associatedtype class deinit enum extension fileprivate func import init inout internal let open operator "
     L"private protocol public rethrows static struct subscript typealias var break case continue default defer do "
     L"else fallthrough for guard if in repeat return switch where while as catch false is nil super self Self "
     L"throw throws true try async await actor some any",
     L"Int Int8 Int16 Int32 Int64 UInt Double Float Bool String Character Array Dictionary Set Optional Void Any",
     L"//", nullptr, L"/*", L"*/", LF_DECORATOR | LF_DEFNAME | LF_TRIPLE | LF_NOSQ},
    {46091, L"TOML", L"Tom's Obvious Minimal Language file", L"toml", LexKind::Ini, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, 0},
    {46084, L"TypeScript", L"TypeScript file", L"ts tsx mts cts", LexKind::CLike,
     L"break case catch class const continue debugger default delete do else export extends false finally for "
     L"function if import in instanceof let new null return super switch this throw true try typeof var void "
     L"while with yield async await of static get set undefined abstract any as asserts bigint boolean declare "
     L"enum implements infer interface is keyof module namespace never number object private protected public "
     L"readonly satisfies string symbol type unique unknown",
     L"Array Boolean Date Error Function JSON Map Math Number Object Promise Record Partial Readonly RegExp Set "
     L"String Symbol console window document",
     L"//", nullptr, L"/*", L"*/", LF_BACKTICK | LF_SQSTRING | LF_DECORATOR},
    {46021, L"Visual Basic", L"Visual Basic file", L"vb vbs bas frm cls", LexKind::CLike,
     L"addhandler addressof alias and andalso as boolean byref byte byval call case catch cbool cbyte cchar cdate "
     L"cdbl cdec char cint class clng cobj const continue cshort csng cstr ctype date decimal declare default "
     L"delegate dim directcast do double each else elseif end endif enum erase error event exit false finally for "
     L"friend function get gettype global gosub goto handles if implements imports in inherits integer interface "
     L"is isnot let lib like long loop me mod module mustinherit mustoverride mybase myclass namespace new next not "
     L"nothing notinheritable notoverridable object of on operator option optional or orelse overloads "
     L"overridable overrides paramarray partial private property protected public raiseevent readonly redim "
     L"removehandler resume return select set shadows shared short single static step stop string structure sub "
     L"synclock then throw to true try trycast typeof using variant wend when while with withevents writeonly xor",
     nullptr, L"'", L"rem ", nullptr, nullptr, LF_CASEI | LF_NOSQ | LF_NOESCAPE},
    {46006, L"XML", L"eXtensible Markup Language file",
     L"xml xaml xsl xslt xsd xul kml svg mxml wsdl xlf xliff gpx plist vcxproj csproj vbproj props targets resx "
     L"manifest nuspec config",
     LexKind::Markup, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0},
    {46049, L"YAML", L"YAML Ain't Markup Language", L"yml yaml", LexKind::CLike,
     L"true false null yes no on off True False Null", nullptr, L"#", nullptr, nullptr, nullptr,
     LF_SQSTRING | LF_COLONKEY},
};

constexpr int kLanguageCount = (int)(sizeof(kLanguages) / sizeof(kLanguages[0]));

struct KwSets {
    bool built = false;
    std::unordered_set<std::wstring> kw, types;
};
KwSets g_kw[kLanguageCount];

void BuildSet(std::unordered_set<std::wstring>& set, const wchar_t* list, bool lower) {
    if (!list) return;
    const wchar_t* p = list;
    while (*p) {
        while (*p == L' ') p++;
        const wchar_t* s = p;
        while (*p && *p != L' ') p++;
        if (p > s) {
            std::wstring w(s, p - s);
            if (lower)
                for (auto& c : w) c = (wchar_t)towlower(c);
            set.insert(w);
        }
    }
}

KwSets& Kw(int lang) {
    KwSets& k = g_kw[lang];
    if (!k.built) {
        bool lower = kLanguages[lang].flags & LF_CASEI;
        BuildSet(k.kw, kLanguages[lang].keywords, lower);
        BuildSet(k.types, kLanguages[lang].types, lower);
        k.built = true;
    }
    return k;
}

inline bool IsIdStart(wchar_t c) { return c == L'_' || iswalpha(c); }
inline bool IsIdChar(wchar_t c) { return c == L'_' || iswalnum(c); }

bool Match(const std::wstring& s, size_t i, size_t end, const wchar_t* lit) {
    if (!lit || !*lit) return false;
    size_t n = wcslen(lit);
    return i + n <= end && s.compare(i, n, lit) == 0;
}

bool MatchI(const std::wstring& s, size_t i, size_t end, const wchar_t* lit) {
    if (!lit || !*lit) return false;
    size_t n = wcslen(lit);
    if (i + n > end) return false;
    for (size_t k = 0; k < n; k++)
        if (towlower(s[i + k]) != towlower(lit[k])) return false;
    return true;
}

size_t FindI(const std::wstring& s, size_t from, const wchar_t* lit) {
    for (size_t i = from; i < s.size(); i++)
        if (MatchI(s, i, s.size(), lit)) return i;
    return npos;
}

void Fill(std::vector<uint8_t>& st, size_t a, size_t b, uint8_t v) {
    for (size_t k = a; k < b && k < st.size(); k++) st[k] = v;
}

int g_jsLang = -1, g_cssLang = -1;

// States: 0 normal, 1 block comment, 7 doc block comment, 2 """ string, 3 ''' string, 4 `template`.
int LexCLike(int langIdx, const std::wstring& s, size_t begin, size_t end, int state, std::vector<uint8_t>& st) {
    const Language& L = kLanguages[langIdx];
    KwSets& K = Kw(langIdx);
    const unsigned f = L.flags;
    size_t i = begin;
    auto findClose = [&](size_t from, const wchar_t* lit) -> size_t {
        if (!lit) return npos;
        size_t p = s.find(lit, from);
        return (p == npos || p + wcslen(lit) > end) ? npos : p;
    };
    auto scanBacktick = [&](size_t j, bool& closed) {
        closed = false;
        while (j < end) {
            if (s[j] == L'\\') {
                j += 2;
                continue;
            }
            if (s[j] == L'`') {
                closed = true;
                return j + 1;
            }
            j++;
        }
        return end;
    };

    if (state == 1 || state == 7) {
        uint8_t style = state == 7 ? ST_COMMENTDOC : ST_COMMENT;
        size_t p = findClose(i, L.blockEnd);
        if (p == npos) {
            Fill(st, i, end, style);
            return state;
        }
        size_t e = p + wcslen(L.blockEnd);
        Fill(st, i, e, style);
        i = e;
    } else if (state == 2 || state == 3) {
        const wchar_t* q = state == 2 ? L"\"\"\"" : L"'''";
        size_t p = findClose(i, q);
        if (p == npos) {
            Fill(st, i, end, ST_TRIPLE);
            return state;
        }
        Fill(st, i, p + 3, ST_TRIPLE);
        i = p + 3;
    } else if (state == 4) {
        bool closed;
        size_t j = scanBacktick(i, closed);
        if (j > end) j = end;
        Fill(st, i, j, ST_STRING);
        if (!closed) return 4;
        i = j;
    }

    bool lineStart = true;
    std::wstring prevWord;
    while (i < end) {
        wchar_t c = s[i];
        if (c == L' ' || c == L'\t') {
            st[i++] = ST_DEFAULT;
            continue;
        }
        bool atStart = lineStart;
        lineStart = false;

        if ((f & LF_HASHPREPROC) && c == L'#' && atStart) {
            size_t j = i + 1;
            while (j < end && !Match(s, j, end, L"//") && !Match(s, j, end, L"/*")) j++;
            Fill(st, i, j, ST_PREPROC);
            i = j;
            continue;
        }
        if (L.blockStart && Match(s, i, end, L.blockStart) && !(L.blockStart[0] == L'=' && !atStart)) {
            size_t bl = wcslen(L.blockStart);
            bool doc = Match(s, i, end, L"/**") && !Match(s, i, end, L"/**/");
            uint8_t style = doc ? ST_COMMENTDOC : ST_COMMENT;
            size_t p = findClose(i + bl, L.blockEnd);
            if (p == npos) {
                Fill(st, i, end, style);
                return doc ? 7 : 1;
            }
            size_t e = p + wcslen(L.blockEnd);
            Fill(st, i, e, style);
            i = e;
            continue;
        }
        bool lc2 = (f & LF_CASEI) ? MatchI(s, i, end, L.lineComment2) : Match(s, i, end, L.lineComment2);
        if (lc2 && L.lineComment2 && iswalpha(L.lineComment2[0]) && !atStart) lc2 = false; // "rem " only at start
        if (Match(s, i, end, L.lineComment) || lc2) {
            bool doc = Match(s, i, end, L"///") || Match(s, i, end, L"//!");
            Fill(st, i, end, doc ? ST_COMMENTDOC : ST_COMMENT);
            return 0;
        }
        if ((f & LF_TRIPLE) && (Match(s, i, end, L"\"\"\"") || Match(s, i, end, L"'''"))) {
            bool dq = s[i] == L'"';
            size_t p = findClose(i + 3, dq ? L"\"\"\"" : L"'''");
            if (p == npos) {
                Fill(st, i, end, ST_TRIPLE);
                return dq ? 2 : 3;
            }
            Fill(st, i, p + 3, ST_TRIPLE);
            i = p + 3;
            continue;
        }
        if (c == L'`' && (f & LF_BACKTICK)) {
            bool closed;
            size_t j = scanBacktick(i + 1, closed);
            if (j > end) j = end;
            Fill(st, i, j, ST_STRING);
            if (!closed) return 4;
            i = j;
            continue;
        }
        if (c == L'"' || (c == L'\'' && !(f & LF_NOSQ))) {
            bool isChar = c == L'\'' && !(f & LF_SQSTRING);
            size_t limit = isChar ? std::min(end, i + 14) : end;
            size_t j = i + 1;
            bool closed = false;
            while (j < limit) {
                if (s[j] == L'\\' && !(f & LF_NOESCAPE)) {
                    j += 2;
                    continue;
                }
                if (s[j] == c) {
                    if ((f & LF_NOESCAPE) && j + 1 < limit && s[j + 1] == c) {
                        j += 2;
                        continue;
                    }
                    j++;
                    closed = true;
                    break;
                }
                j++;
            }
            if (j > end) j = end;
            if (isChar) {
                bool escaped = i + 1 < end && s[i + 1] == L'\\';
                if (!closed || (!escaped && j - i > 4)) { // Rust lifetime or a stray quote
                    st[i++] = ST_OPERATOR;
                    continue;
                }
                Fill(st, i, j, ST_CHAR);
                i = j;
                continue;
            }
            uint8_t style = ST_STRING;
            if (f & (LF_JSONKEY | LF_COLONKEY)) {
                size_t k = j;
                while (k < end && (s[k] == L' ' || s[k] == L'\t')) k++;
                if (k < end && s[k] == L':') style = ST_KEY;
            }
            Fill(st, i, j, style);
            i = j;
            continue;
        }
        if (iswdigit(c) || (c == L'.' && i + 1 < end && iswdigit(s[i + 1]))) {
            size_t j = i;
            if (c == L'0' && j + 1 < end && (s[j + 1] == L'x' || s[j + 1] == L'X')) {
                j += 2;
                while (j < end && (iswxdigit(s[j]) || s[j] == L'_')) j++;
            } else {
                while (j < end) {
                    wchar_t d = s[j];
                    if (iswdigit(d) || d == L'.' || d == L'_') {
                        j++;
                    } else if ((d == L'e' || d == L'E') && j + 1 < end &&
                               (iswdigit(s[j + 1]) ||
                                ((s[j + 1] == L'+' || s[j + 1] == L'-') && j + 2 < end && iswdigit(s[j + 2])))) {
                        j += 2;
                    } else {
                        break;
                    }
                }
            }
            while (j < end && IsIdChar(s[j])) j++; // suffixes: u, L, f, i32, px, em, %...
            if (j < end && s[j] == L'%' && (f & LF_CSSHEX)) j++;
            Fill(st, i, j, ST_NUMBER);
            i = j;
            continue;
        }
        if (c == L'$' && (f & LF_DOLLARVAR)) {
            size_t j = i + 1;
            if (j < end && (s[j] == L'{' || s[j] == L'(')) {
                wchar_t close = s[j] == L'{' ? L'}' : L')';
                size_t p = s.find(close, j);
                j = (p == npos || p >= end) ? end : p + 1;
            } else if (j < end && wcschr(L"?@#*!$0123456789", s[j]) && s[j]) {
                j++;
            } else {
                while (j < end && (IsIdChar(s[j]) || (s[j] == L':' && j + 1 < end && s[j + 1] == L':'))) j++;
            }
            if (j > i + 1) {
                Fill(st, i, j, ST_VARIABLE);
                i = j;
                continue;
            }
        }
        if (c == L'@' && (f & LF_DECORATOR) && i + 1 < end && IsIdStart(s[i + 1])) {
            size_t j = i + 1;
            while (j < end && (IsIdChar(s[j]) || s[j] == L'.')) j++;
            Fill(st, i, j, ST_DECORATOR);
            i = j;
            continue;
        }
        if (c == L'#' && (f & LF_CSSHEX)) {
            size_t j = i + 1;
            while (j < end && iswxdigit(s[j])) j++;
            size_t n = j - i - 1;
            if ((n == 3 || n == 4 || n == 6 || n == 8) && (j >= end || !IsIdChar(s[j]))) {
                Fill(st, i, j, ST_NUMBER);
                i = j;
                continue;
            }
        }
        bool dashId = (f & LF_DASHID) && c == L'-' && i + 1 < end && IsIdStart(s[i + 1]) &&
                      (i == begin || !IsIdChar(s[i - 1]));
        if (IsIdStart(c) || dashId) {
            size_t j = i + 1;
            while (j < end && (IsIdChar(s[j]) || ((f & LF_DASHID) && s[j] == L'-' && j + 1 < end && IsIdChar(s[j + 1]))))
                j++;
            std::wstring key = s.substr(i, j - i);
            if (f & LF_CASEI)
                for (auto& ch : key) ch = (wchar_t)towlower(ch);
            uint8_t style = ST_DEFAULT;
            if (f & LF_DEFNAME) {
                if (prevWord == L"def" || prevWord == L"fn" || prevWord == L"func" || prevWord == L"function" ||
                    prevWord == L"sub")
                    style = ST_DEFNAME;
                else if (prevWord == L"class" || prevWord == L"struct" || prevWord == L"module")
                    style = ST_CLASSNAME;
            }
            if (style == ST_DEFAULT) {
                if (K.kw.count(key))
                    style = ST_KEYWORD;
                else if (K.types.count(key))
                    style = ST_TYPE;
            }
            if (style == ST_DEFAULT && (f & LF_COLONKEY)) {
                size_t k = j;
                while (k < end && (s[k] == L' ' || s[k] == L'\t')) k++;
                if (k < end && s[k] == L':' && (k + 1 >= end || s[k + 1] == L' ' || s[k + 1] == L'\t'))
                    style = ST_KEY;
            }
            Fill(st, i, j, style);
            prevWord = key;
            i = j;
            continue;
        }
        st[i] = (c && wcschr(L"+-*/%=<>!&|^~?:;,.()[]{}\\@#", c)) ? ST_OPERATOR : ST_DEFAULT;
        prevWord.clear();
        i++;
    }
    return 0;
}

// States: 0 text, 1 <!-- comment, 2 inside a tag, 3 CDATA, 4 inside <script ...>, 5 inside <style ...>,
// 1000+n script body (n = JavaScript state), 2000+n style body (n = CSS state).
int LexMarkup(int langIdx, const std::wstring& s, int state, std::vector<uint8_t>& st) {
    const bool html = kLanguages[langIdx].flags & LF_HTML;
    size_t i = 0, n = s.size();
    auto isNameChar = [](wchar_t c) { return IsIdChar(c) || c == L'-' || c == L':' || c == L'.'; };
    while (i < n) {
        if (state >= 1000) {
            bool script = state < 2000;
            int inner = state % 1000;
            size_t close = FindI(s, i, script ? L"</script" : L"</style");
            size_t segEnd = close == npos ? n : close;
            int lang = script ? g_jsLang : g_cssLang;
            if (lang >= 0) inner = LexCLike(lang, s, i, segEnd, inner, st);
            if (close == npos) return (script ? 1000 : 2000) + inner;
            i = close;
            state = 0;
            continue;
        }
        if (state == 1 || state == 3) {
            const wchar_t* endTok = state == 1 ? L"-->" : L"]]>";
            uint8_t style = state == 1 ? ST_COMMENT : ST_STRING;
            size_t p = s.find(endTok, i);
            if (p == npos) {
                Fill(st, i, n, style);
                return state;
            }
            Fill(st, i, p + 3, style);
            i = p + 3;
            state = 0;
            continue;
        }
        if (state == 2 || state == 4 || state == 5) {
            wchar_t c = s[i];
            if (c == L' ' || c == L'\t') {
                st[i++] = ST_DEFAULT;
            } else if (c == L'>') {
                st[i++] = ST_TAG;
                state = state == 4 ? 1000 : (state == 5 ? 2000 : 0);
            } else if ((c == L'/' || c == L'?') && i + 1 < n && s[i + 1] == L'>') {
                st[i] = st[i + 1] = ST_TAG;
                i += 2;
                state = 0;
            } else if (c == L'"' || c == L'\'') {
                size_t p = s.find(c, i + 1);
                size_t e = p == npos ? n : p + 1;
                Fill(st, i, e, ST_ATTRVAL);
                i = e;
            } else if (c == L'=') {
                st[i++] = ST_OPERATOR;
            } else if (isNameChar(c)) {
                size_t j = i;
                while (j < n && isNameChar(s[j])) j++;
                Fill(st, i, j, ST_ATTR);
                i = j;
            } else {
                st[i++] = ST_DEFAULT;
            }
            continue;
        }
        wchar_t c = s[i];
        if (c == L'<') {
            if (Match(s, i, n, L"<!--")) {
                Fill(st, i, i + 4, ST_COMMENT);
                i += 4;
                state = 1;
                continue;
            }
            if (Match(s, i, n, L"<![CDATA[")) {
                Fill(st, i, i + 9, ST_TAG);
                i += 9;
                state = 3;
                continue;
            }
            size_t j = i + 1;
            bool closing = j < n && s[j] == L'/';
            if (j < n && (s[j] == L'/' || s[j] == L'!' || s[j] == L'?')) j++;
            size_t nameStart = j;
            while (j < n && isNameChar(s[j])) j++;
            if (j == nameStart) {
                st[i++] = ST_DEFAULT;
                continue;
            }
            Fill(st, i, j, ST_TAG);
            std::wstring name = s.substr(nameStart, j - nameStart);
            for (auto& ch : name) ch = (wchar_t)towlower(ch);
            state = 2;
            if (html && !closing) {
                if (name == L"script")
                    state = 4;
                else if (name == L"style")
                    state = 5;
            }
            i = j;
            continue;
        }
        if (c == L'&') {
            size_t j = i + 1;
            while (j < n && j < i + 12 && (IsIdChar(s[j]) || s[j] == L'#')) j++;
            if (j < n && s[j] == L';' && j > i + 1) {
                Fill(st, i, j + 1, ST_ENTITY);
                i = j + 1;
                continue;
            }
        }
        st[i++] = ST_DEFAULT;
    }
    return state;
}

int LexIni(const std::wstring& s, std::vector<uint8_t>& st) {
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == L' ' || s[i] == L'\t')) i++;
    if (i >= n) return 0;
    if (s[i] == L';' || s[i] == L'#' || s[i] == L'!') {
        Fill(st, i, n, ST_COMMENT);
        return 0;
    }
    if (s[i] == L'[') {
        size_t p = s.find(L']', i);
        size_t e = p == npos ? n : p + 1;
        Fill(st, i, e, ST_SECTION);
        return 0;
    }
    size_t eq = s.find_first_of(L"=:", i);
    if (eq == npos) return 0;
    Fill(st, i, eq, ST_KEY);
    st[eq] = ST_OPERATOR;
    size_t v = eq + 1;
    while (v < n && (s[v] == L' ' || s[v] == L'\t')) v++;
    if (v >= n) return 0;
    std::wstring val = s.substr(v);
    while (!val.empty() && (val.back() == L' ' || val.back() == L'\t')) val.pop_back();
    if (val[0] == L'"' || val[0] == L'\'') {
        Fill(st, v, n, ST_STRING);
    } else if (val == L"true" || val == L"false") {
        Fill(st, v, v + val.size(), ST_KEYWORD);
    } else {
        bool num = !val.empty();
        for (wchar_t c : val)
            if (!iswdigit(c) && c != L'.' && c != L'-' && c != L'+' && c != L'_' && c != L'x') num = false;
        Fill(st, v, n, num ? ST_NUMBER : ST_DEFAULT);
    }
    return 0;
}

int LexBatch(int langIdx, const std::wstring& s, std::vector<uint8_t>& st) {
    KwSets& K = Kw(langIdx);
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == L' ' || s[i] == L'\t')) i++;
    if (i < n && s[i] == L'@') st[i++] = ST_OPERATOR;
    if (MatchI(s, i, n, L"rem") && (i + 3 == n || s[i + 3] == L' ' || s[i + 3] == L'\t')) {
        Fill(st, i, n, ST_COMMENT);
        return 0;
    }
    if (Match(s, i, n, L"::")) {
        Fill(st, i, n, ST_COMMENT);
        return 0;
    }
    if (i < n && s[i] == L':') {
        Fill(st, i, n, ST_LABEL);
        return 0;
    }
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'%') {
            size_t j = i + 1;
            if (j < n && s[j] == L'%') {
                j++;
                if (j < n) j++;
            } else if (j < n && s[j] == L'~') {
                while (j < n && (iswalpha(s[j]) || s[j] == L'~')) j++;
                if (j < n && iswdigit(s[j])) j++;
            } else if (j < n && (iswdigit(s[j]) || s[j] == L'*')) {
                j++;
            } else {
                size_t p = s.find(L'%', j);
                j = p == npos ? i + 1 : p + 1;
            }
            Fill(st, i, j, j > i + 1 ? ST_VARIABLE : ST_OPERATOR);
            i = j;
        } else if (c == L'!' && i + 1 < n && IsIdStart(s[i + 1])) {
            size_t p = s.find(L'!', i + 1);
            size_t j = p == npos ? i + 1 : p + 1;
            Fill(st, i, j, j > i + 1 ? ST_VARIABLE : ST_OPERATOR);
            i = j;
        } else if (IsIdStart(c)) {
            size_t j = i;
            while (j < n && (IsIdChar(s[j]) || s[j] == L'.' || s[j] == L'-')) j++;
            std::wstring key = s.substr(i, j - i);
            for (auto& ch : key) ch = (wchar_t)towlower(ch);
            Fill(st, i, j, K.kw.count(key) ? ST_KEYWORD : ST_DEFAULT);
            i = j;
        } else if (iswdigit(c)) {
            size_t j = i;
            while (j < n && iswdigit(s[j])) j++;
            Fill(st, i, j, ST_NUMBER);
            i = j;
        } else {
            st[i++] = (c && wcschr(L"&|<>()=", c)) ? ST_OPERATOR : ST_DEFAULT;
        }
    }
    return 0;
}

int LexDiff(const std::wstring& s, std::vector<uint8_t>& st) {
    uint8_t style = ST_DEFAULT;
    if (Match(s, 0, s.size(), L"+++") || Match(s, 0, s.size(), L"---") || Match(s, 0, s.size(), L"diff ") ||
        Match(s, 0, s.size(), L"index ") || Match(s, 0, s.size(), L"Index: "))
        style = ST_DIFFHDR;
    else if (Match(s, 0, s.size(), L"@@"))
        style = ST_PREPROC;
    else if (!s.empty() && (s[0] == L'+' || s[0] == L'>'))
        style = ST_DIFFADD;
    else if (!s.empty() && (s[0] == L'-' || s[0] == L'<'))
        style = ST_DIFFDEL;
    Fill(st, 0, s.size(), style);
    return 0;
}

// States: 0 normal, 1 inside a fenced code block.
int LexMarkdown(const std::wstring& s, int state, std::vector<uint8_t>& st) {
    size_t n = s.size(), i = 0;
    while (i < n && i < 4 && s[i] == L' ') i++;
    if (Match(s, i, n, L"```") || Match(s, i, n, L"~~~")) {
        Fill(st, 0, n, ST_CODE);
        return state == 1 ? 0 : 1;
    }
    if (state == 1) {
        Fill(st, 0, n, ST_CODE);
        return 1;
    }
    if (i >= 4 || (n > 0 && s[0] == L'\t')) {
        Fill(st, 0, n, ST_CODE);
        return 0;
    }
    if (i < n && s[i] == L'#') {
        Fill(st, 0, n, ST_HEADING);
        return 0;
    }
    if (i < n && s[i] == L'>') {
        Fill(st, 0, n, ST_COMMENT);
        return 0;
    }
    if (i + 2 < n && (s[i] == L'-' || s[i] == L'*' || s[i] == L'_' || s[i] == L'=')) {
        bool rule = true;
        for (size_t k = i; k < n; k++)
            if (s[k] != s[i] && s[k] != L' ') rule = false;
        if (rule) {
            Fill(st, 0, n, ST_OPERATOR);
            return 0;
        }
    }
    if (i + 1 < n && (s[i] == L'-' || s[i] == L'*' || s[i] == L'+') && s[i + 1] == L' ') {
        st[i] = ST_OPERATOR;
        i += 2;
    } else {
        size_t j = i;
        while (j < n && iswdigit(s[j])) j++;
        if (j > i && j + 1 < n && (s[j] == L'.' || s[j] == L')') && s[j + 1] == L' ') {
            Fill(st, i, j + 1, ST_OPERATOR);
            i = j + 2;
        }
    }
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'`') {
            size_t p = s.find(L'`', i + 1);
            if (p != npos) {
                Fill(st, i, p + 1, ST_CODE);
                i = p + 1;
                continue;
            }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n) {
            bool dbl = s[i + 1] == c;
            const wchar_t tok[3] = {c, c, 0};
            size_t p = dbl ? s.find(tok, i + 2) : s.find(c, i + 1);
            if (p != npos && p > i + (dbl ? 2 : 1)) {
                size_t e = p + (dbl ? 2 : 1);
                Fill(st, i, e, ST_EMPHASIS);
                i = e;
                continue;
            }
        }
        if (c == L'[') {
            size_t close = s.find(L"](", i);
            size_t paren = close == npos ? npos : s.find(L')', close);
            if (paren != npos) {
                Fill(st, i, close + 1, ST_ATTR);
                Fill(st, close + 1, paren + 1, ST_ATTRVAL);
                i = paren + 1;
                continue;
            }
        }
        st[i++] = ST_DEFAULT;
    }
    return 0;
}

constexpr COLORREF Hex(unsigned v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

Theme MakeLight() {
    Theme t{};
    // Global colours from stylers.model.xml (finding #9).
    t.bg = Hex(0xFFFFFF);
    t.fg = Hex(0x000000);
    t.curLine = Hex(0xE8E8FF);
    t.selBg = Hex(0xC0C0C0);
    t.caret = Hex(0x8000FF);
    t.lineNumFg = Hex(0x808080);
    t.lineNumBg = Hex(0xE4E4E4);
    t.bookmarkBg = Hex(0xE0E0E0);
    t.bookmark = Hex(0x2E6BD8);
    t.whitespace = Hex(0xFFB56A);
    t.indentGuide = Hex(0xC0C0C0);
    t.smart = Hex(0x00FF00);
    t.findMark = Hex(0xFF0000);
    t.braceFg = Hex(0xFF0000);
    t.badBraceFg = Hex(0x800000);
    t.eolBoxBg = Hex(0xA0A0A0);
    t.eolBoxFg = Hex(0xFFFFFF);
    auto& s = t.styles;
    s[ST_DEFAULT] = {Hex(0x000000), false, false};
    s[ST_COMMENT] = {Hex(0x008000), false, false};
    s[ST_COMMENTDOC] = {Hex(0x008080), false, false};
    s[ST_KEYWORD] = {Hex(0x0000FF), true, false};
    s[ST_TYPE] = {Hex(0x8000FF), false, false};
    s[ST_NUMBER] = {Hex(0xFF8000), false, false};
    s[ST_STRING] = {Hex(0x808080), false, false};
    s[ST_CHAR] = {Hex(0x808080), false, false};
    s[ST_OPERATOR] = {Hex(0x000080), true, false};
    s[ST_PREPROC] = {Hex(0x804000), false, false};
    s[ST_TRIPLE] = {Hex(0xFF8000), false, false};
    s[ST_DEFNAME] = {Hex(0xFF00FF), false, false};
    s[ST_CLASSNAME] = {Hex(0x000000), true, false};
    s[ST_BUILTIN] = {Hex(0x880088), true, false};
    s[ST_DECORATOR] = {Hex(0xFF8000), false, true};
    s[ST_TAG] = {Hex(0x0000FF), false, false};
    s[ST_ATTR] = {Hex(0xFF0000), false, false};
    s[ST_ATTRVAL] = {Hex(0x8000FF), false, false};
    s[ST_ENTITY] = {Hex(0x800080), false, true};
    s[ST_SECTION] = {Hex(0x8000FF), true, false};
    s[ST_KEY] = {Hex(0x0000FF), false, false};
    s[ST_VARIABLE] = {Hex(0xFF8000), false, false};
    s[ST_LABEL] = {Hex(0x8000FF), true, false};
    s[ST_HEADING] = {Hex(0x0000FF), true, false};
    s[ST_EMPHASIS] = {Hex(0x800080), true, false};
    s[ST_CODE] = {Hex(0x808080), false, false};
    s[ST_DIFFADD] = {Hex(0x008000), false, false};
    s[ST_DIFFDEL] = {Hex(0xFF0000), false, false};
    s[ST_DIFFHDR] = {Hex(0x804000), true, false};
    return t;
}

Theme MakeDark() {
    Theme t{};
    // Global colours from themes/DarkModeDefault.xml (finding #10).
    t.bg = Hex(0x3F3F3F);
    t.fg = Hex(0xDCDCCC);
    t.curLine = Hex(0x101010);
    t.selBg = Hex(0x585858);
    t.caret = Hex(0x8FAF9F);
    t.lineNumFg = Hex(0x8A8A8A);
    t.lineNumBg = Hex(0x0C0C0C);
    t.bookmarkBg = Hex(0x0C0C0C);
    t.bookmark = Hex(0x5A9BEF);
    t.whitespace = Hex(0x5F5F5F);
    t.indentGuide = Hex(0x4F5F5F);
    t.smart = Hex(0x358A35);
    t.findMark = Hex(0xFF0080);
    t.braceFg = Hex(0xF0F9F9);
    t.badBraceFg = Hex(0xF09F9F);
    t.eolBoxBg = Hex(0x5F5F5F);
    t.eolBoxFg = Hex(0xDCDCCC);
    auto& s = t.styles;
    s[ST_DEFAULT] = {Hex(0xDCDCCC), false, false};
    s[ST_COMMENT] = {Hex(0x7F9F7F), false, false};
    s[ST_COMMENTDOC] = {Hex(0x7F9F7F), false, false};
    s[ST_KEYWORD] = {Hex(0xDFC47D), true, false};
    s[ST_TYPE] = {Hex(0xCEDF99), false, false};
    s[ST_NUMBER] = {Hex(0x8CD0D3), false, false};
    s[ST_STRING] = {Hex(0xCC9393), false, false};
    s[ST_CHAR] = {Hex(0xDCA3A3), false, false};
    s[ST_OPERATOR] = {Hex(0x9F9D6D), false, false};
    s[ST_PREPROC] = {Hex(0xFFCFAF), false, false};
    s[ST_TRIPLE] = {Hex(0xCC9393), false, false};
    s[ST_DEFNAME] = {Hex(0xEFEF8F), false, false};
    s[ST_CLASSNAME] = {Hex(0xEFEF8F), true, false};
    s[ST_BUILTIN] = {Hex(0xCEDF99), true, false};
    s[ST_DECORATOR] = {Hex(0xFFCFAF), false, true};
    s[ST_TAG] = {Hex(0xE3CEAB), false, false};
    s[ST_ATTR] = {Hex(0xDFDFBF), false, false};
    s[ST_ATTRVAL] = {Hex(0xCC9393), false, false};
    s[ST_ENTITY] = {Hex(0xCEDF99), false, true};
    s[ST_SECTION] = {Hex(0xDFC47D), true, false};
    s[ST_KEY] = {Hex(0xDFDFBF), false, false};
    s[ST_VARIABLE] = {Hex(0xFFCFAF), false, false};
    s[ST_LABEL] = {Hex(0xDFC47D), true, false};
    s[ST_HEADING] = {Hex(0xDFC47D), true, false};
    s[ST_EMPHASIS] = {Hex(0xEFEF8F), true, false};
    s[ST_CODE] = {Hex(0xCC9393), false, false};
    s[ST_DIFFADD] = {Hex(0xCEDF99), false, false};
    s[ST_DIFFDEL] = {Hex(0xF09F9F), false, false};
    s[ST_DIFFHDR] = {Hex(0xFFCFAF), true, false};
    return t;
}

} // namespace

int LanguageCount() { return kLanguageCount; }

const Language& GetLanguage(int index) {
    if (index < 0 || index >= kLanguageCount) index = 0;
    return kLanguages[index];
}

int LanguageByMenuId(int id) {
    for (int i = 0; i < kLanguageCount; i++)
        if (kLanguages[i].menuId == id) return i;
    return -1;
}

int LanguageByName(const std::wstring& name) {
    for (int i = 0; i < kLanguageCount; i++)
        if (_wcsicmp(kLanguages[i].name, name.c_str()) == 0) return i;
    return -1;
}

int LanguageForPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring file = slash == npos ? path : path.substr(slash + 1);
    for (auto& c : file) c = (wchar_t)towlower(c);
    if (file == L"makefile" || file == L"gnumakefile") return LanguageByName(L"Makefile");
    if (file == L"cmakelists.txt") return LanguageByName(L"CMake");
    if (file == L".bashrc" || file == L".profile" || file == L".zshrc") return LanguageByName(L"Shell");
    size_t dot = file.rfind(L'.');
    if (dot == npos) return 0;
    std::wstring ext = file.substr(dot + 1);
    for (int i = 1; i < kLanguageCount; i++) {
        const wchar_t* p = kLanguages[i].exts;
        while (p && *p) {
            const wchar_t* s = p;
            while (*p && *p != L' ') p++;
            if ((size_t)(p - s) == ext.size() && ext.compare(0, ext.size(), s, p - s) == 0) return i;
            while (*p == L' ') p++;
        }
    }
    return 0;
}

int LexLine(int lang, const std::wstring& text, int state, std::vector<uint8_t>& styles) {
    if (g_jsLang < 0) {
        g_jsLang = LanguageByName(L"JavaScript");
        g_cssLang = LanguageByName(L"CSS");
    }
    styles.assign(text.size(), ST_DEFAULT);
    if (lang < 0 || lang >= kLanguageCount) return 0;
    switch (kLanguages[lang].kind) {
    case LexKind::CLike: return LexCLike(lang, text, 0, text.size(), state, styles);
    case LexKind::Markup: return LexMarkup(lang, text, state, styles);
    case LexKind::Ini: return LexIni(text, styles);
    case LexKind::Batch: return LexBatch(lang, text, styles);
    case LexKind::Diff: return LexDiff(text, styles);
    case LexKind::Markdown: return LexMarkdown(text, state, styles);
    case LexKind::None: break;
    }
    return 0;
}

const Theme& GetTheme(bool dark) {
    static const Theme light = MakeLight();
    static const Theme darkTheme = MakeDark();
    return dark ? darkTheme : light;
}
