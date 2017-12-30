#include <algorithm>
#include <cassert>

#include "spell_corrector.hpp"

namespace NOpenSpell {


static std::vector<std::wstring> GetDeletes1(const std::wstring& w) {
    std::vector<std::wstring> results;
    for (size_t i = 0; i < w.size(); ++i) {
        auto nw = w.substr(0, i) + w.substr(i+1);
        if (!nw.empty()) {
            results.push_back(nw);
        }
    }
    return results;
}

static std::vector<std::wstring> GetDeletes2(const std::wstring& w) {
    std::vector<std::wstring> results;
    for (size_t i = 0; i < w.size(); ++i) {
        auto nw = w.substr(0, i) + w.substr(i+1);
        if (!nw.empty()) {
            std::vector<std::wstring> currResults = GetDeletes1(nw);
            results.insert(results.end(), currResults.begin(), currResults.end());
        }
    }
    return results;
}

bool TSpellCorrector::LoadLangModel(const std::string& modelFile) {
    if (!LangModel.Load(modelFile)) {
        return false;
    }
    PrepareCache();
    return true;
}

bool TSpellCorrector::TrainLangModel(const std::string& textFile, const std::string& alphabetFile) {
    if (!LangModel.LoadAlphabet(alphabetFile)) {
        return false;
    }
    std::cerr << "[info] loading text\n";
    std::wstring trainText = UTF8ToWide(LoadFile(textFile));
    ToLower(trainText);
    std::cerr << "[info] tokenizing\n";
    TSentences trainSentences = LangModel.Tokenize(trainText);
    TSentences testSentences;
    std::cerr << "[info] prepare test sentences\n";
    size_t testPart = std::min(size_t(0.2 * trainSentences.size()), size_t(5000));
    size_t trainPart = trainSentences.size() - testPart;
    for (size_t i = trainPart; i < trainSentences.size(); ++i) {
        testSentences.push_back(trainSentences[i]);
    }
    trainSentences.resize(trainPart);
    std::cerr << "[info] training model\n";
    LangModel.TrainRaw(trainSentences);
    std::cerr << "[info] prepare cache\n";
    PrepareCache();

    std::cerr << "[info] calc broken percent\n";
    double broken = FindPenalty(testSentences);

    std::cerr << "penalty: " << broken << "\n";

//    if (!LangModel.Train(textFile, alphabetFile)) {
//        return false;
//    }
//    PrepareCache();
    return true;
}

struct TScoredWord {
    TWord Word;
    double Score = 0;
};

TWords TSpellCorrector::GetCandidatesRaw(const TWords& sentence, size_t position) const {
    if (position >= sentence.size()) {
        return TWords();
    }

    TWord w = sentence[position];
    TWords candidates = Edits2(w);

    bool firstLevel = true;
    if (candidates.empty()) {
        candidates = Edits(w, false);
        firstLevel = false;
    }

    {
        TWord c = LangModel.GetWord(std::wstring(w.Ptr, w.Len));
        if (c.Ptr && c.Len) {
            w = c;
            candidates.push_back(c);
        }
    }

    if (candidates.empty()) {
        return candidates;
    }

    std::unordered_set<TWord, TWordHashPtr> uniqueCandidates(candidates.begin(), candidates.end());

    std::vector<TScoredWord> scoredCandidates;
    scoredCandidates.reserve(candidates.size());
    for (TWord cand: uniqueCandidates) {
        TWords candSentence;
        for (size_t i = 0; i < sentence.size(); ++i) {
            if (i == position) {
                candSentence.push_back(cand);
            } else if ((i < position && i + 3 >= position) ||
                       (i > position && i <= position + 3))
            {
                candSentence.push_back(sentence[i]);
            }
        }

        TScoredWord scored;
        scored.Word = cand;
        scored.Score = LangModel.Score(candSentence);
        if (!(scored.Word == w)) {
            if (firstLevel) {
                scored.Score -= Penalty;
            } else {
                scored.Score *= 50.0;
            }
        }
        scoredCandidates.push_back(scored);
    }

    std::sort(scoredCandidates.begin(), scoredCandidates.end(), [](TScoredWord w1, TScoredWord w2) {
        return w1.Score > w2.Score;
    });

    candidates.clear();
    for (auto s: scoredCandidates) {
        candidates.push_back(s.Word);
    }
    return candidates;
}

std::vector<std::wstring> TSpellCorrector::GetCandidates(const std::vector<std::wstring>& sentence, size_t position) const {
    TWords words;
    for (auto&& w: sentence) {
        words.push_back(TWord(w));
    }
    TWords candidates = GetCandidatesRaw(words, position);
    std::vector<std::wstring> results;
    for (auto&& c: candidates) {
        results.push_back(std::wstring(c.Ptr, c.Len));
    }
    return results;
}

std::wstring TSpellCorrector::FixFragment(const std::wstring& text) const {
    TSentences origSentences = LangModel.Tokenize(text);
    std::wstring lowered = text;
    ToLower(lowered);
    TSentences sentences = LangModel.Tokenize(lowered);
    std::wstring result;
    size_t origPos = 0;
    for (size_t i = 0; i < sentences.size(); ++i) {
        TWords words = sentences[i];
        const TWords& origWords = origSentences[i];
        for (size_t j = 0; j < words.size(); ++j) {
            TWord orig = origWords[j];
            TWord lowered = words[j];
            TWords candidates = GetCandidatesRaw(words, j);
            if (candidates.size() > 0) {
                words[j] = candidates[0];
            }
            size_t currOrigPos = orig.Ptr - &text[0];
            while (origPos < currOrigPos) {
                result.push_back(text[origPos]);
                origPos += 1;
            }
            std::wstring newWord = std::wstring(words[j].Ptr, words[j].Len);
            std::wstring origWord = std::wstring(orig.Ptr, orig.Len);
            std::wstring origLowered = std::wstring(lowered.Ptr, lowered.Len);
            if (newWord != origLowered) {
                for (size_t k = 0; k < newWord.size(); ++k) {
                    size_t n = k < origWord.size() ? k : origWord.size() - 1;
                    wchar_t newChar = newWord[k];
                    wchar_t origChar = origWord[n];
                    result.push_back(MakeUpperIfRequired(newChar, origChar));
                }
            } else {
                result += origWord;
            }
            origPos += orig.Len;
        }
    }
    while (origPos < text.size()) {
        result.push_back(text[origPos]);
        origPos += 1;
    }
    return result;
}

std::wstring TSpellCorrector::FixFragmentNormalized(const std::wstring& text) const {
    std::wstring lowered = text;
    ToLower(lowered);
    TSentences sentences = LangModel.Tokenize(lowered);
    std::wstring result;
    for (size_t i = 0; i < sentences.size(); ++i) {
        TWords words = sentences[i];
        for (size_t i = 0; i < words.size(); ++i) {
            TWords candidates = GetCandidatesRaw(words, i);
            if (candidates.size() > 0) {
                words[i] = candidates[0];
            }
            result += std::wstring(words[i].Ptr, words[i].Len) + L" ";
        }
        if (words.size() > 0) {
            result.resize(result.size() - 1);
            result += L". ";
        }
    }
    if (!result.empty()) {
        result.resize(result.size() - 1);
    }
    return result;
}

template<typename T>
inline void AddVec(T& target, const T& source) {
    target.insert(target.end(), source.begin(), source.end());
}

TWords TSpellCorrector::Edits(const TWord& word, bool lastLevel) const {
    std::wstring w(word.Ptr, word.Len);
    TWords result;

    std::vector<std::wstring> cands = GetDeletes1(w);
    cands.push_back(w);
    if (!lastLevel) {
        std::vector<std::wstring> deletes2 = GetDeletes2(w);
        cands.insert(cands.end(), deletes2.begin(), deletes2.end());
    }

    for (auto&& w: cands) {
        TWord c = LangModel.GetWord(w);
        if (c.Ptr && c.Len) {
            result.push_back(c);
        }
        std::string s = WideToUTF8(w);
        auto it = Deletes1.find(s);
        if (it != Deletes1.end()) {
            for (auto c1:it->second) {
                result.push_back(LangModel.GetWordById(c1));
            }
        }
        if (!lastLevel) {
            auto jt = Deletes2.find(s);
            if (jt != Deletes2.end()) {
                for (auto c1:jt->second) {
                    result.push_back(LangModel.GetWordById(c1));
                }
            }
        }
    }

    return result;
}

TWords TSpellCorrector::Edits2(const TWord& word, bool lastLevel) const {
    std::wstring w(word.Ptr, word.Len);
    TWords result;

    for (size_t i = 0; i < w.size() + 1; ++i) {
        // delete
        if (i < w.size()) {
            std::wstring s = w.substr(0, i) + w.substr(i+1);
            TWord c = LangModel.GetWord(s);
            if (c.Ptr && c.Len) {
                result.push_back(c);
            }
            if (!lastLevel) {
                AddVec(result, Edits2(TWord(s)));
            }
        }

        // transpose
        if (i + 1 < w.size()) {
            std::wstring s = w.substr(0, i);
            s += w.substr(i + 1, 1);
            s += w.substr(i, 1);
            if (i + 2 < w.size()) {
                s += w.substr(i+2);
            }
            TWord c = LangModel.GetWord(s);
            if (c.Ptr && c.Len) {
                result.push_back(c);
            }
            if (!lastLevel) {
                AddVec(result, Edits2(TWord(s)));
            }
        }

        // replace
        if (i < w.size()) {
            for (auto&& ch: LangModel.GetAlphabet()) {
                std::wstring s = w.substr(0, i) + ch + w.substr(i+1);
                TWord c = LangModel.GetWord(s);
                if (c.Ptr && c.Len) {
                    result.push_back(c);
                }
                if (!lastLevel) {
                    AddVec(result, Edits2(TWord(s)));
                }
            }
        }

        // inserts
        {
            for (auto&& ch: LangModel.GetAlphabet()) {
                std::wstring s = w.substr(0, i) + ch + w.substr(i);
                TWord c = LangModel.GetWord(s);
                if (c.Ptr && c.Len) {
                    result.push_back(c);
                }
                if (!lastLevel) {
                    AddVec(result, Edits2(TWord(s)));
                }
            }
        }
    }

    return result;
}

void TSpellCorrector::PrepareCache() {
    Deletes1.clear();
    Deletes2.clear();
    auto&& wordToId = LangModel.GetWordToId();
    for (auto&& it: wordToId) {
        TWordId wid = LangModel.GetWordIdNoCreate(it.first);
        auto deletes1 = GetDeletes1(it.first);
        auto deletes2 = GetDeletes2(it.first);
        for (auto&& w: deletes1) {
            Deletes1[WideToUTF8(w)].push_back(wid);
        }
        for (auto&& w: deletes2) {
            Deletes2[WideToUTF8(w)].push_back(wid);
        }
    }
}

double TSpellCorrector::FindPenalty(const TSentences& sentences) {
    double a = 0.0;
    double b = 500.0;
    double target = 0.007;

    while (b - a >= 0.2) {
        double c = a + (b - a) * 0.5;
        double pc = GetBrokenPercent(sentences, c);
        std::cerr << "[info] penalty: " << c << ", broken: " << pc << "\n";
        if (pc <= target) {
            b = c;
        } else {
            a = c;
        }
    }

    return b;
}

double TSpellCorrector::GetBrokenPercent(const TSentences& sentences, double penalty) {
    assert(!sentences.empty());
    Penalty = penalty;
    size_t totalWords = 0;
    size_t broken = 0;
    TWords candidates;
    for (size_t i = 0; i < sentences.size(); ++i) {
        const TWords& sentence = sentences[i];
        for (size_t j = 0; j < sentence.size(); ++j) {
            const TWord& w = sentence[j];
            candidates = GetCandidatesRaw(sentence, j);
            totalWords += 1;
            if (!candidates.empty()) {
                const TWord& c = candidates[0];
                std::wstring origWord(w.Ptr, w.Len);
                std::wstring candWord(c.Ptr, c.Len);
                if (origWord != candWord) {
                    broken += 1;
                }
            }
        }
    }
    return double(broken) / double(totalWords);
}


} // NOpenSpell
