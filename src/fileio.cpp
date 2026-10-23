#include "fileio.h"

static std::wstring LastErrorText() {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, GetLastError(), 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"Unknown error";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s;
}

bool ReadFileBytes(const std::wstring& path, std::string& bytes, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = LastErrorText();
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 0x7FFFFFFF) {
        CloseHandle(h);
        err = L"The file is too large to open.";
        return false;
    }
    bytes.resize((size_t)size.QuadPart);
    DWORD total = 0;
    while (total < bytes.size()) {
        DWORD got = 0;
        if (!ReadFile(h, &bytes[total], (DWORD)(bytes.size() - total), &got, nullptr) || got == 0) break;
        total += got;
    }
    bytes.resize(total);
    CloseHandle(h);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes, std::wstring& err) {
    // Keep the existing file's attributes (hidden/system) the way an in-place save would.
    DWORD attr = GetFileAttributesW(path.c_str());
    DWORD flags = attr == INVALID_FILE_ATTRIBUTES ? FILE_ATTRIBUTE_NORMAL
                                                  : (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           attr == INVALID_FILE_ATTRIBUTES ? CREATE_NEW : TRUNCATE_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = LastErrorText();
        return false;
    }
    DWORD written = 0;
    bool ok = bytes.empty() || (WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
                                written == bytes.size());
    if (!ok) err = LastErrorText();
    CloseHandle(h);
    return ok;
}

static std::wstring FromCodePage(const char* p, size_t n, UINT cp, bool strict, bool* ok) {
    if (ok) *ok = true;
    if (n == 0) return L"";
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int len = MultiByteToWideChar(cp, flags, p, (int)n, nullptr, 0);
    if (len <= 0) {
        if (ok) *ok = false;
        if (strict) return L"";
        return L"";
    }
    std::wstring w(len, L'\0');
    MultiByteToWideChar(cp, flags, p, (int)n, &w[0], len);
    return w;
}

static std::string ToCodePage(const std::wstring& w, UINT cp) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring DecodeBytes(const std::string& bytes, Enc& enc, UINT& cp, bool detect) {
    const unsigned char* b = (const unsigned char*)bytes.data();
    size_t n = bytes.size();
    if (detect) {
        if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
            enc = Enc::UTF8BOM;
        else if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE)
            enc = Enc::UTF16LE;
        else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF)
            enc = Enc::UTF16BE;
        else {
            bool ok = false;
            FromCodePage(bytes.data(), n, CP_UTF8, true, &ok);
            enc = ok ? Enc::UTF8 : Enc::ANSI;
        }
        cp = 0;
    }
    switch (enc) {
    case Enc::UTF8BOM: {
        size_t skip = (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
        return FromCodePage(bytes.data() + skip, n - skip, CP_UTF8, false, nullptr);
    }
    case Enc::UTF8:
        return FromCodePage(bytes.data(), n, CP_UTF8, false, nullptr);
    case Enc::UTF16LE:
    case Enc::UTF16BE: {
        bool be = enc == Enc::UTF16BE;
        size_t skip = 0;
        if (n >= 2 && ((!be && b[0] == 0xFF && b[1] == 0xFE) || (be && b[0] == 0xFE && b[1] == 0xFF))) skip = 2;
        std::wstring w;
        w.reserve((n - skip) / 2);
        for (size_t i = skip; i + 1 < n; i += 2)
            w += be ? (wchar_t)((b[i] << 8) | b[i + 1]) : (wchar_t)(b[i] | (b[i + 1] << 8));
        return w;
    }
    case Enc::CODEPAGE:
        return FromCodePage(bytes.data(), n, cp ? cp : CP_ACP, false, nullptr);
    case Enc::ANSI:
    default:
        return FromCodePage(bytes.data(), n, CP_ACP, false, nullptr);
    }
}

std::string EncodeText(const std::wstring& text, Enc enc, UINT cp) {
    switch (enc) {
    case Enc::UTF8BOM:
        return "\xEF\xBB\xBF" + ToCodePage(text, CP_UTF8);
    case Enc::UTF8:
        return ToCodePage(text, CP_UTF8);
    case Enc::UTF16LE:
    case Enc::UTF16BE: {
        bool be = enc == Enc::UTF16BE;
        std::string s;
        s.reserve(text.size() * 2 + 2);
        s += be ? "\xFE\xFF" : "\xFF\xFE";
        for (wchar_t c : text) {
            char hi = (char)(c >> 8), lo = (char)(c & 0xFF);
            if (be) {
                s += hi;
                s += lo;
            } else {
                s += lo;
                s += hi;
            }
        }
        return s;
    }
    case Enc::CODEPAGE:
        return ToCodePage(text, cp ? cp : CP_ACP);
    case Enc::ANSI:
    default:
        return ToCodePage(text, CP_ACP);
    }
}

