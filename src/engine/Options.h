// NovaChangJie - user-configurable behaviour shared by the engine, the text
// service and the settings application.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nova {

enum class CangjieVersion : uint32_t { V5 = 0, V3 = 1, Mixed = 2 };  // Mixed: 三五代混打
enum class Charset : uint32_t { All = 0, Big5Hkscs = 1, Big5 = 2 };
enum class CandidateOrder : uint32_t { Frequency = 0, Traditional = 1 };
enum class CandidateLayout : uint32_t { Vertical = 0, Horizontal = 1 };

struct Options {
    CangjieVersion version = CangjieVersion::V5;
    Charset charset = Charset::All;
    CandidateOrder order = CandidateOrder::Frequency;
    CandidateLayout layout = CandidateLayout::Vertical;
    uint32_t pageSize = 9;        // 5..9
    uint32_t fontSize = 18;       // Points at 96 DPI scale.

    bool sentenceMode = true;     // 智慧選字: compose whole sentences like 新注音.
    bool wordPairs = true;        // 智慧選字 scores which words follow which (bigram model).
    bool neuralRerank = true;     // 智慧選字 asks the neural model (NovaChangJieLM) to re-rank.
    bool codeCandidates = false;  // Sentence mode: list candidates while typing a code.
    bool quick = false;           // 速成: first + last radical. Set by the active profile; not saved.
    bool association = true;      // 聯想詞 after committing.
    bool prediction = true;       // Show characters whose code begins with the input.
    bool autoCommitUnique = false;  // Commit when only one candidate can match.
    bool autoCommitOnOverflow = false;  // 6th key commits the first candidate.
    bool learning = true;         // Adaptive candidate order.
    bool showLetters = false;     // Show "oiar" instead of 人戈口.
    bool outputSimplified = false;  // 輸出簡體中文.
    bool shiftToggles = true;     // Tap Shift to switch Chinese / English.
    bool startInChinese = true;
    bool startFullShape = false;
    bool chinesePunctuation = true;
    bool showCodeHints = true;    // Show remaining keys for predicted candidates.
    bool prefixHints = true;      // List what can follow the ` prefix key.

    // Sentence search size (not saved; ncj-eval changes them for experiments).
    size_t sentenceBeam = 16;         // Partial sentences kept per position.
    size_t sentenceBeamPerWord = 16;  // ... of which at most this many end in the same word.
    size_t sentenceAlternatives = 16; // Whole sentences re-ranked by the neural model.
    size_t sentenceFirstChars = 32;   // Most likely first characters searched for words.
};

}  // namespace nova
