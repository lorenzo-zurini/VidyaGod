// `VidyaGod coding …` — the coding algebra (MetaPackageFormat chapter 22): shipped bytes (a compressed stream, an
// archive) described as DATA, decoded into their content and a record of the decisions, and coded back bit-exact.
// What a packager runs to make a coded layer, and what anyone runs to check one: describe a format, mint a shipped
// file (refused unless it codes back byte for byte), verify or rebuild it through the read path the mount uses.
// The older hand-wired deflate layer (one stream, its decisions) is here too.
#include "command.h"
#include "cmdutil.h"
#include "bytesource.h"
#include "coding.h"
#include "deflatelayer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/mman.h>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;

std::vector<uint8_t> Slurp(const std::string &P)
{
    std::ifstream F(P, std::ios::binary);
    if (!F) throw Command::BadArgs("cannot read " + P);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(F), {});
}
//Written beside its place and renamed over it once whole: a failed write leaves nothing half-written.
void Spit(const std::string &P, const uint8_t *B, size_t N)
{
    const std::string Tmp = P + ".part";
    {
        std::ofstream F(Tmp, std::ios::binary | std::ios::trunc);
        F.write(reinterpret_cast<const char *>(B), static_cast<std::streamsize>(N));
        F.close();
        if (!F) { std::remove(Tmp.c_str()); throw std::runtime_error("cannot write " + P); }
    }
    if (std::rename(Tmp.c_str(), P.c_str()) != 0) { std::remove(Tmp.c_str()); throw std::runtime_error("cannot write " + P); }
}
std::shared_ptr<ByteSource> Map(const std::string &P)
{
    auto M = std::make_shared<MmapByteSource>();
    if (!M->Map(P)) throw Command::BadArgs("cannot read " + P + " (missing or empty)");
    return M;
}
coding::Description Described(const std::string &P)
{
    const auto B = Slurp(P);
    coding::Description D;
    std::string Err;
    if (!coding::Parse(B.data(), B.size(), D, Err)) throw std::runtime_error("the description " + P + ": " + Err);
    return D;
}
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Read Src front to back in 8 MiB pieces (asking ahead first, as a layer above would), comparing with Want or writing to
// Out; the read path the mount uses. Returns the seconds it took.
double ReadThrough(ByteSource &Src, const uint8_t *Want, std::FILE *Out)
{
    std::vector<uint8_t> B(8u << 20);
    const double T0 = Now();
    for (uint64_t O = 0; O < Src.size(); O += B.size())
    {
        const size_t N = static_cast<size_t>(std::min<uint64_t>(B.size(), Src.size() - O));
        Src.prefetch(O + N, std::min<uint64_t>(B.size(), Src.size() - std::min(Src.size(), O + N)));
        if (!Src.preadAll(B.data(), N, O)) throw std::runtime_error("read failed at " + std::to_string(O));
        if (Want && std::memcmp(B.data(), Want + O, N)) throw std::runtime_error("DIFFERENT in [" + std::to_string(O) + ", " + std::to_string(O + N) + ")");
        if (Out && std::fwrite(B.data(), 1, N, Out) != N) throw std::runtime_error("cannot write");
    }
    return Now() - T0;
}
}

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    using Command::AsPath;
    Command::RegisterGroup("coding", "the coding algebra: describe a format, mint a shipped file into content + record, verify it back");

    Command::Register({
        "coding describe", "write a format's description (data: the program that decodes, codes and prices it)",
        "lz4 (an LZ4 block), deflate (a raw deflate stream), bsa (a Bethesda archive v105: Skyrim SE), or "
        "lzma:lc,lp,pb,dict[,end] (an LZMA stream with those properties; end = 1 when it carries an end marker).",
        { Pos("format", "lz4 | deflate | bsa | lzma:lc,lp,pb,dict[,end]"), AsPath(Pos("out", "the description file to write")) },
        { "coding describe bsa ~/x/bsa105.vgcd", "coding describe lzma:3,0,2,8388608 ~/x/lzma.vgcd" },
        { "coding::LzmaDescription", "coding::DeflateDescription", "coding::Lz4BlockDescription", "coding::BsaDescription",
          "coding::Serialize" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string F = Command::Str(A, "format");
            coding::Description D;
            if (F == "lz4") D = coding::Lz4BlockDescription();
            else if (F == "deflate") D = coding::DeflateDescription();
            else if (F == "bsa") D = coding::BsaDescription();
            else if (F.rfind("lzma:", 0) == 0)
            {
                unsigned Lc = 0, Lp = 0, Pb = 0, End = 1; unsigned long Dict = 0;
                if (std::sscanf(F.c_str() + 5, "%u,%u,%u,%lu,%u", &Lc, &Lp, &Pb, &Dict, &End) < 4 || Lc > 8 || Lp > 4 || Pb > 4)
                    throw Command::BadArgs("lzma:lc,lp,pb,dict[,end] (lc ≤ 8, lp ≤ 4, pb ≤ 4)");
                D = coding::LzmaDescription(static_cast<int>(Lc), static_cast<int>(Lp), static_cast<int>(Pb), static_cast<uint32_t>(Dict), End != 0);
            }
            else throw Command::BadArgs("no description named " + F + " (lz4, deflate, bsa, lzma:…)");
            const auto B = coding::Serialize(D);
            Spit(Command::Str(A, "out"), B.data(), B.size());
            return json{{"name", D.name}, {"isa", D.isa}, {"bytes", B.size()}, {"ops", D.ops.size()}, {"expressions", D.ex.size()}};
        }, {} });

    Command::Register({
        "coding info", "what a description is: its name, instruction set, size, and the checkpoint spacing it suits",
        "A description file is checked as the mount checks one (every op, register and table in bounds; nothing newer than "
        "this build's instruction set). --shipped: the spacing for a stream of that size.",
        { AsPath(Pos("description", "a .vgcd file")), Opt("shipped", "a shipped stream's size in bytes", Type::Int) },
        { "coding info ~/x/bsa105.vgcd --shipped 1073741824" },
        { "coding::Parse", "coding::SuggestedSpacing" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const coding::Description D = Described(Command::Str(A, "description"));
            return json{{"name", D.name}, {"isa", D.isa}, {"this_build_isa", coding::kIsa}, {"ops", D.ops.size()},
                        {"expressions", D.ex.size()}, {"slots", D.nslots}, {"window", D.window},
                        {"spacing", coding::SuggestedSpacing(D, static_cast<uint64_t>(Command::Int(A, "shipped", 0)))}};
        }, {} });

    Command::Register({
        "coding mint", "decode a shipped file into its content and a record, and refuse unless it codes back bit-exact",
        "The content is what the description decodes (an archive's header region and members, a stream's bytes); the record "
        "holds the decisions and checkpoints. Nothing is written unless every shipped byte codes back exactly. --spacing: "
        "shipped bytes between checkpoints (default: what the description suits). --in-memory: hold the content in an "
        "ordinary buffer (small inputs; the default reserves a lazy 4 GiB map, paid for only as the decode fills it).",
        { AsPath(Pos("description", "a .vgcd file")), AsPath(Pos("shipped", "the shipped file")), AsPath(Pos("content", "the content to write")),
          AsPath(Pos("record", "the record to write")), Opt("spacing", "shipped bytes between checkpoints", Type::Int),
          Flag("in-memory", "decode into an ordinary buffer") },
        { "coding mint ~/x/bsa105.vgcd 'Skyrim - Interface.bsa' Interface.content Interface.vgcr" },
        { "coding::MintInto", "coding::Mint" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const coding::Description D = Described(Command::Str(A, "description"));
            MmapByteSource S;
            if (!S.Map(Command::Str(A, "shipped"))) throw Command::BadArgs("cannot read " + Command::Str(A, "shipped"));
            const uint64_t Spacing = A.contains("spacing") ? static_cast<uint64_t>(Command::Int(A, "spacing")) : coding::SuggestedSpacing(D, S.n);
            std::vector<uint8_t> Rec;
            std::string Err;
            size_t Len = 0;
            const double T0 = Now();
            if (Command::Bool(A, "in-memory", false))
            {
                std::vector<uint8_t> Content;
                const size_t Cap = static_cast<size_t>(std::min<uint64_t>(S.n * 64 + (1u << 20), 0xFFFFFFFFu));
                if (!coding::Mint(D, S.p, static_cast<size_t>(S.n), 0, Cap, Spacing, Content, Rec, Err))
                    throw std::runtime_error("refused: " + Err);
                Len = Content.size();
                Spit(Command::Str(A, "content"), Content.data(), Content.size());
            }
            else
            {
                const size_t Cap = 0xFFFFFFFFu;
                void *Buf = ::mmap(nullptr, Cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (Buf == MAP_FAILED) throw std::runtime_error("cannot reserve the content buffer");
                struct Unmap { void *P; size_t N; ~Unmap() { ::munmap(P, N); } } U{ Buf, Cap };
                if (!coding::MintInto(D, S.p, static_cast<size_t>(S.n), 0, static_cast<uint8_t *>(Buf), Cap, Spacing, Len, Rec, Err))
                    throw std::runtime_error("refused: " + Err);
                Spit(Command::Str(A, "content"), static_cast<const uint8_t *>(Buf), Len);
            }
            Spit(Command::Str(A, "record"), Rec.data(), Rec.size());
            return json{{"description", D.name}, {"shipped", S.n}, {"content", Len}, {"record", Rec.size()}, {"spacing", Spacing},
                        {"seconds", Now() - T0}, {"codes_back", "bit-exact"}};
        }, {} });

    const auto Coded = [](const json &A, uint64_t Expect) {
        std::string Err;
        auto S = coding::CodedByteSource::Create(Map(Command::Str(A, "description")), Map(Command::Str(A, "record")),
                                                 Map(Command::Str(A, "content")), Expect, Err);
        if (!S) throw std::runtime_error("refused: " + Err);
        return S;
    };
    Command::Register({
        "coding verify", "rebuild the shipped bytes through the read path (checkpoints, copy runs, the pool) and compare",
        "Every byte, front to back, as a game reading the mounted file would get it. Exit status says identical or not.",
        { AsPath(Pos("description", "a .vgcd file")), AsPath(Pos("record", "the record")), AsPath(Pos("content", "the content")),
          AsPath(Pos("shipped", "the shipped file to compare with")) },
        { "coding verify ~/x/bsa105.vgcd Interface.vgcr Interface.content 'Skyrim - Interface.bsa'" },
        { "coding::CodedByteSource::Create", "coding::CodedByteSource::pread", "coding::CodedByteSource::prefetch",
          "coding::CodedByteSource::size", "coding::CodedByteSource::checkpoints", "coding::CodedByteSource::segmentsDecoded",
          "coding::CodedByteSource::bytesGenerated" }, Needs::Config, false,
        [Coded](Command::Context &, const json &A) {
            MmapByteSource W;
            if (!W.Map(Command::Str(A, "shipped"))) throw Command::BadArgs("cannot read " + Command::Str(A, "shipped"));
            auto S = Coded(A, W.n);
            const double T = ReadThrough(*S, W.p, nullptr);
            return json{{"identical", true}, {"bytes", S->size()}, {"seconds", T}, {"mb_per_s", T > 0 ? S->size() / T / 1e6 : 0.0},
                        {"checkpoints", S->checkpoints()}, {"segments_decoded", S->segmentsDecoded()}, {"bytes_generated", S->bytesGenerated()}};
        }, {} });

    Command::Register({
        "coding cat", "rebuild the shipped bytes into a file",
        "", { AsPath(Pos("description", "a .vgcd file")), AsPath(Pos("record", "the record")), AsPath(Pos("content", "the content")),
              AsPath(Pos("out", "the file to write")) },
        { "coding cat ~/x/bsa105.vgcd Interface.vgcr Interface.content rebuilt.bsa" },
        { "coding::CodedByteSource::Create" }, Needs::Config, false,
        [Coded](Command::Context &, const json &A) {
            auto S = Coded(A, 0);
            const std::string Path = Command::Str(A, "out"), Tmp = Path + ".part";
            std::FILE *Out = std::fopen(Tmp.c_str(), "wb");
            if (!Out) throw std::runtime_error("cannot write " + Path);
            double T = 0;
            try { T = ReadThrough(*S, nullptr, Out); }
            catch (...) { std::fclose(Out); std::remove(Tmp.c_str()); throw; }
            if (std::fclose(Out) != 0 || std::rename(Tmp.c_str(), Path.c_str()) != 0)
            { std::remove(Tmp.c_str()); throw std::runtime_error("cannot write " + Path); }
            return json{{"bytes", S->size()}, {"seconds", T}};
        }, {} });

    Command::Register({
        "coding deflate-split", "split one raw deflate stream into its decoded content and its decisions (the deflate layer)",
        "The hand-wired deflate layer the mount reads: refused unless the stream is exactly one complete raw deflate stream "
        "and the split rebuilds it byte for byte.",
        { AsPath(Pos("stream", "a raw deflate stream")), AsPath(Pos("decisions", "the decisions to write")), AsPath(Pos("content", "the content to write")) },
        { "coding deflate-split member.deflate member.dec member.content" },
        { "deflatelayer::DeflateDecisions" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const auto S = Slurp(Command::Str(A, "stream"));
            std::vector<uint8_t> Dec, Content;
            std::string Err;
            if (!deflatelayer::DeflateDecisions(S.data(), S.size(), Dec, Content, Err)) throw std::runtime_error("refused: " + Err);
            Spit(Command::Str(A, "decisions"), Dec.data(), Dec.size());
            Spit(Command::Str(A, "content"), Content.data(), Content.size());
            return json{{"stream", S.size()}, {"decisions", Dec.size()}, {"content", Content.size()}};
        }, {} });

    Command::Register({
        "coding deflate-verify", "rebuild a raw deflate stream from its decisions and content through the deflate layer, and compare",
        "", { AsPath(Pos("decisions", "the decisions")), AsPath(Pos("content", "the content")), AsPath(Pos("stream", "the stream to compare with")) },
        { "coding deflate-verify member.dec member.content member.deflate" },
        { "deflatelayer::DeflateByteSource::Create", "deflatelayer::DeflateByteSource::pread", "deflatelayer::DeflateByteSource::size",
          "deflatelayer::DeflateByteSource::blocks" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            MmapByteSource W;
            if (!W.Map(Command::Str(A, "stream"))) throw Command::BadArgs("cannot read " + Command::Str(A, "stream"));
            std::string Err;
            auto S = deflatelayer::DeflateByteSource::Create(Map(Command::Str(A, "decisions")), Map(Command::Str(A, "content")), W.n, Err);
            if (!S) throw std::runtime_error("refused: " + Err);
            const double T = ReadThrough(*S, W.p, nullptr);
            return json{{"identical", true}, {"bytes", S->size()}, {"blocks", S->blocks()}, {"seconds", T}};
        }, {} });
}
