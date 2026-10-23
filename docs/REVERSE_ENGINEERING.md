# Reverse engineering на Notepad++ (REA) → Tetradkata++

## Цел

Да разберем какво *прави* Notepad++ от доставения бинарен файл и да построим
независима реконструкция — **Tetradkata++** — която повтаря поведението,
командите и изгледа, без да копира кода на Notepad++.

## Обект на анализа

| Поле | Стойност |
|---|---|
| Пакет | `npp.8.9.8.1.portable.x64.zip` (официален GitHub release v8.9.8.1, 24.09.2026) |
| SHA-256 на zip | `BEDDF5548E75C97930D1575BA1C7D113E6E2B33D0A200310E734CA08E225F414` |
| Бинарен файл | `notepad++.exe`, PE32+ x86-64, 8 546 288 байта |
| SHA-256 на exe | `c52c9ed2ec6d0b5b3f243b29f2124f98db56b6ba92bb42a9af2665709464b3cf` |

## Регистър на находките

Тип доказателство: **O** = наблюдавано в бинарния файл (REA), **S** = наблюдавано в
доставените конфигурационни файлове, **I** = извод, **D** = дизайнерско решение в Tetradkata++.

| # | Находка | Тип | Доказателство |
|---|---|---|---|
| 1 | Подписан с Authenticode: subject `NOTEPAD++`, издател GlobalSign GCC R45 CodeSigning CA 2020, валиден 2025-10-09 → 2028-10-09. Подписът е декодиран структурно, **не** е криптографски проверен. | O | `ev_bef6f71cd3f6f89e4e6ef1ca205a2d8ba50e9e4024a690bc66ac79dbddf7837a` |
| 2 | Ресурсно дърво: 2 487 ресурса (покритие `complete`): 2 067 RT_ICON, 302 RT_GROUP_ICON, 70 RT_DIALOG, 35 RT_BITMAP, 4 RT_CURSOR, 4 RT_GROUP_CURSOR, 3 RT_MENU, 1 RT_VERSION, 1 RT_MANIFEST. Няма RT_ACCELERATOR → клавишните комбинации са в кода / `shortcuts.xml`. | O | `ev_1e7844cef2632c7a200bc51ed659a6df8f93aaea4f3d48313cf902e42dd469ae` |
| 3 | Главно меню (ресурс 1500): 15 горни менюта — File, Edit, Search, View, Encoding, Language, Settings, Tools, Macro, Run, Plugins, Window, ? — около 600 командни ID. Пълният декодиран dump е в [`notepad++-menus-and-dialogs.txt`](notepad++-menus-and-dialogs.txt). | O | байтовете на RT_MENU от диапазоните в #2, декодирани с [`decode_pe_menus.py`](decode_pe_menus.py) |
| 4 | Командните ID са групирани по меню: File 41xxx, Edit 42xxx, Search 43xxx, View 44xxx, Encoding 45xxx, Language 46xxx, ? 47xxx, Settings/Tools 48xxx, Run 49xxx, Window 11xxx. Tetradkata++ използва **същите номера**. | O | #3 |
| 5 | Меню 1501 = контекстно меню на иконата в tray; меню 1950 = променливите на Run: `FULL_CURRENT_PATH`, `CURRENT_DIRECTORY`, `FILE_NAME`, `NAME_PART`, `EXT_PART`, `CURRENT_WORD`, `NPP_DIRECTORY`, `NPP_FULL_FILE_PATH`, `CURRENT_LINE`, `CURRENT_COLUMN`, `CURRENT_LINESTR`. | O | #3 |
| 6 | 70 диалога, напр. 1600 Replace (Find/Replace/Find in Files/Mark), 1670 Search results, 1900 Run..., 2000 Go To..., 2020 Column Editor, 2200 Style Configurator, 2600 Shortcut mapper, 6000 Preferences, 7000 Windows, 8000 Run a Macro Multiple Times. | O | #3 |
| 7 | Редакторът е **Scintilla + Lexilla, статично линкнати** (75 ASCII / 7 UTF-16 срещания на "Scintilla", 39 на "Lexilla", няма `SciLexer.dll`). Има boost (regex търсене). | O | сканиране на низове в exe |
| 8 | Конфигурацията е в `config.xml`, `session.xml`, `shortcuts.xml` (UTF-16 низове в exe). Портативен режим се включва от `doLocalConf.xml` до exe-то. | O + S | низове в exe; списък на файловете в zip |
| 9 | Цветове по подразбиране (светла тема): текст 000000/FFFFFF, Courier New; коментар 008000; ключова дума 0000FF **bold**; тип 8000FF; число FF8000; низ 808080; оператор 000080 **bold**; препроцесор 804000; текуща линия E8E8FF; селекция C0C0C0; каретка 8000FF; номера на редове 808080 върху E4E4E4; bookmark поле E0E0E0; whitespace символи FFB56A; smart highlight 00FF00; find mark FF0000; скоби FF0000 bold; лоша скоба 800000; indent guide C0C0C0. Python: triple-quoted низове FF8000, def име FF00FF, клас bold, декоратор FF8000 italic. HTML: таг 0000FF, атрибут FF0000, стойност 8000FF. | S | `stylers.model.xml` |
| 10 | Тъмна тема (`DarkModeDefault.xml`): фон 3F3F3F, текст DCDCCC, коментар 7F9F7F, ключова дума DFC47D, тип CEDF99, число 8CD0D3, низ CC9393, оператор 9F9D6D, препроцесор FFCFAF, текуща линия 101010, селекция 585858, каретка 8FAF9F, номера 8A8A8A/0C0C0C, smart highlight 358A35. | S | `themes/DarkModeDefault.xml` |
| 11 | Ключовите думи за всеки език са в `langs.model.xml` (не в exe). | S | `langs.model.xml` |
| 12 | Notepad++ използва един Scintilla изглед с много буфери (раздели). Tetradkata++ прави същото: един Editor, много `Document`-и. | I | #7 + поведението на табовете |

