# NovaChangJie

[正體中文](README.zh-TW.md)

NovaChangJie is a free ChangJie input method for Windows 10 and 11. It brings
back the features of the old *Microsoft New ChangJie* and works natively on
Windows 11.

The installer adds two keyboards: **NovaChangJie** (ChangJie) and
**Nova Quick** (Quick).

NovaChangJie is an independent project and is not affiliated with Microsoft.

> **Status:** early release. Please try it in a virtual machine first.

## Features

- **ChangJie and Quick:** Cangjie 5 or Cangjie 3 codes (or both together),
  and a separate Quick keyboard.
- **Sentence mode:** type a whole sentence and the characters are chosen for
  you from the context. Press ↓ to change a choice and Enter to send it.
- **Learns as you type:** characters and words you use often come first.
- **Associations:** after you type a word, likely next words are offered.
- **Prediction and wildcard:** see characters whose code starts with what you
  typed, or use `*` when you forget part of a code.
- **Privacy:** password fields get plain English input, and nothing you type
  in a private browser window is learned.
- **Simplified output:** type ChangJie and get Simplified Chinese.
- **User words:** add your own words and shortcuts.
- **Symbols and punctuation:** Chinese punctuation, full-width and half-width
  characters, and symbol codes.
- **Fits Windows 11:** follows light and dark mode, and works in browsers,
  Store apps, games and the logon screen.

## Keys

| Key | While typing a code | Otherwise |
|---|---|---|
| a–z | Add a radical | Start a code |
| Space | Send the highlighted character | Space |
| 1–9 | Pick a candidate | Digit |
| Shift+1–9 | – | Pick a suggested next word |
| ↑ ↓ | Move the highlight | – |
| PgUp/PgDn | Change page | – |
| `*` | Any keys (wildcard) | `*` |
| Enter | Type the letters as English | Enter |
| Esc | Cancel | – |
| Backspace | Delete one radical | Backspace |
| Shift (tap) | Switch Chinese/English | Switch Chinese/English |
| Shift+Space | Full/half width | Full/half width |
| Ctrl+. | Chinese/English punctuation | Chinese/English punctuation |

## Installing

1. Download `NovaChangJie-0.9.2-x64.msi` and run it.
2. Switch to **NovaChangJie** or **Nova Quick** with Win+Space.

Settings are in the Start menu under *NovaChangJie Settings*. To uninstall,
use *Settings → Apps*.

## Building from source

With Visual Studio 2022 and the Windows 11 SDK installed:

```powershell
.\tools\fetch-data.ps1 -Neural
.\build.ps1 -Msi
```

## License

NovaChangJie is licensed under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/):
you can use, change and share it with credit, but not for commercial use, and
changes must be shared under the same licence.

The dictionary and language data come from open sources, which are credited
in `DATA-LICENSES.md` and `TEXT-SOURCES.md` in the installed program:
libcangjie, rime-essay, rime-cantonese, Mozilla Common Voice, the Hong Kong
Cantonese Corpus, Tatoeba, Taiwan's national laws and regulations, and Hong
Kong e-Legislation. The icons use Noto Sans TC.

No Microsoft code or data was used.
