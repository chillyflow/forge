#!/usr/bin/env python3
"""Generate the spike 004 patch from the pinned llama.cpp tarball.

The patch is not hand-assembled: this script extracts the two files from the
exact tarball the build fetches (pin bb4caa754), applies each edit as a unique
substring replacement (asserting exactly one match, so a silent drift is
impossible), and emits the unified diff. Reviewing this script is reviewing the
patch.

Usage:  python make-patch.py [--tarball PATH] [--out PATH]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

PIN = "bb4caa7540188872173c44d161602d9271386413"
FILES = ("src/llama-grammar.h", "src/llama-grammar.cpp")

HEADER_EDIT = (
    """// string, and the grammar will be given the string from the first match group onwards.

};""",
    """// string, and the grammar will be given the string from the first match group onwards.

    // FORGE PATCH (spike 004): dense per-token first-byte table, so the mask can
    // reject a candidate before touching the per-token piece cache. Spike 003
    // measured the full-vocabulary mask at ~200 ns/candidate, memory-latency
    // bound. 0..255 = first byte of an ASCII-leading piece; 0xFF = the candidate
    // must be decided by the full decoder.
    std::vector<uint8_t> first_byte;
};""",
)

HELPER = """// FORGE PATCH (spike 004): dense first-byte table, built once per grammar.
// 0xFF marks a token whose candidate must go through the full decoder.
static void llama_grammar_build_first_byte(struct llama_grammar & grammar) {
    if (!grammar.vocab) {
        return;
    }
    const int32_t n_vocab = llama_vocab_n_tokens(grammar.vocab);
    grammar.first_byte.assign((size_t) n_vocab, 0xFF);
    for (int32_t id = 0; id < n_vocab; ++id) {
        const std::string & piece = grammar.vocab->token_to_piece(id);
        if (!piece.empty() && piece[0] != 0 && (unsigned char) piece[0] < 0x80) {
            grammar.first_byte[(size_t) id] = (uint8_t) piece[0];
        }
    }
}