## Какво е реконструирано и какво не

**Реконструирано (по менютата от #3, със същите ID):** раздели с drag-reorder; нов/отвори/запази/запази като/копие/всички; reload; rename; recycle bin; затваряне (всички, всички без текущия, вляво, вдясно, незапазените); последни файлове и възстановяване на последния затворен; сесия и backup на незапазени документи (#8); undo/redo; line operations (дублиране, местене, сортиране по 5 начина, премахване на дубликати/празни редове, обръщане, разбъркване); смяна на регистъра (7 режима); коментари; EOL конверсия; blank operations; Find/Replace/Count/Mark/Find All/Find in Files (Normal/Extended/Regex); Go To; съвпадащи скоби; bookmarks и операции с тях; smart highlight; zoom; word wrap; символи за whitespace/EOL; indent guides; кодировки ANSI/UTF-8/UTF-8-BOM/UTF-16 LE/BE + кирилица (1251, KOI8-R/U, 866, ISO-8859-5) и др.; ~30 езика с оцветяване; word completion (Ctrl+Enter и докато пишеш); MD5/SHA-1/SHA-256/SHA-512; макроси; Run с променливите от #5; печат; full screen/post-it/always on top; monitoring (tail -f); тъмна тема (#10); single-instance; портативен режим.

**Не е реконструирано (неизвестно или извън обхвата):** плъгини, Style Configurator, Shortcut Mapper, User Defined Language, Function List, Document Map, Project panels, Folder as Workspace, сгъване на код (folding), колонна/множествена селекция, function completion (API файлове), втори изглед (split view), локализация, тъмна тема за менютата/табовете (тъмни са само редакторът и заглавната лента), смесени EOL в един файл (запазва се един вид). Поведение на Scintilla, което не е описано в ресурсите, е **дизайн** (D), а не наблюдение.

## Граници на анализа

- Анализът е **статичен**: Notepad++ не е стартиран. Поведението на командите е изведено от имената/ID в менютата, конфигурационните файлове и публично документираното поведение.
- Не е декомпилиран код; не е копиран код на Notepad++ (GPL). Списъците с ключови думи в Tetradkata++ са написани наново.
- Подписът (#1) не е криптографски проверен.
