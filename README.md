## An ebook viewer on E-paper
![10.3 Inch EPD + Radxa Zero](doc/demo.jpg)

**Hardware**: 10.3 inch 16-grayscale EPD display with Radxa Zero

**Software**: Debian Bullseye

**Build dependencies**:
- poppler-cpp (PDF rendering)
- sqlite3     (libsqlite3-dev; configuration database)
- freetype2   (file manager text)
- fontconfig  (optional, used to pick system fonts)

Configuration and reading positions are stored in `ereader.db`, created in the
current working directory on first run. It has two tables: `config`, a key/value store
holding the initial `path`, and `book`, which records each book's path, total
page count and the page last read. The current page is saved whenever a page is
loaded successfully.

Run `make` to build. The file manager and the text reader draw UTF-8 text
using the fonts installed on the system. fontconfig picks the preferred
monospace font as the primary face, then one font per common language as a
fallback: Latin, Greek, Cyrillic, Hebrew, Arabic, Persian/Urdu, Devanagari,
Bengali, Gurmukhi, Gujarati, Odia, Tamil, Telugu, Kannada, Malayalam, Sinhala,
Thai, Lao, Tibetan, Myanmar, Khmer, Georgian, Armenian, Ethiopic, Syriac,
Thaana, Tifinagh, N'Ko, Cherokee, Vai, plus Chinese, Japanese, Korean and
common symbols. Without fontconfig the standard font directories
(/usr/share/fonts, ~/.fonts, ...) are scanned instead and the same common
scripts are preferred. Set EREADER_FONT to one or more absolute font file
paths (colon-separated) to use them as the primary faces. The file manager
uses 12 px text and the text reader uses 10 px text (`FBM_FONT_SCALE` scales
both for the target screen).

File manager keys:
- e -- enter the selected folder, open the selected PDF, or read a text file
- q -- go to the parent folder
- j -- select the next entry
- k -- select the previous entry

Text reader keys:
- j -- scroll down half a screen
- k -- scroll up half a screen
- q -- close the text reader

PDF reader keys:
- j -- next page
- k -- previous page
- q -- close the PDF reader

Like the PDF view, the file manager shows the status bar (page buttons, clock,
battery, WiFi) along the bottom edge of the framebuffer. The right edge holds,
from the screen edge inwards, the clock, the WiFi badge and the battery. The
clock is drawn as three vertical columns - year, month-day, hour:minute - with
the characters rotated 90 degrees and written bottom to top, so it fits the
short status bar.
