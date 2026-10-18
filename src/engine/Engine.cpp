#include "Engine.h"

#include <algorithm>
#include <unordered_set>

#include "Unicode.h"

namespace nova {

namespace {

constexpr size_t kMaxPredictions = 60;
constexpr size_t kMaxWildcardResults = 300;
constexpr size_t kMaxAssociations = 60;

// 倉頡字母 a..z
constexpr char16_t kRadicals[] = u"日月金木水火土竹戈十大中一弓人心手口尸廿山女田難卜重";

struct Scored {
    Candidate cand;
    int group = 0;  // 0 user word, 1 character, 2 word, 3 prediction.
    uint32_t weight = 0;
    uint32_t order = 0;
    UserModel::Usage usage;
    bool used = false;
};

bool IsAsciiPunct(char32_t c) {
    return c >= 0x21 && c <= 0x7E && !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') &&
           !(c >= 'A' && c <= 'Z');
}

}  // namespace

char16_t Engine::RadicalFor(char letter) {
    if (letter >= 'a' && letter <= 'z') return kRadicals[letter - 'a'];
    if (letter == '*') return u'＊';
    return static_cast<char16_t>(letter);
}

std::u16string Engine::ToFullWidth(std::u16string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (char16_t c : s) {
        if (c == u' ')
            out += u'　';
        else if (c >= 0x21 && c <= 0x7E)
            out += static_cast<char16_t>(c + 0xFEE0);
        else
            out += c;
    }
    return out;
}

void Engine::SetOptions(const Options& options) {
    options_ = options;
    options_.pageSize = std::clamp<uint32_t>(options_.pageSize, 3, 9);
    if (!buffer_.empty()) Recompute();
    if (!slots_.empty()) Convert();
    if (association_ && !options_.association) Reset();
}

std::u16string Engine::Output(std::u16string_view s) const {
    return fullShape_ ? ToFullWidth(s) : std::u16string(s);
}

size_t Engine::PageCount() const {
    if (candidates_.empty()) return 0;
    return (candidates_.size() + options_.pageSize - 1) / options_.pageSize;
}

void Engine::Reset() {
    ClearSentence();
    prefix_ = Prefix::None;
    prefixCode_.clear();
    buffer_.clear();
    candidates_.clear();
    association_ = false;
    exactCount_ = 0;
    selection_ = 0;
}

void Engine::ClearComposition() { Reset(); }

bool Engine::WouldConsume(const KeyInput& key) const {
    Engine probe = *this;
    probe.dryRun_ = true;
    return probe.Process(key, 0).consumed;
}

EngineOutput Engine::SetChinese(bool on) {
    EngineOutput out;
    if (on == chinese_) return out;
    if (InPrefix())
        out.commit = Output(PrefixText());
    else if (IsComposing())
        out.commit = SentenceText() + Output(Utf8ToUtf16(buffer_));
    Reset();
    chinese_ = on;
    out.modeChanged = true;
    out.consumed = true;
    return out;
}

EngineOutput Engine::Process(const KeyInput& key, int64_t now) {
    if (!chinese_) {
        EngineOutput out;
        if (fullShape_ && !key.ctrl && !key.alt) {
            if (key.key == Key::Space) {
                out.consumed = true;
                out.commit = u"　";
            } else if (key.key == Key::Char && key.ch >= 0x21 && key.ch <= 0x7E) {
                out.consumed = true;
                out.commit = ToFullWidth(std::u16string(1, static_cast<char16_t>(key.ch)));
            }
        }
        return out;
    }
    if (InPrefix()) return ProcessPrefix(key, now);
    if (options_.sentenceMode && IsComposing()) return ProcessSentence(key, now);
    if (IsComposing()) return ProcessComposing(key, now);
    if (association_) return ProcessAssociation(key, now);
    return ProcessIdle(key, now);
}

EngineOutput Engine::ProcessIdle(const KeyInput& key, int64_t now) {
    EngineOutput out;
    if (key.ctrl || key.alt) return out;
    if (key.key == Key::Space) {
        if (fullShape_) {
            out.consumed = true;
            out.commit = u"　";
        }
        return out;
    }
    if (key.key != Key::Char) return out;

    if (key.letter && !key.shift && !key.capsLock) {
        out.consumed = true;
        AppendKey(key.letter, now, out);
        return out;
    }
    if (key.ch == kPrefixKey) {
        out.consumed = true;
        EnterPrefix(Prefix::Menu);
        return out;
    }
    if (IsAsciiPunct(key.ch) && chinesePunct_) {
        std::u16string p = PunctuationFor(key.ch);
        if (!p.empty()) {
            out.consumed = true;
            out.commit = p;
            return out;
        }
    }
    if (fullShape_ && key.ch >= 0x21 && key.ch <= 0x7E) {
        out.consumed = true;
        out.commit = ToFullWidth(std::u16string(1, static_cast<char16_t>(key.ch)));
    }
    return out;
}

EngineOutput Engine::ProcessAssociation(const KeyInput& key, int64_t now) {
    // Shift+1..9 picks a word continuation; Esc dismisses; anything else
    // dismisses the list and is handled as a normal key.
    if (key.key == Key::Escape && !key.ctrl && !key.alt) {
        Reset();
        EngineOutput out;
        out.consumed = true;
        return out;
    }
    if (key.key == Key::Char && key.shift && !key.ctrl && !key.alt && key.digit >= 1) {
        size_t index = PageStart() + static_cast<size_t>(key.digit - 1);
        if (key.digit <= static_cast<int>(options_.pageSize) && index < candidates_.size()) {
            EngineOutput out;
            out.consumed = true;
            CommitCandidate(index, now, out);
            return out;
        }
    }
    Reset();
    return ProcessIdle(key, now);
}

EngineOutput Engine::ProcessComposing(const KeyInput& key, int64_t now) {
    EngineOutput out;
    if (key.ctrl || key.alt) return out;  // Let shortcuts through untouched.
    out.consumed = true;
    const bool vertical = options_.layout == CandidateLayout::Vertical;
    const long page = static_cast<long>(options_.pageSize);

    switch (key.key) {
    case Key::Space:
        if (!candidates_.empty()) {
            CommitCandidate(selection_, now, out);
        } else if (!options_.quick) {
            // 倉頡: a code with no character is a typo. Beep and drop it.
            ClearComposition();
            out.beep = true;
        }
        return out;
    case Key::Enter:
        out.commit = Output(Utf8ToUtf16(buffer_));
        ClearComposition();
        return out;
    case Key::Escape:
        ClearComposition();
        return out;
    case Key::Backspace:
        buffer_.pop_back();
        if (buffer_.empty())
            ClearComposition();
        else
            Recompute();
        return out;
    case Key::Up:
        MoveSelection(vertical ? -1 : -page);
        return out;
    case Key::Down:
        MoveSelection(vertical ? 1 : page);
        return out;
    case Key::Left:
        if (!vertical) MoveSelection(-1);
        return out;
    case Key::Right:
        if (!vertical) MoveSelection(1);
        return out;
    case Key::PageUp:
        MoveSelection(-page);
        return out;
    case Key::PageDown:
    case Key::Tab:
        MoveSelection(page);
        return out;
    case Key::Home:
        selection_ = 0;
        return out;
    case Key::End:
        if (!candidates_.empty()) selection_ = candidates_.size() - 1;
        return out;
    case Key::Char:
        break;
    default:
        return out;  // Swallow other keys so the composition stays intact.
    }

    if (key.letter && !key.shift) {
        AppendKey(key.letter, now, out);
        return out;
    }
    if (key.ch == '*') {
        if (buffer_.size() < kMaxBuffer) {
            buffer_ += '*';
            Recompute();
        }
        return out;
    }
    if (key.digit >= 0 && !key.shift) {
        size_t slot = key.digit == 0 ? 9 : static_cast<size_t>(key.digit - 1);
        size_t index = PageStart() + slot;
        if (slot < options_.pageSize && index < candidates_.size()) CommitCandidate(index, now, out);
        return out;
    }
    if (!candidates_.empty() && (key.ch == '-' || key.ch == '=')) {
        MoveSelection(key.ch == '-' ? -page : page);
        return out;
    }

    if (key.ch == 0) return out;  // Dead key or unmapped key: ignore.

    // Any other printable key: commit the current choice, then type the key.
    std::u16string first;
    if (!candidates_.empty()) {
        CommitCandidate(selection_, now, out);
        first = out.commit;
    }
    Reset();  // No association list: the user is still typing.
    EngineOutput rest = ProcessIdle(key, now);
    if (rest.consumed) {
        out.commit = first + rest.commit;
    } else {
        // Insert the character ourselves so it lands after the committed text.
        std::u16string ch;
        AppendCodePoint(ch, key.ch);
        out.commit = first + ch;
    }
    return out;
}

void Engine::AppendKey(char c, int64_t now, EngineOutput& out) {
    size_t letters = static_cast<size_t>(std::count_if(buffer_.begin(), buffer_.end(), IsCodeLetter));
    if (letters >= MaxCodeLetters() || buffer_.size() >= kMaxBuffer) {
        // 速成 always places the character when a third key starts the next one.
        if (!(options_.autoCommitOnOverflow || options_.quick) || candidates_.empty()) return;
        if (options_.sentenceMode) {
            InsertCandidate(selection_, selection_ != 0);
            AfterInsert(now, out);
        } else {
            CommitCandidate(selection_, now, out);
            Reset();  // Typing continues; do not show associations.
        }
    }
    association_ = false;
    buffer_ += c;
    Recompute();
    if (options_.autoCommitUnique && AutoCommitAllowed()) {
        if (options_.sentenceMode) {
            InsertCandidate(0, false);
            AfterInsert(now, out);
        } else {
            std::u16string before = out.commit;
            CommitCandidate(0, now, out);
            out.commit = before + out.commit;
        }
    }
}

bool Engine::AutoCommitAllowed() const {
    if (!dict_ || exactCount_ != 1 || buffer_.find('*') != std::string::npos) return false;
    for (IndexId idx : CharIndices()) {
        auto longer = dict_->Longer(idx, buffer_);
        if (longer.first != longer.second) return false;
    }
    return true;
}

std::vector<IndexId> Engine::CharIndices() const {
    const bool q = options_.quick;
    switch (options_.version) {
    case CangjieVersion::V3: return {q ? kIndexQuick3 : kIndexCj3};
    case CangjieVersion::Mixed: return {q ? kIndexQuick5 : kIndexCj5, q ? kIndexQuick3 : kIndexCj3};
    default: return {q ? kIndexQuick5 : kIndexCj5};
    }
}

bool Engine::AllowedByCharset(const DictEntry& e) const {
    if (!(e.flags & kEntryHan)) return true;  // Symbols are never filtered.
    switch (options_.charset) {
    case Charset::All:
        return true;
    case Charset::Big5Hkscs:
        return (e.flags & (kEntryBig5 | kEntryHkscs)) != 0;
    case Charset::Big5:
        return (e.flags & kEntryBig5) != 0 && !(e.flags & kEntryHkscs);
    }
    return true;
}

Candidate Engine::MakeCandidate(const DictEntry& e, Candidate::Kind kind, std::string_view code) const {
    Candidate c;
    c.source = std::u16string(dict_->Text(e));
    std::u16string_view simp = dict_->Simplified(e);
    c.text = options_.outputSimplified && !simp.empty() ? std::u16string(simp) : c.source;
    c.code = std::string(code);
    c.kind = kind;
    return c;
}

void Engine::Recompute() {
    candidates_.clear();
    exactCount_ = 0;
    selection_ = 0;
    association_ = false;
    // A dry run only needs to know whether a key is consumed.
    if (dryRun_ || buffer_.empty() || !dict_ || !dict_->IsLoaded()) return;

    const std::vector<IndexId> charIndices = CharIndices();
    const bool wildcard = buffer_.find('*') != std::string::npos;
    std::vector<Scored> list;
    std::unordered_set<std::u16string> seen;

    auto add = [&](const DictEntry& e, int group, std::string_view code) {
        if (!AllowedByCharset(e)) return;
        Candidate::Kind kind = (e.flags & kEntryPhrase) ? Candidate::Kind::Phrase : Candidate::Kind::Char;
        Scored s;
        s.cand = MakeCandidate(e, kind, code);
        if (!seen.insert(s.cand.text).second) return;
        s.group = group;
        s.weight = e.weight;
        s.order = e.order;
        if (group == 3 && options_.showCodeHints && code.size() > buffer_.size())
            s.cand.hint = std::string(code.substr(buffer_.size()));
        // A wildcard match shows its full code, e.g. 合 [人一口] (倉頡 only).
        if (wildcard && !options_.quick) s.cand.hint = "[" + std::string(code) + "]";
        list.push_back(std::move(s));
    };

    if (wildcard) {
        auto scan = [&](IndexId idx) {
            auto [b, e] = dict_->All(idx);
            for (auto it = b; it != e && list.size() < kMaxWildcardResults * 4; ++it) {
                std::string_view code = CodeView(it->code);
                if (WildcardMatch(buffer_, code)) add(dict_->Entry(it->entry), 1, code);
            }
        };
        for (IndexId idx : charIndices) scan(idx);
    } else {
        if (user_) {
            for (IndexId idx : charIndices) {
                for (const UserPhrase& p : user_->UserPhrasesForCode(buffer_, idx == kIndexCj3)) {
                    Scored s;
                    s.cand.text = s.cand.source = p.text;
                    s.cand.code = buffer_;
                    s.cand.kind = Candidate::Kind::UserPhrase;
                    s.group = 0;
                    if (seen.insert(s.cand.text).second) list.push_back(std::move(s));
                }
            }
        }
        for (IndexId idx : charIndices) {
            auto [b, e] = dict_->Exact(idx, buffer_);
            for (auto it = b; it != e; ++it) add(dict_->Entry(it->entry), 1, buffer_);
        }
    }

    const bool byFrequency = options_.order == CandidateOrder::Frequency;
    if (options_.learning && user_)
        for (Scored& s : list) s.used = user_->Lookup(s.cand.source, &s.usage);

    auto less = [&](const Scored& a, const Scored& b) {
        if (a.used != b.used) return a.used;  // Recently used choices first.
        if (a.used && a.usage.lastUsed != b.usage.lastUsed) return a.usage.lastUsed > b.usage.lastUsed;
        if (a.group != b.group) return a.group < b.group;
        if (byFrequency && a.weight != b.weight) return a.weight > b.weight;
        if (a.order != b.order) return a.order > b.order;
        return a.weight > b.weight;
    };
    std::stable_sort(list.begin(), list.end(), less);
    if (wildcard && list.size() > kMaxWildcardResults) list.resize(kMaxWildcardResults);
    exactCount_ = list.size();

    // Prediction: characters whose code continues the typed keys. Rank the
    // raw index items first so only the best few become candidates.
    if (!wildcard && options_.prediction && buffer_.size() < MaxCodeLetters()) {
        std::vector<const CodeIndexItem*> more;
        std::unordered_set<uint32_t> entries;
        for (IndexId idx : charIndices) {
            auto [b, e] = dict_->Longer(idx, buffer_);
            for (auto it = b; it != e; ++it) {
                const DictEntry& entry = dict_->Entry(it->entry);
                if ((entry.flags & kEntryHan) && AllowedByCharset(entry) && entries.insert(it->entry).second)
                    more.push_back(&*it);
            }
        }
        auto better = [&](const CodeIndexItem* x, const CodeIndexItem* y) {
            const DictEntry& a = dict_->Entry(x->entry);
            const DictEntry& c = dict_->Entry(y->entry);
            if (byFrequency && a.weight != c.weight) return a.weight > c.weight;
            if (a.order != c.order) return a.order > c.order;
            return a.weight > c.weight;
        };
        size_t keep = std::min(more.size(), kMaxPredictions * 2);
        std::partial_sort(more.begin(), more.begin() + keep, more.end(), better);
        for (size_t i = 0; i < keep && list.size() < exactCount_ + kMaxPredictions; ++i)
            add(dict_->Entry(more[i]->entry), 3, CodeView(more[i]->code));
    }

    candidates_.reserve(list.size());
    for (Scored& s : list) candidates_.push_back(std::move(s.cand));
}

void Engine::MoveSelection(long delta) {
    if (candidates_.empty()) return;
    long last = static_cast<long>(candidates_.size()) - 1;
    long next = static_cast<long>(selection_) + delta;
    selection_ = static_cast<size_t>(std::clamp(next, 0L, last));
}

void Engine::SetSelection(size_t index) {
    if (index < candidates_.size()) selection_ = index;
}

void Engine::SetPage(size_t page) {
    size_t start = page * options_.pageSize;
    if (start < candidates_.size()) selection_ = start;
}

void Engine::CommitCandidate(size_t index, int64_t now, EngineOutput& out) {
    if (index >= candidates_.size()) return;
    Candidate c = candidates_[index];
    out.commit += c.text;
    // For an association, source holds the whole word, so learning and the
    // next association both use it.
    if (Records()) user_->Record(c.source, now);
    Reset();
    if (options_.association) ShowAssociations(c.source);
}

void Engine::ShowAssociations(std::u16string_view committed) {
    candidates_.clear();
    selection_ = 0;
    association_ = false;
    if (dryRun_ || !dict_ || committed.empty()) return;
    char32_t last = LastCodePoint(committed);
    auto [b, e] = dict_->Associations(last);
    std::vector<Scored> list;
    std::unordered_set<std::u16string> seen;
    for (auto it = b; it != e && list.size() < kMaxAssociations * 2; ++it) {
        const DictEntry& entry = dict_->Entry(it->entry);
        if (!AllowedByCharset(entry)) continue;
        Scored s;
        s.cand = MakeCandidate(entry, Candidate::Kind::Association, CodeView(entry.cj5));
        // Show and insert only the continuation (drop the first character).
        size_t skip = FirstCodePoint(s.cand.text) >= 0x10000 ? 2 : 1;
        if (s.cand.text.size() <= skip) continue;
        s.cand.text = s.cand.text.substr(skip);
        if (!seen.insert(s.cand.text).second) continue;
        s.weight = entry.weight;
        if (options_.learning && user_) s.used = user_->Lookup(s.cand.source, &s.usage);
        list.push_back(std::move(s));
    }
    std::stable_sort(list.begin(), list.end(), [](const Scored& a, const Scored& b) {
        if (a.used != b.used) return a.used;
        if (a.used && a.usage.lastUsed != b.usage.lastUsed) return a.usage.lastUsed > b.usage.lastUsed;
        return a.weight > b.weight;
    });
    if (list.size() > kMaxAssociations) list.resize(kMaxAssociations);
    for (Scored& s : list) candidates_.push_back(std::move(s.cand));
    association_ = !candidates_.empty();
}

EngineOutput Engine::Select(size_t index, int64_t now) {
    EngineOutput out;
    if (index >= candidates_.size()) return out;
    out.consumed = true;
    if (InPrefix()) {
        CommitPrefix(index, now, out);
    } else if (selecting_) {
        ChooseSlotCandidate(index);
    } else if (options_.sentenceMode && !buffer_.empty()) {
        InsertCandidate(index, true);
        AfterInsert(now, out);
    } else {
        CommitCandidate(index, now, out);
    }
    return out;
}

EngineOutput Engine::CommitSelection(int64_t now) {
    EngineOutput out;
    out.consumed = IsComposing();
    if (InPrefix()) {
        if (prefix_ != Prefix::Menu && !candidates_.empty()) CommitPrefix(selection_, now, out);
    } else if (options_.sentenceMode && IsComposing()) {
        if (!buffer_.empty() && !candidates_.empty()) InsertCandidate(selection_, selection_ != 0);
        CommitSentence(now, out, false);
    } else if (IsComposing() && !candidates_.empty()) {
        CommitCandidate(selection_, now, out);
    }
    Reset();
    return out;
}

std::u16string Engine::PunctuationFor(char32_t ch) {
    switch (ch) {
    case ',': return u"，";
    case '.': return u"。";
    case ';': return u"；";
    case ':': return u"：";
    case '?': return u"？";
    case '!': return u"！";
    case '(': return u"（";
    case ')': return u"）";
    case '[': return u"「";
    case ']': return u"」";
    case '{': return u"『";
    case '}': return u"』";
    case '<': return u"《";
    case '>': return u"》";
    case '\\': return u"、";
    case '~': return u"～";
    case '^': return u"……";
    case '\'':
        singleQuoteOpen_ = !singleQuoteOpen_;
        return singleQuoteOpen_ ? u"‘" : u"’";
    case '"':
        doubleQuoteOpen_ = !doubleQuoteOpen_;
        return doubleQuoteOpen_ ? u"“" : u"”";
    default:
        return {};
    }
}

}  // namespace nova
