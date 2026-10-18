// NovaChangJie - 多功能前導字元鍵 (multi-function prefix key), as in
// Microsoft New ChangJie. After ` the next key picks what follows:
//
//   U + hex digits    Unicode code point     `U4E2D  -> 中
//   B + 4 hex digits  Big5 code              `BA4A4  -> 中
//   P + letters       user word shortcut (捷徑) from 使用者造詞
//   punctuation key   its full-width form    `,      -> ，
//
// Space or Enter commits; a 4-digit Big5 code or a 6-digit Unicode value
// commits by itself. Backspace steps back, Esc cancels.
#include <algorithm>

#include "Engine.h"
#include "Unicode.h"

namespace nova {

namespace {

constexpr size_t kMaxUnicodeDigits = 6;
constexpr size_t kMaxBig5Digits = 4;
constexpr size_t kMaxShortcutLetters = kCodeBytes - 1;

int HexValue(const KeyInput& key) {
    if (key.digit >= 0 && !key.shift) return key.digit;
    if (key.letter >= 'a' && key.letter <= 'f') return 10 + (key.letter - 'a');
    return -1;
}

char32_t ParseHex(const std::string& hex) {
    char32_t v = 0;
    for (char c : hex) v = v * 16 + static_cast<char32_t>(c <= '9' ? c - '0' : c - 'A' + 10);
    return v;
}

bool IsCharacter(char32_t cp) {
    return cp >= 0x20 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF) && !(cp >= 0x7F && cp < 0xA0);
}

}  // namespace

std::u16string Engine::PrefixText() const {
    std::u16string out(1, static_cast<char16_t>(kPrefixKey));
    switch (prefix_) {
    case Prefix::Unicode: out += u'U'; break;
    case Prefix::Big5: out += u'B'; break;
    case Prefix::Shortcut: out += u'P'; break;
    default: break;
    }
    return out + Utf8ToUtf16(prefixCode_);
}

void Engine::EnterPrefix(Prefix mode) {
    buffer_.clear();
    association_ = false;
    prefix_ = mode;
    prefixCode_.clear();
    RecomputePrefix();
}

void Engine::RecomputePrefix() {
    candidates_.clear();
    selection_ = 0;
    exactCount_ = 0;
    if (dryRun_) return;
    switch (prefix_) {
    case Prefix::Menu:
        if (options_.prefixHints) {
            const struct {
                const char* key;
                const char16_t* label;
                const char16_t* text;
            } items[] = {
                {"u", u"U", u"Unicode 內碼"},
                {"b", u"B", u"Big5 內碼"},
                {"p", u"P", u"捷徑字元 (使用者造詞)"},
                {"", u"，", u"全形標點：按標點符號鍵"},
            };
            for (const auto& item : items) {
                Candidate c;
                c.label = item.label;
                c.text = item.text;
                c.code = item.key;
                candidates_.push_back(std::move(c));
            }
        }
        break;
    case Prefix::Unicode:
    case Prefix::Big5: {
        if (prefixCode_.empty()) break;
        const bool unicode = prefix_ == Prefix::Unicode;
        if (!unicode && prefixCode_.size() < kMaxBig5Digits) break;
        char32_t value = ParseHex(prefixCode_);
        char32_t cp = unicode ? value : (big5Decoder_ ? big5Decoder_(static_cast<uint16_t>(value)) : 0);
        if (!IsCharacter(cp)) break;
        Candidate c;
        AppendCodePoint(c.text, cp);
        c.source = c.text;
        c.label = (unicode ? u"U+" : u"Big5 ") + Utf8ToUtf16(prefixCode_);
        candidates_.push_back(std::move(c));
        break;
    }
    case Prefix::Shortcut: {
        if (!user_) break;
        std::vector<UserPhrase> phrases = user_->AllUserPhrases();
        std::stable_sort(phrases.begin(), phrases.end(),
                         [](const UserPhrase& a, const UserPhrase& b) { return a.shortcut < b.shortcut; });
        for (const UserPhrase& p : phrases) {
            if (p.shortcut.empty() || p.shortcut.compare(0, prefixCode_.size(), prefixCode_) != 0) continue;
            Candidate c;
            c.text = c.source = p.text;
            c.code = p.shortcut;
            c.kind = Candidate::Kind::UserPhrase;
            if (options_.showCodeHints) c.hint = p.shortcut.substr(prefixCode_.size());
            candidates_.push_back(std::move(c));  // An exact shortcut sorts first.
        }
        break;
    }
    case Prefix::None:
        break;
    }
}

