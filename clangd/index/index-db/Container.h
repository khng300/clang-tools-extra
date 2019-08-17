#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_INDEXDB_CONTAINER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_INDEXDB_CONTAINER_H

#include "index/Serialization.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

namespace clang {
namespace clangd {
namespace dbindex {
namespace container {

struct ChunkMetadata {
  char Name[4];
  uint32_t Size;
  uint64_t Offset;
};

// CONTAINER ENCODING
// - Signature - "MMAP": 4 bytes
// - Number of chunks: 4 bytes
// - ChunkMetadata[]: 16 * (number of chunks) bytes
// - Padding: N bytes aligning to page boundaries
// - Chunks[] (Page size aligned)
class Container {
public:
  using MapType = llvm::StringMap<llvm::StringRef>;
  using const_iterator = MapType::const_iterator;
  using iterator = const_iterator;
  using value_type = MapType::mapped_type;

  Container(const void *V, size_t N);
  Container(llvm::StringRef Data);

  const_iterator begin() const { return ChunksMap.begin(); }
  const_iterator end() const { return ChunksMap.end(); }
  const_iterator find(const char (&Name)[5]) const {
    return ChunksMap.find(Name);
  }

  size_t size() const { return Size; }

private:
  const void *StartAddress;
  size_t Size;
  MapType ChunksMap;
};

using FourCC = std::array<char, 4>;
// Get a FourCC from a string literal, e.g. fourCC("RIFF").
inline constexpr FourCC fourCC(const char (&Literal)[5]) {
  return FourCC{{Literal[0], Literal[1], Literal[2], Literal[3]}};
}
inline constexpr llvm::StringRef toStringRef(const FourCC &Literal) {
  return llvm::StringRef(Literal.data(), Literal.size());
}

struct File {
  struct ChunkInfo {
    FourCC Name;
    llvm::StringRef Data;
  };

  size_t HeaderOffsetInPage = 16; // Default to 16 bytes for LMDB
  std::vector<ChunkInfo> Chunks;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const File &F);

llvm::Expected<IndexFileIn> readContainer(llvm::StringRef Data);

void writeContainer(const IndexFileOut &Data, raw_ostream &OS);

llvm::Optional<Symbol> getSymbolInContainer(llvm::StringRef Data, SymbolID ID);
llvm::Optional<std::vector<Ref>> getRefsInContainer(llvm::StringRef Data,
                                                    SymbolID ID);
void getRelationsInContainer(
    llvm::StringRef Data, SymbolID Subject, index::SymbolRole Predicate,
    llvm::function_ref<bool(const Relation &)> Callback);

Symbol readSymbol(llvm::StringRef Data);
std::string writeSymbol(const Symbol &Sym);

} // namespace container
} // namespace dbindex
} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_INDEXDB_CONTAINER_H
