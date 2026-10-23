// Loading and saving documents: BOM/UTF-8 detection, code pages, EOL handling.
#pragma once
#include "document.h"

#include <string>

bool ReadFileBytes(const std::wstring& path, std::string& bytes, std::wstring& err);
bool WriteFileBytes(const std::wstring& path, const std::string& bytes, std::wstring& err);

// Decodes bytes. With detect=true the encoding is detected (BOM, then UTF-8 validity, else ANSI);
// otherwise enc/cp are taken as given.
std::wstring DecodeBytes(const std::string& bytes, Enc& enc, UINT& cp, bool detect);
std::string EncodeText(const std::wstring& text, Enc enc, UINT cp);

void SplitLines(const std::wstring& text, std::vector<std::wstring>& lines, Eol& eol, bool& foundEol);
std::wstring JoinLines(const Document& doc);

// forceEnc < 0 means detect.
bool LoadDocument(const std::wstring& path, Document& doc, std::wstring& err, int forceEnc = -1, UINT forceCp = 0);
bool SaveDocument(const std::wstring& path, Document& doc, std::wstring& err, bool updateDoc = true);

bool GetFileTime(const std::wstring& path, FILETIME& ft);
std::wstring EncodingName(Enc enc, UINT cp);
const wchar_t* EolName(Eol eol);
bool LooksBinary(const std::string& bytes);