void Engine::CommitPrefix(size_t index, int64_t now, EngineOutput& out) {
    if (index >= candidates_.size()) return;
    if (prefix_ == Prefix::Menu) {
        // A hint row was chosen (mouse): switch to that mode.
        const std::string& key = candidates_[index].code;
        if (key == "u") EnterPrefix(Prefix::Unicode);
        else if (key == "b") EnterPrefix(Prefix::Big5);
        else if (key == "p") EnterPrefix(Prefix::Shortcut);
        return;
    }
    const Candidate c = candidates_[index];
    out.commit += c.text;
    if (Records() && prefix_ == Prefix::Shortcut) user_->Record(c.source, now);
    Reset();
}

EngineOutput Engine::ProcessPrefix(const KeyInput& key, int64_t now) {
    EngineOutput out;
    if (key.ctrl || key.alt) return out;
    out.consumed = true;
    const bool vertical = options_.layout == CandidateLayout::Vertical;
    const long page = static_cast<long>(options_.pageSize);

    switch (key.key) {
    case Key::Escape:
        Reset();
        return out;
    case Key::Backspace:
        if (!prefixCode_.empty()) {
            prefixCode_.pop_back();
            RecomputePrefix();
        } else if (prefix_ != Prefix::Menu) {
            EnterPrefix(Prefix::Menu);
        } else {
            Reset();
        }
        return out;
    case Key::Space:
        if (!candidates_.empty()) CommitPrefix(selection_, now, out);
        return out;
    case Key::Enter:
        if (prefix_ != Prefix::Menu && !candidates_.empty()) {
            CommitPrefix(selection_, now, out);
        } else {
            out.commit = Output(PrefixText());  // Nothing to convert: type it as is.
            Reset();
        }
        return out;
    case Key::Up: MoveSelection(vertical ? -1 : -page); return out;
    case Key::Down: MoveSelection(vertical ? 1 : page); return out;
    case Key::Left: if (!vertical) MoveSelection(-1); return out;
    case Key::Right: if (!vertical) MoveSelection(1); return out;
    case Key::PageUp: MoveSelection(-page); return out;
    case Key::PageDown:
    case Key::Tab: MoveSelection(page); return out;
    case Key::Char:
        break;
    default:
        return out;
    }

    switch (prefix_) {
    case Prefix::Menu:
        if (key.letter) {
            // Letters pick the mode with or without Shift ('U' or 'u').
            if (key.letter == 'u') EnterPrefix(Prefix::Unicode);
            else if (key.letter == 'b') EnterPrefix(Prefix::Big5);
            else if (key.letter == 'p') EnterPrefix(Prefix::Shortcut);
            return out;
        }
        if (key.ch >= 0x21 && key.ch <= 0x7E) {
            // Punctuation (and digits): the full-width form of the key; . is the 句號 。.
            out.commit = key.ch == '.' ? u"。" : ToFullWidth(std::u16string(1, static_cast<char16_t>(key.ch)));
            Reset();
        }
        return out;
    case Prefix::Unicode:
    case Prefix::Big5: {
        const bool unicode = prefix_ == Prefix::Unicode;
        const size_t maxDigits = unicode ? kMaxUnicodeDigits : kMaxBig5Digits;
        int v = HexValue(key);
        if (v < 0 || prefixCode_.size() >= maxDigits) return out;
        prefixCode_ += "0123456789ABCDEF"[v];
        RecomputePrefix();
        // A complete code commits by itself.
        if (prefixCode_.size() == maxDigits && !candidates_.empty()) CommitPrefix(0, now, out);
        return out;
    }
    case Prefix::Shortcut:
        if (key.letter && !key.shift) {
            if (prefixCode_.size() < kMaxShortcutLetters) {
                prefixCode_ += key.letter;
                RecomputePrefix();
            }
            return out;
        }
        if (key.digit >= 0 && !key.shift) {
            size_t slot = key.digit == 0 ? 9 : static_cast<size_t>(key.digit - 1);
            size_t index = PageStart() + slot;
            if (slot < options_.pageSize && index < candidates_.size()) CommitPrefix(index, now, out);
            return out;
        }
        if (key.ch == '-' || key.ch == '=') MoveSelection(key.ch == '-' ? -page : page);
        return out;
    case Prefix::None:
        break;
    }
    return out;
}

}  // namespace nova
