# Tetradkata++

Нативен Windows текстов редактор (C++ / Win32), реконструиран от Notepad++ 8.9.8.1
чрез reverse engineering с **REA**. Не използва код на Notepad++ и няма външни
зависимости: собствен editor control, един статично линкнат `.exe`.

Как е направен анализът и кое е наблюдение и кое е дизайнерско решение:
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

## Build

```bat
build.bat
```

Изисква portable llvm-mingw в `toolchain\` (не е в git). Резултатът е `build\Tetradkata++.exe`.
Иконата се генерира с `python tools\make_icon.py`.

## Какво може

- Табове: drag за пренареждане, middle-click затваря, двоен клик на празното място отваря нов документ.
  Иконите са сини (запазен), червени (променен) и сиви (read-only).
- Оцветяване за ~30 езика, включително вграден JavaScript/CSS в HTML.
- Find / Replace / Find in Files / Mark (Normal, Extended, Regex), Count, Find All с панел за резултати (F7, F4).
- Smart highlight, съвпадащи скоби, bookmarks (Ctrl+F2 / F2), word completion (Ctrl+Enter).
- Line operations, сортиране (5 вида), смяна на регистъра (7 режима), коментари, blank operations.
- Кодировки: ANSI, UTF-8, UTF-8-BOM, UTF-16 LE/BE, Windows-1251, KOI8-R/U, OEM 866 и др.; EOL конверсия.
- Сесия и backup: незапазените документи се пазят на всеки 7 секунди и се възстановяват при следващо пускане.
- Макроси, Run с `$(FULL_CURRENT_PATH)` и подобни променливи, MD5/SHA-1/SHA-256/SHA-512, печат.
- Full screen (F11), Post-it (F12), тъмна тема, zoom, word wrap, показване на whitespace/EOL.
- Single instance: повторно пускане с файл го отваря в работещия прозорец (`-multiInst` го изключва).
- Портативен режим: празен `doLocalConf.ini` до exe-то пази настройките там вместо в `%APPDATA%\Tetradkata++`.

## Командни IDs

Менютата използват същите числови ID като Notepad++ (декодирани от RT_MENU ресурса), така че
`docs/notepad++-menus-and-dialogs.txt` е и справочник за командите на Tetradkata++.
