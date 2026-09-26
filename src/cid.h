#ifndef CID_H
#define CID_H

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// Cid — a node's identity (MetaPackageFormat generation 6, §1.7): the CID its CANONICAL bytes get as an IPFS file
// with Kubo's parameters. Canonical = UTF-8 JSON, object keys sorted, arrays in order, no insignificant whitespace,
// the node's own CID and POS left out. A node of ≤ 256 KiB is one raw block: CIDv1, raw codec (0x55), sha2-256,
// base32 multibase ("bafkrei…"). Pure: no IPFS, no Qt — the same bytes always give the same CID, on every machine.
// ---------------------------------------------------------------------------

namespace Cid
{

//The canonical bytes of a node (its CID and POS dropped).
std::string Canonical(const nlohmann::ordered_json &Node);

//The Kubo-parity CID of Bytes as one raw block. "" (with *Error) past 256 KiB — a node that large is a chunked
//UnixFS file, which this pure function does not build.
std::string OfBytes(const std::string &Bytes, std::string *Error = nullptr);

//Canonical + OfBytes.
std::string OfNode(const nlohmann::ordered_json &Node, std::string *Error = nullptr);

//SHA-256 of Bytes (raw 32-byte digest). Exposed for the known-answer tests.
std::string Sha256(const std::string &Bytes);

} // namespace Cid

#endif // CID_H
