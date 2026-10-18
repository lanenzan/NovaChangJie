// ncj-dictbuild: compile character table + word corpus into nova.dict.
//
//   ncj-dictbuild --table table.txt [--corpus essay.txt] [--min-weight N]
//                 [--max-chars N] [[--weight W] --text phrases.txt ...]
//                 [--min-pairs N] [--extra-words words.txt [--extra-scale S]]
//                 --out nova.dict
//
// --text adds running text for the word pair model; --weight W counts the
// following --text files W times (default 1). --extra-words adds words the
// dictionary lacks from a second word list (weights times S, default 0.1).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "DictBuilder.h"
#include "Dictionary.h"

namespace {

int Usage() {
    std::cerr << "usage: ncj-dictbuild --table <table.txt> [--corpus <words.txt>]\n"
                 "                     [--min-weight <n>] [--max-chars <n>]\n"
                 "                     [[--weight <w>] --text <phrases.txt> ...] [--min-pairs <n>]\n"
                 "                     [--extra-words <words.txt> [--extra-scale <s>]]\n"
                 "                     --out <nova.dict>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string table, corpus, extraWords, outPath;
    std::vector<std::pair<std::string, double>> texts;
    double weight = 1;
    nova::BuildOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--table") table = value();
        else if (arg == "--corpus") corpus = value();
        else if (arg == "--out") outPath = value();
        else if (arg == "--min-weight") options.minPhraseWeight = static_cast<uint32_t>(std::strtoul(value().c_str(), nullptr, 10));
        else if (arg == "--max-chars") options.maxPhraseChars = std::strtoul(value().c_str(), nullptr, 10);
        else if (arg == "--weight") weight = std::strtod(value().c_str(), nullptr);
        else if (arg == "--text") texts.emplace_back(value(), weight);
        else if (arg == "--min-pairs") options.minPairCount = std::strtod(value().c_str(), nullptr);
        else if (arg == "--extra-words") extraWords = value();
        else if (arg == "--extra-scale") options.extraWordScale = std::strtod(value().c_str(), nullptr);
        else return Usage();
    }
    if (table.empty() || outPath.empty()) return Usage();

    nova::DictBuilder builder;
    std::string error;
    std::ifstream tableIn(table, std::ios::binary);
    if (!tableIn) {
        std::cerr << "cannot open " << table << "\n";
        return 1;
    }
    if (!builder.LoadCharTable(tableIn, &error)) {
        std::cerr << table << ": " << error << "\n";
        return 1;
    }
    if (!corpus.empty()) {
        std::ifstream corpusIn(corpus, std::ios::binary);
        if (!corpusIn) {
            std::cerr << "cannot open " << corpus << "\n";
            return 1;
        }
        if (!builder.LoadCorpus(corpusIn, options, &error)) {
            std::cerr << corpus << ": " << error << "\n";
            return 1;
        }
    }

    if (!extraWords.empty()) {
        std::ifstream extraIn(extraWords, std::ios::binary);
        if (!extraIn || !builder.LoadExtraWords(extraIn, options, &error)) {
            std::cerr << extraWords << ": " << (extraIn ? error : "cannot open") << "\n";
            return 1;
        }
    }
    // Text last: it is split into the words loaded above.
    for (const auto& [path, w] : texts) {
        std::ifstream textIn(path, std::ios::binary);
        if (!textIn) {
            std::cerr << "cannot open " << path << "\n";
            return 1;
        }
        if (!builder.LoadText(textIn, w, options, &error)) {
            std::cerr << path << ": " << error << "\n";
            return 1;
        }
    }

    std::vector<char> blob = builder.Build(options);
    nova::Dictionary check;
    if (!check.Load(blob.data(), blob.size(), &error)) {
        std::cerr << "internal error: built dictionary does not validate: " << error << "\n";
        return 1;
    }
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (!out) {
        std::cerr << "cannot write " << outPath << "\n";
        return 1;
    }
    std::cout << "nova.dict: " << builder.CharCount() << " characters, " << builder.PhraseCount()
              << " words, " << builder.PairCount() << " word pairs seen, " << blob.size() / 1024 << " KiB\n";
    return 0;
}
