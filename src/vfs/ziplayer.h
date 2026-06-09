#ifndef VIDYAGODFS_ZIPLAYER_H
#define VIDYAGODFS_ZIPLAYER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <memory>
#include <mutex>
#include <cstdint>
#include <ctime>
#include <zip.h>

//One file entry inside a zip layer, captured at index time.
struct ZipEntry {
    std::string name;            // normalized, no leading/trailing '/'
    uint64_t    index = 0;       // zip entry index, for zip_fopen_index
    uint64_t    size = 0;        // uncompressed size
    time_t      mtime = 0;
    bool        isDir = false;
    // STORED (uncompressed, unencrypted) entries are served zero-copy: their bytes sit contiguously
    // in the archive at `dataOffset`, so reads are a direct pread (no decompression, no materialization).
    bool        stored = false;
    uint64_t    dataOffset = 0;  // absolute byte offset of the entry data within the archive
};

//A whole zip archive indexed for the FS lifetime.
//  STORED entries → zero-copy pread on `rawFd` at ZipEntry::dataOffset (any size, bounded by nothing).
//  DEFLATE entries → materialized on open (memory ≤ threshold, else a temp file), since DEFLATE is one
//    non-seekable stream per entry. Wine keeps mmapped EXE/DLLs open for the process lifetime, so this
//    is one extraction per open, not per page fault.
struct ZipIndex {
    std::string archivePath;
    zip_t*      archive = nullptr;  // libzip handle, for DEFLATE inflation + the entry list
    int         rawFd = -1;         // plain O_RDONLY fd for STORED pread (thread-safe, no shared offset)
    std::unordered_map<std::string, ZipEntry>              byName;      // files + explicit dir entries
    std::unordered_map<std::string, std::set<std::string>> dirChildren; // parent vrel → child segment names
    std::mutex  mtx;             // guards zip_fopen_index extraction on `archive` (DEFLATE only)
    ~ZipIndex();
};

//Opens ArchivePath and indexes every entry (synthesizing missing parent dirs). nullptr on failure.
std::shared_ptr<ZipIndex> BuildZipIndex(const std::string &ArchivePath);

//Per-open reader. STORED: serves reads via pread on the shared archive fd (`storedFd`, not owned).
//DEFLATE: small entries live in `mem`; large ones in a temp file `tmpFd` (owned, freed on destroy).
struct ZipReader {
    bool              stored = false;
    int               storedFd = -1;    // shared ZipIndex::rawFd — NOT closed by the destructor
    uint64_t          dataOffset = 0;
    std::vector<char> mem;
    int               tmpFd = -1;
    std::string       tmpPath;
    uint64_t          size = 0;
    ~ZipReader();
};

//Extracts entry E from Z into Out (memory if <= MemThreshold, else a temp file under TempDir).
//Returns 0 or -errno.
int OpenZipEntry(ZipIndex &Z, const ZipEntry &E, const std::string &TempDir, ZipReader &Out);

//Serves a read from an opened ZipReader. Returns bytes read (>=0) or -errno.
int ReadZipEntry(ZipReader &R, char *Buf, size_t Size, off_t Off);

#endif // VIDYAGODFS_ZIPLAYER_H