struct llama_grammar * llama_grammar_init_impl(
        const struct llama_vocab * vocab,
        const llama_grammar_element ** rules,
        size_t n_rules,
        size_t start_rule_index) {"""

BUILDER1 = """    llama_grammar * grammar = new llama_grammar {
        vocab,
        std::move(vec_rules),
        std::move(stacks),
        /* .partial_utf8 = */             {},
        /* .lazy = */                     false,
        /* .awaiting_trigger = */         false,
        /* .trigger_buffer = */           "",
        /* .trigger_buffer_positions = */ {},
        /* .trigger_tokens = */           {},
        /* .trigger_patterns = */         {},
        /* .first_byte = */               {},
    };

    // FORGE PATCH (spike 004): build the first-byte table with the grammar.
    llama_grammar_build_first_byte(*grammar);

    return grammar;
}"""

BUILDER2 = """    llama_grammar * grammar = new llama_grammar {
        vocab,
        std::move(vec_rules),
        std::move(stacks),
        /* .partial_utf8 = */             {},
        /* .lazy = */                     lazy,
        /* .awaiting_trigger = */         lazy,
        /* .trigger_buffer = */           "",
        /* .trigger_buffer_positions = */ {},
        std::move(vec_trigger_tokens),
        std::move(vec_trigger_patterns),
        /* .first_byte = */               {},
    };

    // FORGE PATCH (spike 004): build the first-byte table with the grammar.
    llama_grammar_build_first_byte(*grammar);

    return grammar;
}"""

PREFILTER = """    // FORGE PATCH (spike 004): a candidate survives only if its first code point
    // is accepted by some stack position. For an ASCII first byte that is the test
    // llama_grammar_match_char() performs, so run it on a dense byte table before
    // touching the per-token piece cache. Skipped when a pending partial sequence
    // makes the first byte a continuation byte, or when a stack position matches
    // candidates by token id.
    bool prefilter = !grammar.first_byte.empty() && grammar.partial_utf8.n_remain == 0;
    uint8_t allowed_bytes[32] = {0};
    llama_token allowed_ids[8] = {0};
    size_t allowed_id_count = 0;
    if (prefilter) {
        for (const auto & stack : grammar.stacks) {
            if (stack.empty()) {
                continue; // an empty stack admits end-of-generation only
            }
            const llama_grammar_element * top = stack.back();
            if (top->type == LLAMA_GRETYPE_TOKEN) {
                // a token position admits exactly one token id
                if (allowed_id_count < 8) {
                    allowed_ids[allowed_id_count++] = (llama_token) top->value;
                } else {
                    prefilter = false;
                    break;
                }
                continue;
            }
            if (top->type == LLAMA_GRETYPE_TOKEN_NOT) {
                // admits every token except one, so the byte test rejects nothing
                prefilter = false;
                break;
            }
            for (uint32_t byte = 0; byte < 256; byte++) {
                if (llama_grammar_match_char(top, byte).first) {
                    allowed_bytes[byte >> 3] |= (uint8_t) (1u << (byte & 7));
                }
            }
        }
    }

    std::vector<std::pair<std::vector<uint32_t>, llama_partial_utf8>> candidates_decoded;
    candidates_decoded.reserve(cur_p->size);"""

LOOP = """    for (size_t i = 0; i < cur_p->size; ++i) {
        const llama_token id       = cur_p->data[i].id;
        const bool       is_eog_id = grammar.vocab->is_eog(id);

        if (prefilter && !is_eog_id) {
            const uint8_t first = grammar.first_byte[(size_t) id];
            bool allowed = first != 0xFF &&
                           (allowed_bytes[first >> 3] & (uint8_t) (1u << (first & 7))) != 0;
            for (size_t k = 0; !allowed && k < allowed_id_count; k++) {
                allowed = allowed_ids[k] == id;
            }
            if (!allowed) {
                cur_p->data[i].logit = -INFINITY;
                continue;
            }
        }

        const std::string & piece = grammar.vocab->token_to_piece(id);

        if (is_eog_id) {"""

CPP_EDITS = [
    ("""struct llama_grammar * llama_grammar_init_impl(
        const struct llama_vocab * vocab,
        const llama_grammar_element ** rules,
        size_t n_rules,
        size_t start_rule_index) {""", HELPER),
    ("""    return new llama_grammar {
        vocab,
        std::move(vec_rules),
        std::move(stacks),
        /* .partial_utf8 = */             {},
        /* .lazy = */                     false,
        /* .awaiting_trigger = */         false,
        /* .trigger_buffer = */           "",
        /* .trigger_buffer_positions = */ {},
        /* .trigger_tokens = */           {},
        /* .trigger_patterns = */         {},
    };
}""", BUILDER1),
    ("""    return new llama_grammar {
        vocab,
        std::move(vec_rules),
        std::move(stacks),
        /* .partial_utf8 = */             {},
        /* .lazy = */                     lazy,
        /* .awaiting_trigger = */         lazy,
        /* .trigger_buffer = */           "",
        /* .trigger_buffer_positions = */ {},
        std::move(vec_trigger_tokens),
        std::move(vec_trigger_patterns),
    };
}""", BUILDER2),
    ("""        grammar.trigger_tokens,
        grammar.trigger_patterns,
    };""", """        grammar.trigger_tokens,
        grammar.trigger_patterns,
        grammar.first_byte,
    };"""),
    ("""    std::vector<std::pair<std::vector<uint32_t>, llama_partial_utf8>> candidates_decoded;
    candidates_decoded.reserve(cur_p->size);""", PREFILTER),
    ("""    for (size_t i = 0; i < cur_p->size; ++i) {
        const llama_token id      = cur_p->data[i].id;
        const std::string & piece = grammar.vocab->token_to_piece(id);

        if (grammar.vocab->is_eog(id)) {""", LOOP),
]


def crlf(text):
    return text.replace("\n", "\r\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarball", default=None)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "llama-grammar-firstbyte-prefilter.patch"))
    args = ap.parse_args()

    tarball = args.tarball
    if not tarball:
        for base in ("build-src", "build"):
            cand = os.path.join(base, "_deps", "llama-subbuild", "llama-populate-prefix",
                                "src", PIN + ".tar.gz")
            if os.path.exists(cand):
                tarball = os.path.abspath(cand)
                break
    if not tarball or not os.path.exists(tarball):
        print("cannot find the pinned tarball; pass --tarball", file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="llama-004-")
    os.makedirs(os.path.join(tmp, "orig", "src"))
    os.makedirs(os.path.join(tmp, "mod", "src"))
    with tarfile.open(tarball, "r:gz") as archive:
        for f in FILES:
            stream = archive.extractfile("llama.cpp-" + PIN + "/" + f)
            if stream is None:
                print("missing in tarball: " + f, file=sys.stderr)
                return 2
            data = stream.read()
            for variant in ("orig", "mod"):
                target = os.path.join(tmp, variant, "src", os.path.basename(f))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as handle:
                    handle.write(data)

    def edit(name, pairs):
        path = os.path.join(tmp, "mod", "src", name)
        text = open(path, encoding="utf-8", newline="").read()
        for i, (old, new) in enumerate(pairs):
            count = text.count(old)
            if count != 1:
                raise AssertionError(f"{name} edit {i}: {count} matches for {old[:70]!r}")
            text = text.replace(old, new)
        open(path, "w", encoding="utf-8", newline="").write(text)
        print(f"{name}: {len(pairs)} edits applied, each matched exactly once")

    edit("llama-grammar.h", [HEADER_EDIT])
    edit("llama-grammar.cpp", CPP_EDITS)

    raw = subprocess.run(["git", "diff", "--no-index", "orig", "mod"], cwd=tmp,
                         capture_output=True, text=True).stdout
    fixed = []
    for line in raw.splitlines(keepends=True):
        line = line.replace("diff --git a/orig/", "diff --git a/").replace(" b/mod/", " b/")
        line = line.replace("--- a/orig/", "--- a/").replace("+++ b/mod/", "+++ b/")
        fixed.append(line)
    with open(args.out, "w", encoding="utf-8", newline="") as handle:
        handle.write("".join(fixed))
    print("wrote", args.out)
    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