void SplitLines(const std::wstring& text, std::vector<std::wstring>& lines, Eol& eol, bool& foundEol) {
    lines.clear();
    foundEol = false;
    size_t start = 0, n = text.size();
    for (size_t i = 0; i < n; i++) {
        wchar_t c = text[i];
        if (c != L'\r' && c != L'\n') continue;
        lines.emplace_back(text, start, i - start);
        bool crlf = c == L'\r' && i + 1 < n && text[i + 1] == L'\n';
        if (!foundEol) {
            eol = crlf ? Eol::CRLF : (c == L'\r' ? Eol::CR : Eol::LF);
            foundEol = true;
        }
        if (crlf) i++;
        start = i + 1;
    }
    lines.emplace_back(text, start, n - start);
}

std::wstring JoinLines(const Document& doc) {
    const wchar_t* sep = doc.eol == Eol::CRLF ? L"\r\n" : (doc.eol == Eol::LF ? L"\n" : L"\r");
    size_t total = 0;
    for (auto& l : doc.lines) total += l.size() + 2;
    std::wstring out;
    out.reserve(total);
    for (size_t i = 0; i < doc.lines.size(); i++) {
        if (i) out += sep;
        out += doc.lines[i];
    }
    return out;
}

bool GetFileTime(const std::wstring& path, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    ft = fad.ftLastWriteTime;
    return true;
}

bool LooksBinary(const std::string& bytes) {
    size_t n = bytes.size() < 8192 ? bytes.size() : 8192;
    if (n >= 2 && (((unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) ||
                   ((unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF)))
        return false;
    for (size_t i = 0; i < n; i++)
        if (bytes[i] == 0) return true;
    return false;
}

bool LoadDocument(const std::wstring& path, Document& doc, std::wstring& err, int forceEnc, UINT forceCp) {
    std::string bytes;
    if (!ReadFileBytes(path, bytes, err)) return false;
    Enc enc = forceEnc < 0 ? Enc::UTF8 : (Enc)forceEnc;
    UINT cp = forceCp;
    std::wstring text = DecodeBytes(bytes, enc, cp, forceEnc < 0);
    Eol eol = doc.eol;
    bool found = false;
    std::vector<std::wstring> lines;
    SplitLines(text, lines, eol, found);
    doc.lines = std::move(lines);
    doc.bookmarks.assign(doc.lines.size(), 0);
    doc.lexState.assign(doc.lines.size() + 1, 0);
    doc.lexValid = 1;
    doc.marks.clear();
    doc.version++;
    if (found) doc.eol = eol;
    doc.enc = enc;
    doc.codepage = cp;
    doc.path = path;
    doc.ClearUndo();
    DWORD attr = GetFileAttributesW(path.c_str());
    doc.fileReadOnly = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY);
    doc.hasFileTime = GetFileTime(path, doc.fileTime);
    doc.caret = doc.Clamp(doc.caret);
    doc.anchor = doc.Clamp(doc.anchor);
    return true;
}

bool SaveDocument(const std::wstring& path, Document& doc, std::wstring& err, bool updateDoc) {
    std::string bytes = EncodeText(JoinLines(doc), doc.enc, doc.codepage);
    if (!WriteFileBytes(path, bytes, err)) return false;
    if (updateDoc) {
        doc.path = path;
        doc.SetSavePoint();
        doc.hasFileTime = GetFileTime(path, doc.fileTime);
    }
    return true;
}

std::wstring EncodingName(Enc enc, UINT cp) {
    switch (enc) {
    case Enc::ANSI: return L"ANSI";
    case Enc::UTF8: return L"UTF-8";
    case Enc::UTF8BOM: return L"UTF-8-BOM";
    case Enc::UTF16BE: return L"UTF-16 BE BOM";
    case Enc::UTF16LE: return L"UTF-16 LE BOM";
    case Enc::CODEPAGE:
        switch (cp) {
        case 1250: return L"Windows-1250";
        case 1251: return L"Windows-1251";
        case 1252: return L"Windows-1252";
        case 1253: return L"Windows-1253";
        case 1254: return L"Windows-1254";
        case 28591: return L"ISO 8859-1";
        case 28592: return L"ISO 8859-2";
        case 28595: return L"ISO 8859-5";
        case 28597: return L"ISO 8859-7";
        case 437: return L"OEM-US";
        case 850: return L"OEM 850";
        case 855: return L"OEM 855";
        case 866: return L"OEM 866";
        case 20866: return L"KOI8-R";
        case 21866: return L"KOI8-U";
        }
        return L"CP " + std::to_wstring(cp);
    }
    return L"";
}

const wchar_t* EolName(Eol eol) {
    switch (eol) {
    case Eol::CRLF: return L"Windows (CR LF)";
    case Eol::LF: return L"Unix (LF)";
    case Eol::CR: return L"Macintosh (CR)";
    }
    return L"";
}
