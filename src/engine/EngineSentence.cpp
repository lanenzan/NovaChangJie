// NovaChangJie - sentence mode (智慧選字), modelled on 新注音:
//
//   * Each code + Space (or a 6th key) adds one character slot to an
//     underlined composition instead of committing it.
//   * Slots typed without an explicit choice stay ambiguous; Convert() picks
//     characters for all of them with a word lattice, so 'a' becomes 日 or 曰
//     depending on the words it forms with its neighbours.
//   * ←/→ move the cursor, ↓ lists words and characters at the cursor,
//     Enter (or Space with no code typed) commits the whole sentence.
//   * Sentences longer than kMaxSentence commit their leading words.
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "Engine.h"
#include "Unicode.h"

namespace nova {

namespace {

constexpr size_t kMaxSlotChoices = 100;  // Characters kept per slot (速成 codes match many).
constexpr size_t kMaxWildcardSlotChoices = 300;  // Characters kept for a code with '*'.
constexpr size_t kMaxAssocScan = 4000;  // Words scanned per first character.
constexpr size_t kMaxSlotWords = 40;  // Word choices listed at a slot.
constexpr double kLearnedBonus = 3.0;  // Recently used words win close calls.
constexpr double kLearnedPairBonus = 3.0;  // So do word pairs the user has typed.
constexpr double kNeuralWeight = 0.75;  // Neural log-probability added to the lattice score (tuned with ncj-eval).

// Decodes the code point at `i` (advancing it); 0 at the end.
char32_t NextCodePoint(std::u16string_view s, size_t& i) {
    if (i >= s.size()) return 0;
    char16_t c = s[i++];
    if (c >= 0xD800 && c <= 0xDBFF && i < s.size() && s[i] >= 0xDC00 && s[i] <= 0xDFFF)
        return 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) + (s[i++] - 0xDC00);
    return c;
}

std::vector<std::u16string> SplitChars(std::u16string_view s) {
    std::vector<std::u16string> out;
    for (char32_t cp : CodePoints(s)) {
        std::u16string ch;
        AppendCodePoint(ch, cp);
        out.push_back(std::move(ch));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Display

std::u16string Engine::SentenceText() const {
    std::u16string out;
    for (const auto& c : converted_) out += ToOutputText(c);
    return out;
}

std::u16string Engine::CompositionText() const {
    if (InPrefix()) return PrefixText();
    std::u16string code;
    for (char c : buffer_) code += options_.showLetters && c != '*' ? static_cast<char16_t>(c) : RadicalFor(c);
    if (slots_.empty()) return code;
    std::u16string out;
    for (size_t i = 0; i < converted_.size(); ++i) {
        if (i == cursor_) out += code;
        out += ToOutputText(converted_[i]);
    }
    if (cursor_ >= converted_.size()) out += code;
    return out;
}

size_t Engine::CaretOffset() const {
    if (InPrefix()) return PrefixText().size();
    size_t offset = 0;
    for (size_t i = 0; i < cursor_ && i < converted_.size(); ++i) offset += ToOutputText(converted_[i]).size();
    return offset + buffer_.size();  // Radicals and letters are all BMP.
}

bool Engine::CandidatesVisible() const {
    if (candidates_.empty()) return false;
    if (options_.sentenceMode && !buffer_.empty() && !selecting_) return options_.codeCandidates;
    return true;
}

std::u16string Engine::ToOutputText(std::u16string_view source) const {
    if (!options_.outputSimplified || !dict_) return std::u16string(source);
    std::u16string out;
    for (char32_t cp : CodePoints(source)) {
        const DictEntry* e = dict_->FindChar(cp);
        std::u16string_view simp = e ? dict_->Simplified(*e) : std::u16string_view();
        if (!simp.empty())
            out += simp;
        else
            AppendCodePoint(out, cp);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Slots

uint32_t Engine::CharEntry(std::u16string_view ch) const {
    return dict_ ? dict_->FindCharId(FirstCodePoint(ch)) : UINT32_MAX;
}

uint32_t Engine::CharWeight(std::u16string_view ch) const {
    const DictEntry* e = dict_ ? dict_->FindChar(FirstCodePoint(ch)) : nullptr;
    return e ? e->weight : 0;
}

bool Engine::MakeSlot(const std::string& code, const std::u16string& fixed, Slot* slot) const {
    slot->code = code;
    slot->fixed = fixed;
    slot->choices.clear();
    if (dict_ && !code.empty()) {
        const bool wildcard = code.find('*') != std::string::npos;
        std::unordered_set<std::u16string> seen;
        auto add = [&](const DictEntry& entry) {
            if (!AllowedByCharset(entry)) return;
            std::u16string text(dict_->Text(entry));
            if (seen.insert(text).second) slot->choices.push_back({std::move(text), entry.weight, dict_->EntryId(entry)});
        };
        if (wildcard) {
            // Every matching character stays a choice, so the conversion can
            // pick the one that forms a word with its neighbours.
            for (IndexId idx : CharIndices()) {
                auto [b, e] = dict_->All(idx);
                for (auto it = b; it != e; ++it)
                    if (WildcardMatch(code, CodeView(it->code))) add(dict_->Entry(it->entry));
            }
        } else {
            for (IndexId idx : CharIndices()) {
                auto [b, e] = dict_->Exact(idx, code);
                for (auto it = b; it != e; ++it) add(dict_->Entry(it->entry));
            }
        }
        std::stable_sort(slot->choices.begin(), slot->choices.end(),
                         [](const SlotChoice& a, const SlotChoice& b) { return a.weight > b.weight; });
        const size_t limit = wildcard ? kMaxWildcardSlotChoices : kMaxSlotChoices;
        if (slot->choices.size() > limit) slot->choices.resize(limit);
    }
    if (!fixed.empty() &&
        std::none_of(slot->choices.begin(), slot->choices.end(), [&](const SlotChoice& c) { return c.text == fixed; }))
        slot->choices.push_back({fixed, CharWeight(fixed), CharEntry(fixed)});
    return !slot->choices.empty();
}

void Engine::InsertCandidate(size_t index, bool explicitChoice) {
    if (index >= candidates_.size()) return;
    const Candidate c = candidates_[index];
    const bool cj3 = options_.version == CangjieVersion::V3;
    std::vector<Slot> added;
    if (!explicitChoice && buffer_.find('*') != std::string::npos) {
        // A wildcard code: leave the choice among its matches to the conversion.
        Slot s;
        if (MakeSlot(buffer_, {}, &s)) added.push_back(std::move(s));
    } else if (c.kind == Candidate::Kind::Char && !explicitChoice && c.hint.empty()) {
        // Leave the choice to the conversion.
        Slot s;
        if (MakeSlot(buffer_, {}, &s)) added.push_back(std::move(s));
    } else if (c.kind == Candidate::Kind::Char) {
        Slot s;
        if (MakeSlot(c.code, c.source, &s)) added.push_back(std::move(s));
    } else {
        // A word: one fixed slot per character.
        for (const std::u16string& ch : SplitChars(c.source)) {
            const DictEntry* e = dict_ ? dict_->FindChar(FirstCodePoint(ch)) : nullptr;
            std::string code = e ? std::string(CodeView(cj3 ? e->cj3 : e->cj5)) : std::string();
            Slot s;
            if (MakeSlot(code, ch, &s)) added.push_back(std::move(s));
        }
    }
    if (added.empty()) return;
    slots_.insert(slots_.begin() + static_cast<long>(cursor_), std::make_move_iterator(added.begin()),
                  std::make_move_iterator(added.end()));
    cursor_ += added.size();
    buffer_.clear();
    candidates_.clear();
    selection_ = 0;
    exactCount_ = 0;
    Convert();
}

void Engine::AfterInsert(int64_t now, EngineOutput& out) {
    if (slots_.size() > kMaxSentence) CommitLeading(slots_.size() - kMaxSentence, now, out);
}

// ---------------------------------------------------------------------------
// Conversion: beam search through a lattice of single characters and corpus
// words. A sentence scores the sum of log P(word | previous word) from the
// dictionary's word pair model (log P(word) when it has none), plus bonuses
// for words and word pairs the user has typed before.

std::vector<Engine::WordMatch> Engine::MatchWords(size_t start, bool useFixed, size_t maxFirstChars) const {
    std::vector<WordMatch> out;
    const size_t n = slots_.size();
    if (!dict_ || start + 1 >= n) return out;

    // Characters allowed at each following slot, built on first use.
    std::vector<std::unordered_set<char32_t>> allowedSets(n);
    std::vector<bool> built(n, false);
    auto allowed = [&](size_t i, char32_t cp) {
        const Slot& s = slots_[i];
        if (useFixed && !s.fixed.empty()) return FirstCodePoint(s.fixed) == cp;
        if (!built[i]) {
            for (const SlotChoice& c : s.choices) allowedSets[i].insert(FirstCodePoint(c.text));
            built[i] = true;
        }
        return allowedSets[i].count(cp) != 0;
    };

    std::vector<std::u16string> firsts;
    const Slot& first = slots_[start];
    if (useFixed && !first.fixed.empty()) {
        firsts.push_back(first.fixed);
    } else {
        for (const SlotChoice& c : first.choices) {
            if (firsts.size() >= maxFirstChars) break;
            firsts.push_back(c.text);
        }
    }
    for (const std::u16string& f : firsts) {
        auto [b, e] = dict_->Associations(FirstCodePoint(f));
        size_t scanned = 0;
        for (auto it = b; it != e && scanned < kMaxAssocScan; ++it, ++scanned) {
            const DictEntry& entry = dict_->Entry(it->entry);
            std::u16string_view word = dict_->Text(entry);
            size_t pos = 0;
            NextCodePoint(word, pos);  // First character already matches.
            size_t length = 1;
            bool ok = true;
            while (pos < word.size()) {
                char32_t cp = NextCodePoint(word, pos);
                if (start + length >= n || !allowed(start + length, cp)) {
                    ok = false;
                    break;
                }
                ++length;
            }
            if (ok && length >= 2 && AllowedByCharset(entry)) out.push_back({&entry, length});
        }
    }
    return out;
}

void Engine::Convert() {
    const size_t n = slots_.size();
    converted_.assign(n, {});
    segments_.clear();
    alternatives_.clear();
    if (n == 0 || dryRun_ || !dict_) return;
    const bool pairs = options_.wordPairs && dict_->HasWordPairs();
    const bool learned = options_.learning && user_;

    // One way to continue a partial sentence: a character or a word.
    struct Step {
        uint32_t entry;
        size_t length;
        std::u16string text;
        double score;  // log P(word) plus the learned bonus.
        bool used;     // The user has typed this word.
    };
    // A partial sentence, linked to the one it extends.
    struct Hyp {
        double score;
        int parent;
        uint32_t entry;
        size_t start, length;
        const Step* step;
    };
    std::vector<std::vector<Step>> steps(n);
    std::vector<Hyp> hyps;
    std::vector<std::vector<int>> ending(n + 1);
    hyps.push_back({0, -1, dict_->SentenceStartId(), 0, 0, nullptr});
    ending[0].push_back(0);

    auto prune = [&](std::vector<int>& list) {
        std::sort(list.begin(), list.end(), [&](int a, int b) { return hyps[a].score > hyps[b].score; });
        std::vector<int> kept;
        std::vector<std::pair<uint32_t, size_t>> perWord;
        for (int h : list) {
            if (kept.size() >= options_.sentenceBeam) break;
            auto it = std::find_if(perWord.begin(), perWord.end(),
                                   [&](const auto& p) { return p.first == hyps[h].entry; });
            if (it == perWord.end()) {
                perWord.push_back({hyps[h].entry, 1});
            } else if (it->second++ >= options_.sentenceBeamPerWord) {
                continue;
            }
            kept.push_back(h);
        }
        list.swap(kept);
    };
    auto makeStep = [&](uint32_t entry, size_t length, std::u16string text, uint32_t weight) {
        Step st{entry, length, std::move(text), std::log(static_cast<double>(weight) + 1.0) - kLogCorpus, false};
        if (learned && user_->Lookup(st.text, nullptr)) {
            st.used = true;
            st.score += kLearnedBonus;
        }
        return st;
    };

    for (size_t i = 0; i < n; ++i) {
        prune(ending[i]);
        if (ending[i].empty()) continue;
        const Slot& s = slots_[i];
        std::vector<Step>& next = steps[i];
        if (!s.fixed.empty()) {
            next.push_back(makeStep(CharEntry(s.fixed), 1, s.fixed, CharWeight(s.fixed)));
        } else {
            for (const SlotChoice& c : s.choices) next.push_back(makeStep(c.entry, 1, c.text, c.weight));
        }
        for (const WordMatch& m : MatchWords(i, true, options_.sentenceFirstChars))
            next.push_back(makeStep(dict_->EntryId(*m.entry), m.length, std::u16string(dict_->Text(*m.entry)),
                                       m.entry->weight));

        for (int h : ending[i]) {
            const Hyp prev = hyps[h];
            const float backoff = pairs ? dict_->Backoff(prev.entry) : 0.0f;
            for (const Step& st : next) {
                double score = st.score;
                if (pairs && st.entry != UINT32_MAX && prev.entry != UINT32_MAX) {
                    float lp;
                    if (dict_->PairLogProb(prev.entry, st.entry, &lp))
                        score += lp - (st.score - (st.used ? kLearnedBonus : 0.0));
                    else
                        score += backoff;
                }
                if (st.used && prev.step && prev.step->used &&
                    user_->Lookup(prev.step->text + u' ' + st.text, nullptr))
                    score += kLearnedPairBonus;
                hyps.push_back({prev.score + score, h, st.entry, i, st.length, &st});
                ending[i + st.length].push_back(static_cast<int>(hyps.size() - 1));
            }
        }
    }
    prune(ending[n]);
    if (ending[n].empty()) return;

    // The best few sentences, best first.
    for (int last : ending[n]) {
        Alternative alt;
        alt.score = hyps[last].score;
        std::vector<Segment> reversed;
        for (int h = last; hyps[h].parent >= 0; h = hyps[h].parent)
            reversed.push_back({hyps[h].start, hyps[h].length, hyps[h].step->text});
        alt.segments.assign(reversed.rbegin(), reversed.rend());
        for (const Segment& seg : alt.segments) alt.text += seg.text;
        if (std::none_of(alternatives_.begin(), alternatives_.end(),
                         [&](const Alternative& a) { return a.text == alt.text; }))
            alternatives_.push_back(std::move(alt));
        if (alternatives_.size() >= options_.sentenceAlternatives) break;
    }
    if (scorer_ && options_.neuralRerank && alternatives_.size() > 1) {
        std::vector<std::u16string> texts;
        for (const Alternative& a : alternatives_) texts.push_back(a.text);
        std::vector<float> neural;
        if (scorer_->Score(texts, &neural) && neural.size() == texts.size()) {
            for (size_t k = 0; k < neural.size(); ++k) alternatives_[k].score += kNeuralWeight * neural[k];
            std::stable_sort(alternatives_.begin(), alternatives_.end(),
                             [](const Alternative& a, const Alternative& b) { return a.score > b.score; });
        }
    }
    segments_ = alternatives_.front().segments;
    for (const Segment& seg : segments_) {
        std::vector<std::u16string> chars = SplitChars(seg.text);
        for (size_t k = 0; k < seg.length && k < chars.size(); ++k) converted_[seg.start + k] = chars[k];
    }
}

// ---------------------------------------------------------------------------
// Choices at a slot (↓)

void Engine::OpenSlotCandidates() {
    selecting_ = false;
    candidates_.clear();
    selection_ = 0;
    const size_t n = slots_.size();
    if (n == 0 || dryRun_) return;
    const size_t p = cursor_ < n ? cursor_ : n - 1;

    std::vector<WordMatch> words = MatchWords(p, false, kMaxSlotChoices);
    std::stable_sort(words.begin(), words.end(), [](const WordMatch& a, const WordMatch& b) {
        if (a.length != b.length) return a.length > b.length;
        return a.entry->weight > b.entry->weight;
    });
    std::unordered_set<std::u16string> seen;
    for (const WordMatch& m : words) {
        if (candidates_.size() >= kMaxSlotWords) break;
        Candidate c;
        c.source = std::u16string(dict_->Text(*m.entry));
        if (!seen.insert(c.source).second) continue;
        c.text = ToOutputText(c.source);
        c.kind = Candidate::Kind::Phrase;
        c.span = m.length;
        candidates_.push_back(std::move(c));
    }
    // Single characters, with recently used ones first.
    std::vector<SlotChoice> chars = slots_[p].choices;
    if (options_.learning && user_) {
        std::stable_sort(chars.begin(), chars.end(), [&](const SlotChoice& a, const SlotChoice& b) {
            UserModel::Usage ua, ub;
            bool ha = user_->Lookup(a.text, &ua), hb = user_->Lookup(b.text, &ub);
            if (ha != hb) return ha;
            return ha && ua.lastUsed > ub.lastUsed;
        });
    }
    for (const SlotChoice& ch : chars) {
        Candidate c;
        c.source = ch.text;
        c.text = ToOutputText(ch.text);
        c.code = slots_[p].code;
        c.kind = Candidate::Kind::Char;
        candidates_.push_back(std::move(c));
    }
    // Start on the current conversion so ↓ then Enter changes nothing: the
    // word that starts here if there is one, otherwise the character.
    size_t span = 1;
    std::u16string current = converted_[p];
    for (const Segment& seg : segments_) {
        if (seg.start == p && seg.length > 1) {
            span = seg.length;
            current = seg.text;
        }
    }
    auto find = [&](size_t wantSpan, const std::u16string& text) {
        for (size_t i = 0; i < candidates_.size(); ++i) {
            if (candidates_[i].span == wantSpan && candidates_[i].source == text) {
                selection_ = i;
                return true;
            }
        }
        return false;
    };
    if (!find(span, current) && span > 1) find(1, converted_[p]);
    selecting_ = !candidates_.empty();
    selectPos_ = p;
}

void Engine::ChooseSlotCandidate(size_t index) {
    if (index >= candidates_.size() || selectPos_ >= slots_.size()) return;
    const Candidate c = candidates_[index];
    std::vector<std::u16string> chars = SplitChars(c.source);
    for (size_t k = 0; k < chars.size() && selectPos_ + k < slots_.size(); ++k) slots_[selectPos_ + k].fixed = chars[k];
    cursor_ = std::min(selectPos_ + chars.size(), slots_.size());
    selecting_ = false;
    candidates_.clear();
    selection_ = 0;
    Convert();
}

// ---------------------------------------------------------------------------
// Committing

void Engine::CommitSentence(int64_t now, EngineOutput& out, bool associate) {
    std::u16string last;
    if (!slots_.empty()) {
        out.commit += SentenceText();
        if (Records()) RecordSegments(segments_.size(), now);
        if (!segments_.empty()) last = segments_.back().text;
    }
    Reset();
    if (associate && options_.association && !last.empty()) ShowAssociations(last);
}

void Engine::CommitLeading(size_t minSlots, int64_t now, EngineOutput& out) {
    // Commit whole words from the start until at least `minSlots` are gone.
    size_t count = 0;
    for (const Segment& seg : segments_) {
        if (count >= minSlots) break;
        count += seg.length;
    }
    count = std::max(count, std::min(minSlots, slots_.size()));
    for (size_t i = 0; i < count && i < converted_.size(); ++i) out.commit += ToOutputText(converted_[i]);
    if (Records()) {
        size_t whole = 0;
        while (whole < segments_.size() && segments_[whole].start + segments_[whole].length <= count) ++whole;
        RecordSegments(whole, now);
    }
    // Keep the committed characters as they were converted.
    slots_.erase(slots_.begin(), slots_.begin() + static_cast<long>(count));
    cursor_ = cursor_ > count ? cursor_ - count : 0;
    Convert();
}

void Engine::RecordSegments(size_t count, int64_t now) {
    // Each word, and each pair of neighbouring words as "word word".
    for (size_t i = 0; i < count && i < segments_.size(); ++i) {
        user_->Record(segments_[i].text, now);
        if (i > 0) user_->Record(segments_[i - 1].text + u' ' + segments_[i].text, now);
    }
}

void Engine::ClearSentence() {
    slots_.clear();
    converted_.clear();
    segments_.clear();
    alternatives_.clear();
    cursor_ = 0;
    selecting_ = false;
    selectPos_ = 0;
}

// ---------------------------------------------------------------------------
// Keys

EngineOutput Engine::ProcessSentence(const KeyInput& key, int64_t now) {
    EngineOutput out;
    if (key.ctrl || key.alt) return out;  // Shortcuts pass through.
    out.consumed = true;
    const bool vertical = options_.layout == CandidateLayout::Vertical;
    const long page = static_cast<long>(options_.pageSize);

    // A list of choices at a slot is open.
    if (selecting_) {
        switch (key.key) {
        case Key::Escape:
            selecting_ = false;
            candidates_.clear();
            return out;
        case Key::Space:
        case Key::Enter:
            ChooseSlotCandidate(selection_);
            return out;
        case Key::Up: MoveSelection(vertical ? -1 : -page); return out;
        case Key::Down: MoveSelection(vertical ? 1 : page); return out;
        case Key::PageUp: MoveSelection(-page); return out;
        case Key::PageDown:
        case Key::Tab: MoveSelection(page); return out;
        case Key::Left:
        case Key::Right:
            if (!vertical) {
                MoveSelection(key.key == Key::Left ? -1 : 1);
                return out;
            }
            break;
        default:
            break;
        }
        if (key.key == Key::Char && key.digit >= 0 && !key.shift) {
            size_t slot = key.digit == 0 ? 9 : static_cast<size_t>(key.digit - 1);
            size_t index = PageStart() + slot;
            if (slot < options_.pageSize && index < candidates_.size()) ChooseSlotCandidate(index);
            return out;
        }
        if (key.key == Key::Char && (key.ch == '-' || key.ch == '=')) {
            MoveSelection(key.ch == '-' ? -page : page);
            return out;
        }
        selecting_ = false;  // Any other key closes the list and acts normally.
        candidates_.clear();
        selection_ = 0;
    }

    // A code is being typed.
    if (!buffer_.empty()) {
        const bool listShown = CandidatesVisible();
        switch (key.key) {
        case Key::Space:
            if (!candidates_.empty()) {
                InsertCandidate(selection_, selection_ != 0);
                AfterInsert(now, out);
            } else if (!options_.quick) {
                // 倉頡: a code with no character is a typo. Beep and drop it,
                // keeping the sentence typed so far.
                buffer_.clear();
                selection_ = 0;
                if (slots_.empty()) Reset();
                out.beep = true;
            }
            return out;
        case Key::Enter:
            if (!candidates_.empty()) {
                InsertCandidate(selection_, selection_ != 0);
                CommitSentence(now, out, true);
            } else {
                std::u16string letters = Output(Utf8ToUtf16(buffer_));
                buffer_.clear();
                CommitSentence(now, out, false);
                out.commit += letters;
            }
            return out;
        case Key::Escape:
            buffer_.clear();
            candidates_.clear();
            selection_ = 0;
            if (slots_.empty()) Reset();
            return out;
        case Key::Backspace:
            buffer_.pop_back();
            if (buffer_.empty()) {
                candidates_.clear();
                selection_ = 0;
            } else {
                Recompute();
            }
            return out;
        case Key::Down:
            if (!listShown) {
                // ↓ while typing: place the character, then list its choices.
                if (!candidates_.empty()) {
                    InsertCandidate(selection_, false);
                    size_t placed = cursor_ ? cursor_ - 1 : 0;
                    cursor_ = placed;
                    OpenSlotCandidates();
                    cursor_ = placed + 1;
                }
                return out;
            }
            MoveSelection(vertical ? 1 : page);
            return out;
        case Key::Up: if (listShown) MoveSelection(vertical ? -1 : -page); return out;
        case Key::Left: if (listShown && !vertical) MoveSelection(-1); return out;
        case Key::Right: if (listShown && !vertical) MoveSelection(1); return out;
        case Key::PageUp: if (listShown) MoveSelection(-page); return out;
        case Key::PageDown:
        case Key::Tab: if (listShown) MoveSelection(page); return out;
        case Key::Home:
        case Key::End:
            return out;
        case Key::Char:
            break;
        default:
            return out;
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
        if (listShown && key.digit >= 0 && !key.shift) {
            size_t slot = key.digit == 0 ? 9 : static_cast<size_t>(key.digit - 1);
            size_t index = PageStart() + slot;
            if (slot < options_.pageSize && index < candidates_.size()) {
                InsertCandidate(index, true);
                AfterInsert(now, out);
            }
            return out;
        }
        if (listShown && (key.ch == '-' || key.ch == '=')) {
            MoveSelection(key.ch == '-' ? -page : page);
            return out;
        }
        if (key.ch == 0) return out;
        // Anything else typed: place the character, commit, then type the key.
        if (!candidates_.empty()) InsertCandidate(selection_, selection_ != 0);
        buffer_.clear();
    } else {
        // Only converted characters: edit the sentence.
        const size_t n = slots_.size();
        switch (key.key) {
        case Key::Space:
        case Key::Enter:
            CommitSentence(now, out, true);
            return out;
        case Key::Escape:
            Reset();
            return out;
        case Key::Backspace:
            if (cursor_ > 0) {
                slots_.erase(slots_.begin() + static_cast<long>(cursor_ - 1));
                --cursor_;
                if (slots_.empty()) Reset(); else Convert();
            }
            return out;
        case Key::Delete:
            if (cursor_ < n) {
                slots_.erase(slots_.begin() + static_cast<long>(cursor_));
                if (slots_.empty()) Reset(); else Convert();
            }
            return out;
        case Key::Left: if (cursor_ > 0) --cursor_; return out;
        case Key::Right: if (cursor_ < n) ++cursor_; return out;
        case Key::Home: cursor_ = 0; return out;
        case Key::End: cursor_ = n; return out;
        case Key::Down:
            OpenSlotCandidates();
            return out;
        case Key::Char:
            break;
        default:
            return out;  // Up, PageUp/Down, Tab: nothing to do.
        }
        if (key.letter && !key.shift) {
            AppendKey(key.letter, now, out);
            return out;
        }
        if (key.ch == 0) return out;
    }

    // Commit the sentence, then handle the key as if nothing was composed.
    CommitSentence(now, out, false);
    EngineOutput rest = ProcessIdle(key, now);
    if (rest.consumed) {
        out.commit += rest.commit;
    } else {
        AppendCodePoint(out.commit, key.ch);
    }
    return out;
}

}  // namespace nova
