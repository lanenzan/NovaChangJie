// NovaChangJie - input engine. Platform independent: it turns abstract key
// presses into composition state, candidate lists and committed text. The
// Windows text service only translates keys and renders the results.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Dictionary.h"
#include "Options.h"
#include "UserModel.h"

namespace nova {

enum class Key {
    Char,  // Printable character; see KeyInput::ch.
    Space,
    Enter,
    Escape,
    Backspace,
    Tab,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    Delete,
    Other,
};

struct KeyInput {
    Key key = Key::Other;
    char32_t ch = 0;   // Character produced by the keyboard layout (Key::Char).
    char letter = 0;   // 'a'..'z' when the physical key is a letter key.
    int digit = -1;    // 0..9 when the physical key is a digit key.
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool capsLock = false;
};

struct Candidate {
    enum class Kind { Char, Phrase, UserPhrase, Association };
    std::u16string text;    // What is shown and inserted.
    std::u16string source;  // Traditional form used for learning / association.
    std::string code;       // Full code of the candidate.
    std::string hint;       // Remaining keys for a predicted candidate.
    std::u16string label;   // Shown instead of the 1..9 selection number, if set.
    Kind kind = Kind::Char;
    size_t span = 1;        // Sentence mode: characters this choice covers.
};

struct EngineOutput {
    bool consumed = false;     // The key was handled by the engine.
    std::u16string commit;     // Text to insert into the document.
    bool modeChanged = false;  // Chinese / full shape / punctuation changed.
    bool beep = false;         // A code with no character was confirmed and dropped.
};

// Scores whole sentences with a stronger language model than the
// dictionary's (the neural model in the NovaChangJieLM helper). Sentence
// mode uses it to re-rank its best few conversions.
class SentenceScorer {
public:
    virtual ~SentenceScorer() = default;
    // log P(sentence) for each sentence, or false if no score is available
    // (then sentence mode keeps its own order).
    virtual bool Score(const std::vector<std::u16string>& sentences, std::vector<float>* logProbs) = 0;
};

// Big5 code (e.g. 0xA4A4) to Unicode, or 0 if it is not a character. The
// engine has no Big5 table; the platform supplies one.
using Big5Decoder = char32_t (*)(uint16_t big5);

class Engine {
public:
    static constexpr size_t kMaxCodeLetters = 5;
    static constexpr size_t kMaxQuickLetters = 2;  // 速成
    static constexpr size_t kMaxBuffer = 7;  // Letters plus '*' wildcards.
    static constexpr size_t kMaxSentence = 20;  // Longer sentences commit their start.
    static constexpr char32_t kPrefixKey = '`';  // 多功能前導字元鍵

    void SetDictionary(const Dictionary* dict) { dict_ = dict; }
    void SetUserModel(UserModel* user) { user_ = user; }
    void SetBig5Decoder(Big5Decoder decoder) { big5Decoder_ = decoder; }
    void SetSentenceScorer(SentenceScorer* scorer) { scorer_ = scorer; }
    void SetOptions(const Options& options);
    const Options& GetOptions() const { return options_; }

    // Handle one key press. `now` is the Unix time used for learning.
    EngineOutput Process(const KeyInput& key, int64_t now);
    // Whether Process() would consume the key, without changing any state.
    bool WouldConsume(const KeyInput& key) const;

    // Select a candidate by absolute index (mouse click, UI element).
    EngineOutput Select(size_t index, int64_t now);
    // Commit the highlighted candidate (if any) and end composition.
    EngineOutput CommitSelection(int64_t now);
    void SetSelection(size_t index);
    void SetPage(size_t page);
    // Discard composition and association list.
    void Reset();

    bool Chinese() const { return chinese_; }
    bool FullShape() const { return fullShape_; }
    bool ChinesePunct() const { return chinesePunct_; }
    // Switching modes while composing commits the typed letters as text.
    EngineOutput SetChinese(bool on);
    void SetFullShape(bool on) { fullShape_ = on; }
    void SetChinesePunct(bool on) { chinesePunct_ = on; }
    // Private fields (e.g. an InPrivate browser window): nothing typed is learned.
    void SetPrivate(bool on) { private_ = on; }

    bool IsComposing() const { return !buffer_.empty() || !slots_.empty() || prefix_ != Prefix::None; }
    // The prefix key (`) was pressed: U, B, P or a punctuation key follows.
    bool InPrefix() const { return prefix_ != Prefix::None; }
    const std::string& Buffer() const { return buffer_; }
    // Sentence mode: converted characters with the code being typed (as
    // radicals, e.g. 人戈口) at the cursor. Otherwise just the radicals.
    std::u16string CompositionText() const;
    // Caret position inside CompositionText(), in UTF-16 units.
    size_t CaretOffset() const;
    // Sentence mode: the converted text (without the code being typed).
    std::u16string SentenceText() const;
    size_t SentenceLength() const { return slots_.size(); }
    size_t Cursor() const { return cursor_; }
    // Whether the candidate list should be shown. In sentence mode the list
    // for a half-typed code stays hidden (like 新注音) unless enabled.
    bool CandidatesVisible() const;

    // Sentence mode: the best few conversions of the whole sentence (best
    // first, distinct texts), with their language model scores.
    struct Segment {  // A word of a conversion, covering slots [start, start + length).
        size_t start = 0;
        size_t length = 0;
        std::u16string text;
    };
    struct Alternative {
        std::u16string text;
        double score = 0;
        std::vector<Segment> segments;
    };
    const std::vector<Alternative>& Alternatives() const { return alternatives_; }

