// ncj-eval: measures how often sentence mode (智慧選字) picks the right
// characters. Each test phrase is typed as its characters' codes, one Space
// per character, and the composed sentence is compared with the original.
//
//   ncj-eval --dict nova.dict --text heldout.txt [--quick] [--cj3]
//            [--unigram] [--neural] [--limit N] [--show N] [--dump alternatives.tsv]
//            [--beam N] [--alts N] [--first N] [--per-word N]
//
// --neural re-ranks with the running NovaChangJieLM helper (Windows only).
// --dump writes "truth<TAB>text<TAB>score..." with the engine's best few
// conversions of every phrase, for re-scoring experiments.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Dictionary.h"
#include "Engine.h"
#include "Unicode.h"
#ifdef NOVA_EVAL_NEURAL
#include "LmClient.h"
#endif

using namespace nova;

namespace {

int Usage() {
    std::cerr << "usage: ncj-eval --dict <nova.dict> --text <phrases.txt> [--quick] [--cj3]\n"
                 "                [--unigram] [--neural] [--limit <n>] [--show <n>] [--dump <file>]\n"
                 "                [--beam <n>] [--alts <n>] [--first <n>] [--per-word <n>]\n";
    return 2;
}

KeyInput LetterKey(char c) {
    KeyInput k;
    k.key = Key::Char;
    k.ch = static_cast<char32_t>(c);
    k.letter = c;
    return k;
}

KeyInput SpaceKey() {
    KeyInput k;
    k.key = Key::Space;
    return k;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dictPath, textPath, dumpPath;
    bool quick = false, cj3 = false, unigram = false, neural = false;
    size_t limit = SIZE_MAX, show = 0, beam = 0, alts = 0, first = 0, perWord = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--dict") dictPath = value();
        else if (arg == "--text") textPath = value();
        else if (arg == "--quick") quick = true;
        else if (arg == "--cj3") cj3 = true;
        else if (arg == "--unigram") unigram = true;
        else if (arg == "--neural") neural = true;
        else if (arg == "--limit") limit = std::strtoul(value().c_str(), nullptr, 10);
        else if (arg == "--show") show = std::strtoul(value().c_str(), nullptr, 10);
        else if (arg == "--dump") dumpPath = value();
        else if (arg == "--beam") beam = std::strtoul(value().c_str(), nullptr, 10);
        else if (arg == "--alts") alts = std::strtoul(value().c_str(), nullptr, 10);
        else if (arg == "--first") first = std::strtoul(value().c_str(), nullptr, 10);
        else if (arg == "--per-word") perWord = std::strtoul(value().c_str(), nullptr, 10);
        else return Usage();
    }
    if (dictPath.empty() || textPath.empty()) return Usage();

    std::ifstream dictIn(dictPath, std::ios::binary);
    std::vector<char> blob((std::istreambuf_iterator<char>(dictIn)), std::istreambuf_iterator<char>());
    Dictionary dict;
    std::string error;
    if (!dict.Load(blob.data(), blob.size(), &error)) {
        std::cerr << dictPath << ": " << error << "\n";
        return 1;
    }
    std::ifstream textIn(textPath, std::ios::binary);
    if (!textIn) {
        std::cerr << "cannot open " << textPath << "\n";
        return 1;
    }

    UserModel user;
    Options o;
    o.sentenceMode = true;
    o.learning = false;
    o.association = false;
    o.quick = quick;
    o.version = cj3 ? CangjieVersion::V3 : CangjieVersion::V5;
    o.wordPairs = !unigram;
    if (beam) o.sentenceBeam = beam;
    if (alts) o.sentenceAlternatives = alts;
    if (first) o.sentenceFirstChars = first;
    if (perWord) o.sentenceBeamPerWord = perWord;
    Engine engine;
    engine.SetDictionary(&dict);
    engine.SetUserModel(&user);
    engine.SetOptions(o);
#ifdef NOVA_EVAL_NEURAL
    LmClient lm(1000);  // Generous: measure accuracy, not latency.
    if (neural) {
        std::vector<float> probe;
        if (!lm.Score({u"測試"}, &probe)) {
            std::cerr << "NovaChangJieLM is not running\n";
            return 1;
        }
        engine.SetSentenceScorer(&lm);
    }
#else
    if (neural) {
        std::cerr << "--neural needs the Windows build\n";
        return 1;
    }
#endif

    std::ofstream dump;
    if (!dumpPath.empty()) dump.open(dumpPath, std::ios::binary | std::ios::trunc);
    size_t phrases = 0, exact = 0, chars = 0, correct = 0, skipped = 0, shown = 0;
    std::string line;
    while (phrases < limit && std::getline(textIn, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::u16string truth = Utf8ToUtf16(line);
        std::vector<char32_t> cps = CodePoints(truth);
        if (cps.empty() || cps.size() > Engine::kMaxSentence) continue;

        // Every character needs a code; otherwise the phrase cannot be typed.
        std::vector<std::string> codes;
        for (char32_t cp : cps) {
            const DictEntry* e = dict.FindChar(cp);
            std::string code = e ? std::string(CodeView(cj3 ? e->cj3 : e->cj5)) : std::string();
            if (quick && code.size() > 2) code = std::string{code.front(), code.back()};
            if (code.empty()) break;
            codes.push_back(code);
        }
        if (codes.size() != cps.size()) {
            ++skipped;
            continue;
        }

        engine.Reset();
        for (const std::string& code : codes) {
            for (char c : code) engine.Process(LetterKey(c), 0);
            engine.Process(SpaceKey(), 0);
        }
        std::vector<char32_t> got = CodePoints(engine.SentenceText());
        if (dump.is_open()) {
            dump << line;
            for (const Engine::Alternative& a : engine.Alternatives()) dump << '\t' << Utf16ToUtf8(a.text) << '\t' << a.score;
            dump << '\n';
        }
        size_t ok = 0;
        for (size_t i = 0; i < cps.size(); ++i)
            if (i < got.size() && got[i] == cps[i]) ++ok;
        ++phrases;
        chars += cps.size();
        correct += ok;
        if (ok == cps.size() && got.size() == cps.size()) {
            ++exact;
        } else if (shown < show) {
            ++shown;
            std::cout << line << "\t" << Utf16ToUtf8(engine.SentenceText()) << "\n";
        }
    }
    engine.Reset();
    if (phrases == 0) {
        std::cerr << "no phrases\n";
        return 1;
    }
    std::printf("%s%s: %zu phrases, %zu characters (%zu skipped)\n", quick ? "quick" : "cangjie",
                cj3 ? "3" : "5", phrases, chars, skipped);
    std::printf("  characters correct: %.2f%%\n", 100.0 * correct / chars);
    std::printf("  phrases correct:    %.2f%%\n", 100.0 * exact / phrases);
    return 0;
}
