// NovaChangJie engine tests. Portable: builds with MSVC, GCC or Clang and
// uses a tiny in-memory dictionary, so no data download is needed.
#include <cstdio>
#include <sstream>
#include <string>

#include "DictBuilder.h"
#include "Dictionary.h"
#include "Engine.h"
#include "PhraseCode.h"
#include "Unicode.h"

using namespace nova;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace {

KeyInput Letter(char c) {
    KeyInput k;
    k.key = Key::Char;
    k.ch = static_cast<char32_t>(c);
    k.letter = c;
    return k;
}

KeyInput Digit(int d, bool shift = false) {
    KeyInput k;
    k.key = Key::Char;
    k.ch = shift ? U")!@#$%^&*("[d] : static_cast<char32_t>('0' + d);
    k.digit = d;
    k.shift = shift;
    return k;
}

KeyInput Ch(char32_t c) {
    KeyInput k;
    k.key = Key::Char;
    k.ch = c;
    return k;
}

KeyInput Special(Key key) {
    KeyInput k;
    k.key = key;
    return k;
}

void AddSampleEntries(DictBuilder& b) {
    const uint16_t han = kEntryHan | kEntryBig5;
    b.AddChar(u"日", {"a"}, {"a"}, han, 900, 5000);
    b.AddChar(u"曰", {"a"}, {"a"}, han, 800, 100);
    b.AddChar(u"明", {"ab"}, {"ab"}, han, 700, 4000);
    b.AddChar(u"倉", {"oiar"}, {"oiar"}, han, 600, 300, u"仓");
    b.AddChar(u"頡", {"grmbc"}, {"grmbc"}, han, 500, 200, u"颉");
    b.AddChar(u"中", {"l"}, {"l"}, han, 950, 9000);
    b.AddChar(u"國", {"wirm"}, {"wirm"}, han, 400, 6000, u"国");
    b.AddChar(u"文", {"yk"}, {"yk"}, han, 400, 7000);
    b.AddChar(u"𠀀", {"mnl"}, {"mnl"}, kEntryHan, 0, 0);  // Ext-B, not Big5.
    b.AddChar(u"欠", {"no"}, {"nok"}, han, 300, 3000);  // Different 三代/五代 codes.
    b.AddChar(u"，", {"zxab"}, {"zxab"}, kEntryPunct, 0, 0);
    b.AddPhrase(u"倉頡", 1000);
    b.AddPhrase(u"中國", 50000);
    b.AddPhrase(u"中文", 30000);
    b.AddPhrase(u"曰國", 900000);  // Makes 'a' convert to 曰 before 國.
}

std::vector<char> SampleDictionary() {
    DictBuilder b;
    AddSampleEntries(b);
    return b.Build();
}

void TestPhraseCode() {
    CHECK(DerivePhraseCode({"oiar", "grmbc"}) == "orgc");
    CHECK(DerivePhraseCode({"l", "wirm"}) == "llwm");
    CHECK(DerivePhraseCode({"a", "b", "cd"}) == "abcd");
    CHECK(DerivePhraseCode({"ab", "cd", "ef", "gh", "ij"}) == "acegj");
    CHECK(DerivePhraseCode({"xab", "cd"}).empty());
}

void TestWildcard() {
    CHECK(WildcardMatch("a*", "abc"));
    CHECK(WildcardMatch("a*c", "abc"));
    CHECK(WildcardMatch("a*c", "ac"));
    CHECK(!WildcardMatch("a*d", "abc"));
    CHECK(WildcardMatch("*b*", "abc"));
}

void TestDictionary(const Dictionary& d) {
    CHECK(d.IsLoaded());
    auto [b, e] = d.Exact(kIndexCj5, "a");
    CHECK(e - b == 2);
    CHECK(d.Text(d.Entry(b->entry)) == u"日");  // Higher traditional order first.
    auto [lb, le] = d.Longer(kIndexCj5, "a");
    CHECK(le - lb == 1);
    auto [pb, pe] = d.Exact(kIndexPhrase, "orgc");
    CHECK(pe - pb == 1 && d.Text(d.Entry(pb->entry)) == u"倉頡");
    CHECK(d.Simplified(d.Entry(pb->entry)) == u"仓颉");
    CHECK(d.FindChar(U'明') != nullptr);
    auto [ab, ae] = d.Associations(U'中');
    CHECK(ae - ab == 2 && d.Text(d.Entry(ab->entry)) == u"中國");

    std::vector<char> junk(64, 'x');
    Dictionary bad;
    CHECK(!bad.Load(junk.data(), junk.size()));
}

Engine MakeEngine(const Dictionary& d, UserModel& user, Options o = {}, bool sentence = false) {
    Engine e;
    e.SetDictionary(&d);
    e.SetUserModel(&user);
    o.sentenceMode = sentence;
    o.learning = false;
    o.association = false;
    e.SetOptions(o);
    return e;
}

void TestBasicTyping(const Dictionary& d) {
    UserModel user;
    Engine e = MakeEngine(d, user);
    CHECK(e.WouldConsume(Letter('a')));
    CHECK(!e.IsComposing());  // WouldConsume must not change state.
    e.Process(Letter('a'), 1);
    CHECK(e.IsComposing());
    CHECK(e.CompositionText() == u"日");
    CHECK(e.Candidates().size() >= 3);
    CHECK(e.Candidates()[0].text == u"日");  // Frequency order.
    CHECK(e.Candidates()[2].hint == "b");    // Predicted 明 shows its last key.
    auto out = e.Process(Special(Key::Space), 1);
    CHECK(out.commit == u"日");
    CHECK(!e.IsComposing());

    e.Process(Letter('a'), 1);
    out = e.Process(Digit(2), 1);
    CHECK(out.commit == u"曰");

    e.Process(Letter('a'), 1);
    out = e.Process(Special(Key::Escape), 1);
    CHECK(out.consumed && out.commit.empty() && !e.IsComposing());

    e.Process(Letter('a'), 1);
    out = e.Process(Special(Key::Enter), 1);
    CHECK(out.commit == u"a");
}

void TestPhrasesAndCharset(const Dictionary& d) {
    UserModel user;
    Engine e = MakeEngine(d, user);
    for (char c : std::string("orgc")) e.Process(Letter(c), 1);
    CHECK(e.Candidates().empty());  // Dictionary words have no word codes (詞語輸入 was removed).
    auto out = e.Process(Special(Key::Space), 1);  // A typo: beep and drop the code.
    CHECK(out.consumed && out.beep && out.commit.empty() && !e.IsComposing());

    Options o;

    Engine all = MakeEngine(d, user);
    for (char c : std::string("mnl")) all.Process(Letter(c), 1);
    CHECK(all.Candidates().size() == 1);
    o = Options();
    o.charset = Charset::Big5;
    Engine big5 = MakeEngine(d, user, o);
    for (char c : std::string("mnl")) big5.Process(Letter(c), 1);
    CHECK(big5.Candidates().empty());
}

void TestSimplifiedOutput(const Dictionary& d) {
    UserModel user;
    Options o;
    o.outputSimplified = true;
    Engine e = MakeEngine(d, user, o);
    for (char c : std::string("oiar")) e.Process(Letter(c), 1);
    auto out = e.Process(Special(Key::Space), 1);
    CHECK(out.commit == u"仓");
}

void TestWildcardInput(const Dictionary& d) {
    UserModel user;
    Engine e = MakeEngine(d, user);
    e.Process(Letter('o'), 1);
    e.Process(Ch('*'), 1);
    e.Process(Letter('r'), 1);
    CHECK(e.CompositionText() == u"人＊口");
    CHECK(!e.Candidates().empty() && e.Candidates()[0].text == u"倉");
    CHECK(e.Candidates()[0].hint == "[oiar]");  // Full code shown for a wildcard match.

    Options o;
    o.quick = true;
    Engine q = MakeEngine(d, user, o);
    q.Process(Letter('o'), 1);
    q.Process(Ch('*'), 1);
    CHECK(q.Candidates().empty() || q.Candidates()[0].hint.empty());  // Not in 速成.
}

void TestOverflowAndPunctuation(const Dictionary& d) {
    UserModel user;
    Engine off = MakeEngine(d, user);
    for (char c : std::string("grmbc")) off.Process(Letter(c), 1);
    auto out = off.Process(Letter('a'), 1);  // Off by default: the sixth key is ignored.
    CHECK(out.commit.empty() && off.Buffer() == "grmbc");

    Options o;
    o.autoCommitOnOverflow = true;
    Engine e = MakeEngine(d, user, o);
    for (char c : std::string("grmbc")) e.Process(Letter(c), 1);
    out = e.Process(Letter('a'), 1);  // Sixth key commits 頡.
    CHECK(out.commit == u"頡");
    CHECK(e.Buffer() == "a");

    out = e.Process(Ch(','), 1);  // Punctuation commits 日 then inserts ，.
    CHECK(out.commit == u"日，");
    CHECK(!e.IsComposing());

    CHECK(!e.Process(Ch('_'), 1).consumed);  // Underscore stays an underscore.
    e.Process(Letter('a'), 1);
    out = e.Process(Ch('_'), 1);
    CHECK(out.commit == u"日_");

    out = e.Process(Ch('"'), 1);
    CHECK(out.commit == u"“");
    out = e.Process(Ch('"'), 1);
    CHECK(out.commit == u"”");
}

void TestModes(const Dictionary& d) {
    UserModel user;
    Engine e = MakeEngine(d, user);
    e.Process(Letter('a'), 1);
    auto out = e.SetChinese(false);
    CHECK(out.commit == u"a" && !e.IsComposing());
    CHECK(!e.WouldConsume(Letter('a')));
    e.SetFullShape(true);
    out = e.Process(Ch('A'), 1);
    CHECK(out.commit == u"Ａ");  // 英 全形.
    out = e.Process(Special(Key::Space), 1);
    CHECK(out.commit == u"　");
}

void TestAssociationAndLearning(const Dictionary& d) {
    UserModel user;
    Engine e;
    e.SetDictionary(&d);
    e.SetUserModel(&user);
    Options o;
    o.sentenceMode = false;
    e.SetOptions(o);  // Learning and association on.

    e.Process(Letter('l'), 100);
    auto out = e.Process(Special(Key::Space), 100);
    CHECK(out.commit == u"中");
    CHECK(e.IsAssociation());
    CHECK(e.Candidates().size() == 2 && e.Candidates()[0].text == u"國");
    out = e.Process(Digit(2, true), 100);  // Shift+2 picks 文.
    CHECK(out.commit == u"文");

    // Learning: choosing 曰 puts it first next time.
    e.Reset();
    e.Process(Letter('a'), 200);
    e.Process(Digit(2), 200);
    e.Reset();
    e.Process(Letter('a'), 300);
    CHECK(e.Candidates()[0].text == u"曰");

    UserModel copy;
    copy.MergeHistory(user.SerializeHistory());
    UserModel::Usage u;
    CHECK(copy.Lookup(u"曰", &u) && u.count == 1 && u.lastUsed == 200);

    // Private fields (InPrivate windows): nothing is learned.
    e.SetPrivate(true);
    e.Reset();
    e.Process(Letter('a'), 250);
    e.Process(Digit(2), 250);  // 日, now second.
    e.SetPrivate(false);
    UserModel::Usage day;
    CHECK(!user.Lookup(u"日", &day));

    // Sentence mode: Enter commits and offers associations; the next Enter
    // (a new line) is passed to the app and closes them.
    o.sentenceMode = true;
    e.SetOptions(o);
    e.Reset();
    e.Process(Letter('l'), 400);
    e.Process(Special(Key::Space), 400);
    out = e.Process(Special(Key::Enter), 400);
    CHECK(out.commit == u"中" && e.IsAssociation());
    CHECK(!e.WouldConsume(Special(Key::Enter)));
    out = e.Process(Special(Key::Enter), 400);
    CHECK(!out.consumed && !e.IsAssociation() && !e.CandidatesVisible());
}

void TestUserPhrases(const Dictionary& d) {
    auto phrases = UserModel::ParseUserPhrases("# comment\n明日\n倉頡中文 qq\n\xF0\x9F\x98\x80\n", d);
    CHECK(phrases.size() == 2);
    CHECK(phrases[0].text == u"明日" && phrases[0].code5 == "abaa");
    CHECK(phrases[1].shortcut == "qq" && !phrases[1].code5.empty());  // Shortcut plus word code.
    auto chosen = UserModel::ParseUserPhrases("明日 @5:ab,a\n明明 zz @5:a\n", d);
    CHECK(chosen.size() == 2 && chosen[0].code5 == "abaa" && chosen[1].shortcut == "zz");
    CHECK(chosen[1].code5 == "aaab");  // First 明 forced to code "a", second uses "ab".
    UserModel user;
    user.SetUserPhrases(phrases);
    Engine e = MakeEngine(d, user);
    e.Process(Letter('q'), 1);
    e.Process(Letter('q'), 1);
    CHECK(!e.Candidates().empty() && e.Candidates()[0].text == u"倉頡中文");
}

void TestSentenceMode(const Dictionary& d) {
    UserModel user;
    Options o;
    o.autoCommitOnOverflow = true;  // For the sixth-key check below.
    Engine e = MakeEngine(d, user, o, true);
    e.Process(Letter('a'), 1);
    CHECK(e.IsComposing() && !e.CandidatesVisible());  // Like 新注音: no list while typing.
    auto out = e.Process(Special(Key::Space), 1);
    CHECK(out.consumed && out.commit.empty());
    CHECK(e.CompositionText() == u"日" && e.CaretOffset() == 1);

    for (char c : std::string("wirm")) e.Process(Letter(c), 1);
    CHECK(e.CompositionText() == u"日田戈口一");
    e.Process(Special(Key::Space), 1);
    CHECK(e.CompositionText() == u"曰國");  // Context changed 日 to 曰.

    e.Process(Special(Key::Home), 1);
    e.Process(Special(Key::Down), 1);  // Choices at slot 0: 曰國, 日, 曰.
    CHECK(e.CandidatesVisible() && e.Candidates().size() == 3);
    CHECK(e.Candidates()[0].text == u"曰國" && e.Candidates()[0].span == 2);
    CHECK(e.Selection() == 0);  // Starts on the current word, 曰國.
    e.Process(Digit(2), 1);     // Pick 日.
    CHECK(e.CompositionText() == u"日國" && e.Cursor() == 1);

    out = e.Process(Special(Key::Enter), 1);
    CHECK(out.commit == u"日國" && !e.IsComposing());

    // Punctuation commits the sentence first.
    e.Process(Letter('l'), 1);
    e.Process(Special(Key::Space), 1);
    out = e.Process(Ch(','), 1);
    CHECK(out.commit == u"中，");

    // Sixth key places the character instead of committing it.
    for (char c : std::string("grmbca")) e.Process(Letter(c), 1);
    CHECK(e.SentenceText() == u"頡" && e.Buffer() == "a");
    e.Process(Special(Key::Escape), 1);  // Clears the code only.
    CHECK(e.SentenceText() == u"頡" && e.Buffer().empty());
    e.Process(Special(Key::Backspace), 1);
    CHECK(!e.IsComposing());

    // A code with no character: beep, drop it, keep the sentence.
    e.Process(Letter('l'), 1);
    e.Process(Special(Key::Space), 1);
    for (char c : std::string("enxi")) e.Process(Letter(c), 1);
    out = e.Process(Special(Key::Space), 1);
    CHECK(out.beep && e.SentenceText() == u"中" && e.Buffer().empty() && e.IsComposing());
    e.Process(Special(Key::Escape), 1);

    // Wildcard codes stay open, so the word decides: a* matches 日, 明, 曰.
    Engine x = MakeEngine(d, user, Options(), true);
    x.Process(Letter('a'), 1);
    x.Process(Ch('*'), 1);
    x.Process(Special(Key::Space), 1);
    CHECK(x.SentenceText() == u"日");
    x.Process(Letter('w'), 1);
    x.Process(Ch('*'), 1);
    x.Process(Letter('m'), 1);
    x.Process(Special(Key::Space), 1);
    CHECK(x.SentenceText() == u"曰國");
}

void TestMixedVersions(const Dictionary& d) {
    UserModel user;
    auto first = [&](CangjieVersion v, const char* code) {
        Options o;
        o.version = v;
        o.prediction = false;  // Only exact codes.
        Engine e = MakeEngine(d, user, o);
        for (const char* c = code; *c; ++c) e.Process(Letter(*c), 1);
        return e.Candidates().empty() ? std::u16string() : e.Candidates()[0].text;
    };
    CHECK(first(CangjieVersion::V5, "no") == u"欠");
    CHECK(first(CangjieVersion::V5, "nok").empty());
    CHECK(first(CangjieVersion::V3, "nok") == u"欠");
    CHECK(first(CangjieVersion::V3, "no").empty());
    CHECK(first(CangjieVersion::Mixed, "no") == u"欠");   // 三五代混打: either code works.
    CHECK(first(CangjieVersion::Mixed, "nok") == u"欠");

    // Sentence mode too.
    Options o;
    o.version = CangjieVersion::Mixed;
    Engine s = MakeEngine(d, user, o, true);
    for (char c : std::string("nok")) s.Process(Letter(c), 1);
    s.Process(Special(Key::Space), 1);
    for (char c : std::string("l")) s.Process(Letter(c), 1);
    s.Process(Special(Key::Space), 1);
    CHECK(s.SentenceText() == u"欠中");
}

void TestQuick(const Dictionary& d) {
    // 速成: first + last radical. 頡 grmbc -> gc, 倉 oiar -> or, 國 wirm -> wm.
    UserModel user;
    Options o;
    o.quick = true;
    o.prediction = false;
    Engine e = MakeEngine(d, user, o);
    for (char c : std::string("gc")) e.Process(Letter(c), 1);
    CHECK(!e.Candidates().empty() && e.Candidates()[0].text == u"頡");
    e.Reset();
    for (char c : std::string("grmbc")) e.Process(Letter(c), 1);  // Full codes do not apply.
    CHECK(e.Candidates().empty() || e.Candidates()[0].text != u"頡");

    // A third key places the character and starts the next one.
    Engine s = MakeEngine(d, user, o, true);
    for (char c : std::string("orgc")) s.Process(Letter(c), 1);
    s.Process(Special(Key::Space), 1);
    CHECK(s.SentenceText() == u"倉頡");
    auto out = s.Process(Special(Key::Enter), 1);
    CHECK(out.commit == u"倉頡");
}

void TestPrefixKey(const Dictionary& d) {
    UserModel user;
    user.SetUserPhrases(UserModel::ParseUserPhrases("倉頡中文 qq\n明日 qw\n", d));
    Engine e = MakeEngine(d, user, Options(), true);
    e.SetBig5Decoder([](uint16_t b) -> char32_t { return b == 0xA4A4 ? U'中' : 0; });
    auto type = [&](const char* s) {
        EngineOutput all;
        for (const char* c = s; *c; ++c) {
            KeyInput k = *c >= 'a' && *c <= 'z' ? Letter(*c)
                         : *c >= '0' && *c <= '9' ? Digit(*c - '0')
                                                  : Ch(static_cast<char32_t>(*c));
            if (*c >= 'A' && *c <= 'Z') {
                k = Letter(static_cast<char>(*c - 'A' + 'a'));
                k.ch = static_cast<char32_t>(*c);
                k.shift = true;
            }
            if (*c == ' ') k = Special(Key::Space);
            all.commit += e.Process(k, 1).commit;
        }
        return all.commit;
    };

    // ` alone lists the functions.
    CHECK(type("`").empty() && e.InPrefix() && e.CompositionText() == u"`");
    CHECK(e.Candidates().size() == 4 && e.Candidates()[0].label == u"U");
    e.Process(Special(Key::Escape), 1);
    CHECK(!e.IsComposing());

    // Unicode: Space commits; six digits commit by themselves.
    type("`U4e2d");
    CHECK(e.CompositionText() == u"`U4E2D" && e.Candidates().size() == 1);
    CHECK(e.Process(Special(Key::Space), 1).commit == u"中");
    CHECK(!e.IsComposing());
    CHECK(type("`u020000") == u"\U00020000");
    CHECK(type("`uD800").empty() && e.Candidates().empty());  // A surrogate is not a character.
    e.Process(Special(Key::Escape), 1);

    // Big5: four digits commit.
    CHECK(type("`ba4a4") == u"中");
    CHECK(type("`b0041").empty() && e.InPrefix());  // Not a Big5 code: stays open.
    e.Process(Special(Key::Backspace), 1);
    CHECK(e.CompositionText() == u"`B004");
    for (int i = 0; i < 4; ++i) e.Process(Special(Key::Backspace), 1);
    CHECK(e.CompositionText() == u"`");
    e.Process(Special(Key::Backspace), 1);
    CHECK(!e.IsComposing());

    // Shortcuts from 使用者造詞.
    type("`pq");
    CHECK(e.Candidates().size() == 2 && e.Candidates()[0].text == u"倉頡中文");
    CHECK(type("2") == u"明日");
    type("`pqq");
    CHECK(e.Process(Special(Key::Enter), 1).commit == u"倉頡中文");

    // Punctuation: the full-width form.
    CHECK(type("`,") == u"，");
    CHECK(type("`.") == u"。");  // 句號, not the full-width full stop ．
    CHECK(type("`[") == u"［");

    // ` after a sentence commits the sentence first.
    type("a ");
    CHECK(e.SentenceText() == u"日");
    CHECK(type("`") == u"日" && e.InPrefix());
    e.Process(Special(Key::Escape), 1);
}

// A sentence scorer that prefers sentences containing a given character.
class FakeScorer : public SentenceScorer {
public:
    explicit FakeScorer(char16_t liked) : liked_(liked) {}
    bool Score(const std::vector<std::u16string>& sentences, std::vector<float>* logProbs) override {
        ++calls;
        logProbs->clear();
        for (const auto& s : sentences) logProbs->push_back(s.find(liked_) != std::u16string::npos ? 50.0f : 0.0f);
        return true;
    }
    int calls = 0;

private:
    char16_t liked_;
};

void TestWordPairs() {
    // Training text where 曰 is always followed by 文: the pair beats the
    // far more common 日 that a unigram model picks.
    DictBuilder b;
    AddSampleEntries(b);
    std::istringstream text("曰文\n曰文\n曰文\n");
    std::string error;
    CHECK(b.LoadText(text, 1.0, BuildOptions(), &error));
    std::vector<char> blob = b.Build(BuildOptions());
    Dictionary d;
    CHECK(d.Load(blob.data(), blob.size(), &error));
    CHECK(d.HasWordPairs());
    float lp = 0;
    CHECK(d.PairLogProb(d.FindCharId(U'曰'), d.FindCharId(U'文'), &lp) && lp > -1.0f);
    CHECK(!d.PairLogProb(d.FindCharId(U'日'), d.FindCharId(U'文'), &lp));

    auto type = [](Engine& e) {
        e.Process(Letter('a'), 1);
        e.Process(Special(Key::Space), 1);
        e.Process(Letter('y'), 1);
        e.Process(Letter('k'), 1);
        e.Process(Special(Key::Space), 1);
    };
    UserModel user;
    Engine e = MakeEngine(d, user, Options(), true);
    type(e);
    CHECK(e.SentenceText() == u"曰文");
    CHECK(e.Alternatives().size() >= 2 && e.Alternatives()[0].text == u"曰文");

    Options noPairs;
    noPairs.wordPairs = false;
    Engine u = MakeEngine(d, user, noPairs, true);
    type(u);
    CHECK(u.SentenceText() == u"日文");

    // A sentence scorer (the neural model) can overrule the pairs.
    FakeScorer scorer(u'日');
    Engine n = MakeEngine(d, user, Options(), true);
    n.SetSentenceScorer(&scorer);
    type(n);
    CHECK(scorer.calls > 0 && n.SentenceText() == u"日文");
    Options noNeural;
    noNeural.neuralRerank = false;
    Engine nn = MakeEngine(d, user, noNeural, true);
    nn.SetSentenceScorer(&scorer);
    type(nn);
    CHECK(nn.SentenceText() == u"曰文");

    // Learning: committing a sentence records its words and word pairs.
    Engine l = MakeEngine(d, user, Options(), true);
    Options learn = l.GetOptions();  // MakeEngine turns learning off.
    learn.learning = true;
    l.SetOptions(learn);
    type(l);
    CHECK(l.Process(Special(Key::Enter), 1).commit == u"曰文");
    CHECK(user.Lookup(u"曰", nullptr) && user.Lookup(u"曰 文", nullptr));
}

// Characters the user picked never change, whatever comes after them: not
// through word context, word pairs, learning or the neural scorer.
void TestManualChoiceKept(const Dictionary& d) {
    UserModel user;
    FakeScorer prefersYue(u'曰');  // Pushes every conversion towards 曰.
    auto indexOf = [](const Engine& e, std::u16string_view text) {
        for (size_t i = 0; i < e.Candidates().size(); ++i)
            if (e.Candidates()[i].text == text) return i;
        return SIZE_MAX;
    };
    auto typeGuo = [](Engine& e) {  // 國, which makes 'a' convert to 曰 (word 曰國).
        for (char c : std::string("wirm")) e.Process(Letter(c), 1);
        e.Process(Special(Key::Space), 1);
    };

    // Without a choice, context changes 日 to 曰.
    Engine free = MakeEngine(d, user, Options(), true);
    free.Process(Letter('a'), 1);
    free.Process(Special(Key::Space), 1);
    typeGuo(free);
    CHECK(free.SentenceText() == u"曰國");

    // Picked by number / mouse while typing the code.
    Engine e = MakeEngine(d, user, Options(), true);
    e.SetSentenceScorer(&prefersYue);
    e.Process(Letter('a'), 1);
    size_t ri = indexOf(e, u"日");
    CHECK(ri != SIZE_MAX);
    e.Select(ri, 1);
    typeGuo(e);
    CHECK(e.SentenceText() == u"日國");

    // Picked by moving the highlight to 曰, then Space: stays 曰 although
    // both the dictionary and the scorer prefer 日 before 文.
    FakeScorer prefersRi(u'日');
    Options shown;
    shown.codeCandidates = true;
    Engine h = MakeEngine(d, user, shown, true);
    h.SetSentenceScorer(&prefersRi);
    h.Process(Letter('a'), 1);
    size_t yue = indexOf(h, u"曰");
    CHECK(yue != SIZE_MAX && yue > 0);
    while (h.Selection() < yue) h.Process(Special(Key::Down), 1);
    h.Process(Special(Key::Space), 1);
    h.Process(Letter('y'), 1);
    h.Process(Letter('k'), 1);
    h.Process(Special(Key::Space), 1);
    CHECK(h.SentenceText() == u"曰文");

    // Picked with ↓ in the sentence, then more typing and editing around it.
    Engine c = MakeEngine(d, user, Options(), true);
    c.SetSentenceScorer(&prefersYue);
    c.Process(Letter('a'), 1);
    c.Process(Special(Key::Space), 1);
    typeGuo(c);
    CHECK(c.SentenceText() == u"曰國");
    c.Process(Special(Key::Home), 1);
    c.Process(Special(Key::Down), 1);  // Choices at 曰.
    size_t at = SIZE_MAX;
    for (size_t i = 0; i < c.Candidates().size(); ++i)
        if (c.Candidates()[i].text == u"日" && c.Candidates()[i].span == 1) at = i;
    CHECK(at != SIZE_MAX);
    c.Select(at, 1);
    CHECK(c.SentenceText() == u"日國");
    c.Process(Special(Key::End), 1);
    for (char ch : std::string("wirm")) c.Process(Letter(ch), 1);
    c.Process(Special(Key::Space), 1);
    c.Process(Special(Key::Backspace), 1);  // Edit after the choice.
    CHECK(c.SentenceText() == u"日國");
    // A whole word chosen with ↓ keeps all its characters.
    c.Process(Special(Key::Home), 1);
    c.Process(Special(Key::Down), 1);
    size_t word = SIZE_MAX;
    for (size_t i = 0; i < c.Candidates().size(); ++i)
        if (c.Candidates()[i].text == u"曰國" && c.Candidates()[i].span == 2) word = i;
    CHECK(word != SIZE_MAX);
    c.Select(word, 1);
    CHECK(c.SentenceText() == u"曰國");
    CHECK(c.Process(Special(Key::Enter), 1).commit == u"曰國");
}

}  // namespace

int main() {
    TestPhraseCode();
    TestWildcard();
    std::vector<char> blob = SampleDictionary();
    Dictionary dict;
    std::string error;
    CHECK(dict.Load(blob.data(), blob.size(), &error));
    TestDictionary(dict);
    TestBasicTyping(dict);
    TestPhrasesAndCharset(dict);
    TestSimplifiedOutput(dict);
    TestWildcardInput(dict);
    TestOverflowAndPunctuation(dict);
    TestModes(dict);
    TestAssociationAndLearning(dict);
    TestUserPhrases(dict);
    TestSentenceMode(dict);
    TestMixedVersions(dict);
    TestQuick(dict);
    TestPrefixKey(dict);
    TestWordPairs();
    TestManualChoiceKept(dict);
    if (g_failures == 0) std::printf("All engine tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
