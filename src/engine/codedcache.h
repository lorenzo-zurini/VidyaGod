#pragma once
// The per-machine twin of a PREDICTED coded record (MetaPackageFormat 04 §CODED, 22.15.4).
//
// A predicted record ("VGCQ") holds only what an encoder did otherwise than its family predicts: small, the form a
// package keeps and sends. Reading through it runs the family (an LZ4HC parse per block: tens of MB/s), so a machine keeps
// its PLAIN twin ("VGCR", every decision, today's read speed) in its cache, made once per record, and mounts that
// instead. The twin is derived data: deleting the cache costs a re-expansion, never a byte of the game.

#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

struct ByteSource;

namespace CodedCache
{
//The twins live in Dir (the app's: <data root>/CACHE/coded) as <record CID>.<content CID>.<description CID>.vgcr: a twin
//is the decisions of ONE stream, and a record over other content (or under another description) codes another stream.
//The twin for one record: made here if it is not there yet (refused unless it codes back bit-exact from the content).
//Content: the bytes of the layer the record codes from.
bool PlainTwin(const std::filesystem::path &Dir, const std::filesystem::path &Description, const std::filesystem::path &Record,
               const std::string &RecordCid, const std::string &ContentCid, const std::string &DescriptionCid,
               const std::shared_ptr<ByteSource> &Content, std::filesystem::path &Out, std::string &Err);
//Every coded layer of a mount spec whose record is predicted: its source becomes the twin (made first where missing).
//A layer whose twin cannot be made keeps its predicted record (it serves the same bytes, slower) and the reason is logged.
//Returns how many layers now read a twin.
size_t UsePlainRecords(nlohmann::ordered_json &Spec, const std::filesystem::path &Dir);
}
