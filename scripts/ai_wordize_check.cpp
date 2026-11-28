// ---------------------------------------------------------------------------
// ai_wordize_check.cpp - the Linux harness for docsy's retrieval tokenizers.
//
// Compiles AiWordize / AiIsStopword / AiQueryTerms straight out of
// src/ai_index.inl (extracted verbatim by sed - the real functions, not a
// copy) and asserts what the sidechat's lexical pool is built from: an
// Arabic word must survive tokenization as its own UTF-8 bytes, an Arabic
// question must produce terms, and ASCII folding must stay as it was.
//
//   sed -n '/word normalization (QuoteLocator/,/^\/\/ LocateInChunk/p' src/ai_index.inl > /tmp/ai_tok1.inc
//   sed -n '/static bool AiIsStopword/,/^static double AiCosine/p' src/ai_index.inl | head -n -2 > /tmp/ai_tok2.inc
//   g++ -std=c++20 -O1 -I src -o /tmp/ai_wordize_check scripts/ai_wordize_check.cpp
//   /tmp/ai_wordize_check
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "ai_tok1.inc"
#include "ai_tok2.inc"

static int g_failed = 0, g_total = 0;
static void Expect(bool ok, const char* what) {
    g_total++;
    if (!ok) { g_failed++; printf("  FAIL: %s\n", what); }
    else printf("  ok: %s\n", what);
}
static bool Has(const std::vector<std::string>& v, const char* s) {
    for (auto& x : v) if (x == s) return true;
    return false;
}

int main() {
    printf("AiWordize (the chunk/quote tokenizer):\n");
    std::vector<std::string> w;
    AiWordize("\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9 49 \xD9\x85\xD9\x86 \xD8\xA7\xD9\x84\xD9\x82\xD8\xA7\xD9\x86\xD9\x88\xD9\x86", w);
    // "المادة 49 من القانون" - an Arabic legal phrase
    Expect(w.size() == 4, "an Arabic phrase keeps every word");
    Expect(Has(w, "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9"), "المادة rides as its own UTF-8 bytes");
    Expect(Has(w, "\xD8\xA7\xD9\x84\xD9\x82\xD8\xA7\xD9\x86\xD9\x88\xD9\x86"), "القانون survives");
    Expect(Has(w, "49"), "ASCII digits still tokenize");
    AiWordize("The King's 1948 Law, l'article 74.", w);
    // ASCII behavior is byte-for-byte the old rule: only whitespace splits,
    // other ASCII punctuation is absorbed into the word ("King's" read as
    // "kings" before this fix and reads the same after)
    Expect(Has(w, "the") && Has(w, "kings") && Has(w, "1948") &&
           Has(w, "law") && Has(w, "larticle") && Has(w, "74"),
           "ASCII folding and glue unchanged (regression)");
    AiWordize("caf\xC3\xA9 r\xC3\xA9sum\xC3\xA9", w);
    Expect(Has(w, "caf\xC3\xA9") && Has(w, "r\xC3\xA9sum\xC3\xA9"), "accented Latin survives");
    // "\xD8\xA7\xD9\x84\xD9\x88\xD8\xAB\xD9\x8A\xD9\x82\xD8\xA9\xD8\x9F" = الوثيقة؟ - the Arabic question
    // mark rides on the word's tail in real text and must not glue into it
    AiWordize("\xD8\xA7\xD9\x84\xD9\x88\xD8\xAB\xD9\x8A\xD9\x82\xD8\xA9\xD8\x9F", w);
    Expect(Has(w, "\xD8\xA7\xD9\x84\xD9\x88\xD8\xAB\xD9\x8A\xD9\x82\xD8\xA9"), "a trailing Arabic ؟ splits off the word");
    AiWordize("\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9\xD8\x8C \xD8\xA7\xD9\x84\xD8\xAE\xD8\xA7\xD9\x85\xD8\xB3\xD8\xA9", w);
    Expect(Has(w, "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9"), "a trailing Arabic \xD8\x8C splits off the word");

    printf("AiQueryTerms (the question-side tokenizer):\n");
    std::vector<std::string> t;
    // "ما هو دور الملك في الوثيقة؟" - "what is the king's role in the document?"
    AiQueryTerms("\xD9\x85\xD8\xA7 \xD9\x87\xD9\x88 \xD8\xAF\xD9\x88\xD8\xB1 \xD8\xA7\xD9\x84\xD9\x85\xD9\x84\xD9\x83 \xD9\x81\xD9\x8A \xD8\xA7\xD9\x84\xD9\x88\xD8\xAB\xD9\x8A\xD9\x82\xD8\xA9\xD8\x9F", t);
    Expect(!t.empty(), "an Arabic question produces terms (the old code produced none)");
    Expect(Has(t, "\xD8\xA7\xD9\x84\xD9\x85\xD9\x84\xD9\x83"), "الملك (the king) is a term");
    Expect(Has(t, "\xD8\xA7\xD9\x84\xD9\x88\xD8\xAB\xD9\x8A\xD9\x82\xD8\xA9"), "الوثيقة (the document) is a term");
    AiQueryTerms("What is the King's role in the document?", t);
    Expect(Has(t, "king") && Has(t, "role") && Has(t, "document") && !Has(t, "the") &&
           !Has(t, "what") && !Has(t, "is"),
           "English question: stopwords dropped, content kept (regression)");

    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