    bool IsAssociation() const { return association_; }
    bool HasCandidates() const { return !candidates_.empty(); }
    const std::vector<Candidate>& Candidates() const { return candidates_; }
    size_t Selection() const { return selection_; }
    size_t PageSize() const { return options_.pageSize; }
    size_t PageStart() const { return selection_ / options_.pageSize * options_.pageSize; }
    size_t PageCount() const;
    size_t CurrentPage() const { return selection_ / options_.pageSize; }

    static char16_t RadicalFor(char letter);
    static std::u16string ToFullWidth(std::u16string_view s);

private:
    EngineOutput ProcessComposing(const KeyInput& key, int64_t now);
    EngineOutput ProcessIdle(const KeyInput& key, int64_t now);
    EngineOutput ProcessAssociation(const KeyInput& key, int64_t now);
    EngineOutput ProcessSentence(const KeyInput& key, int64_t now);
    EngineOutput ProcessPrefix(const KeyInput& key, int64_t now);

    void AppendKey(char c, int64_t now, EngineOutput& out);
    void Recompute();
    bool AutoCommitAllowed() const;
    // Whether committed text is recorded for learning.
    bool Records() const { return !dryRun_ && options_.learning && user_ && !private_; }
    // Character code indexes to search: one, or both for 三五代混打.
    std::vector<IndexId> CharIndices() const;
    size_t MaxCodeLetters() const { return options_.quick ? kMaxQuickLetters : kMaxCodeLetters; }
    void CommitCandidate(size_t index, int64_t now, EngineOutput& out);
    void ShowAssociations(std::u16string_view committed);
    void ClearComposition();
    void MoveSelection(long delta);
    bool AllowedByCharset(const DictEntry& e) const;
    Candidate MakeCandidate(const DictEntry& e, Candidate::Kind kind, std::string_view code) const;
    std::u16string PunctuationFor(char32_t ch);
    std::u16string Output(std::u16string_view s) const;  // Applies full shape.

    // Sentence mode (智慧選字, like 新注音).
    struct SlotChoice {
        std::u16string text;  // One character (traditional form).
        uint32_t weight = 0;
        uint32_t entry = UINT32_MAX;  // Dictionary entry id.
    };
    struct Slot {
        std::string code;
        std::u16string fixed;  // Character chosen by the user, if any.
        std::vector<SlotChoice> choices;
    };
    bool MakeSlot(const std::string& code, const std::u16string& fixed, Slot* slot) const;
    // Moves a candidate of the code being typed into the sentence.
    void InsertCandidate(size_t index, bool explicitChoice);
    void AfterInsert(int64_t now, EngineOutput& out);
    void Convert();
    // Words matching slots from `start` on (choices only, ignoring fixes).
    struct WordMatch {
        const DictEntry* entry;
        size_t length;
    };
    std::vector<WordMatch> MatchWords(size_t start, bool useFixed, size_t maxFirstChars) const;
    void OpenSlotCandidates();
    void ChooseSlotCandidate(size_t index);
    void CommitSentence(int64_t now, EngineOutput& out, bool associate);
    void CommitLeading(size_t minSlots, int64_t now, EngineOutput& out);
    void ClearSentence();
    // Learns the first `count` words of the conversion and their pairs.
    void RecordSegments(size_t count, int64_t now);
    uint32_t CharWeight(std::u16string_view ch) const;
    uint32_t CharEntry(std::u16string_view ch) const;  // UINT32_MAX if unknown.
    std::u16string ToOutputText(std::u16string_view source) const;

    const Dictionary* dict_ = nullptr;
    UserModel* user_ = nullptr;
    Options options_;
    bool dryRun_ = false;
    Big5Decoder big5Decoder_ = nullptr;
    SentenceScorer* scorer_ = nullptr;

    bool chinese_ = true;
    bool fullShape_ = false;
    bool chinesePunct_ = true;
    bool private_ = false;
    bool singleQuoteOpen_ = false;
    bool doubleQuoteOpen_ = false;

    std::string buffer_;
    bool association_ = false;
    std::vector<Candidate> candidates_;
    size_t exactCount_ = 0;  // Candidates matching the full code exactly.
    size_t selection_ = 0;

    // 多功能前導字元鍵: ` then U (Unicode), B (Big5), P (捷徑) or punctuation.
    enum class Prefix { None, Menu, Unicode, Big5, Shortcut };
    void EnterPrefix(Prefix mode);
    void RecomputePrefix();
    void CommitPrefix(size_t index, int64_t now, EngineOutput& out);
    std::u16string PrefixText() const;  // What was typed, e.g. "`U4E2D".
    Prefix prefix_ = Prefix::None;
    std::string prefixCode_;  // Hex digits or shortcut letters after the mode key.

    std::vector<Slot> slots_;
    std::vector<std::u16string> converted_;  // Chosen character per slot.
    std::vector<Segment> segments_;          // Words of the best conversion.
    std::vector<Alternative> alternatives_;
    size_t cursor_ = 0;                      // Insertion point, 0..slots_.size().
    bool selecting_ = false;                 // List of choices at a slot is open.
    size_t selectPos_ = 0;
};

}  // namespace nova
