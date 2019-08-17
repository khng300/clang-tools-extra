#include "Container.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Process.h"

namespace clang {
namespace clangd {
namespace dbindex {
namespace container {

static constexpr char CONTAINER_MAGIC[4] = {'M', 'M', 'A', 'P'};

class Reader {
public:
  Reader(const void *Data, size_t N)
      : Begin(static_cast<const char *>(Data)),
        End(static_cast<const char *>(Data) + N) {}

  Reader(llvm::StringRef Data)
      : Begin(Data.data()), End(Data.data() + Data.size()) {}

  uint8_t consume8() {
    if (Begin == End) {
      Err = true;
      return 0;
    }
    return *Begin++;
  }

  uint16_t consume16() {
    if (Begin + 2 > End) {
      Err = true;
      return 0;
    }
    auto Ret = llvm::support::endian::read16le(Begin);
    Begin += 2;
    return Ret;
  }

  uint32_t consume32() {
    if (Begin + 4 > End) {
      Err = true;
      return 0;
    }
    auto Ret = llvm::support::endian::read32le(Begin);
    Begin += 4;
    return Ret;
  }

  uint64_t consume64() {
    if (Begin + 8 > End) {
      Err = true;
      return 0;
    }
    auto Ret = llvm::support::endian::read64le(Begin);
    Begin += 8;
    return Ret;
  }

  template <typename T> T consumeEnum() {
    using UT = typename std::underlying_type<T>::type;
    if (Begin + sizeof(UT) > End) {
      Err = true;
      return static_cast<T>(-1);
    }
    auto Ret =
        llvm::support::endian::read<T, llvm::support::endianness::little>(
            Begin);
    Begin += sizeof(UT);
    return static_cast<T>(Ret);
  }

  llvm::StringRef consumeFixedString(size_t N) {
    if (Begin + N > End) {
      Err = true;
      return llvm::StringRef();
    }
    auto Ret = llvm::StringRef(Begin, N);
    Begin += N;
    return Ret;
  }

  llvm::StringRef consumeCString() {
    const char *Pointer = Begin;
    if (Begin == End) {
      Err = true;
      return llvm::StringRef();
    }
    while (Begin != End && *Begin++)
      ;
    return llvm::StringRef(Pointer, Begin - Pointer).drop_back();
  }

  bool eof() const { return Begin == End || Err; }
  bool err() const { return Err; }
  size_t remaining() const { return End - Begin; }
  llvm::StringRef rest() const { return llvm::StringRef(Begin, End - Begin); }

private:
  const char *Begin, *End;
  bool Err = false;
};

class Writer {
public:
  Writer(llvm::raw_ostream &OS) : OS(OS) {}

  void write8(uint8_t I) { OS.write(I); }
  void write16(uint16_t I) {
    char Buf[2];
    llvm::support::endian::write16le(Buf, I);
    OS.write(Buf, sizeof(Buf));
  }
  void write32(uint32_t I) {
    char Buf[4];
    llvm::support::endian::write32le(Buf, I);
    OS.write(Buf, sizeof(Buf));
  }
  void write64(uint64_t I) {
    char Buf[8];
    llvm::support::endian::write64le(Buf, I);
    OS.write(Buf, sizeof(Buf));
  }
  template <typename T> void writeEnum(T I) {
    using UT = typename std::underlying_type<T>::type;
    char Buf[sizeof(UT)];
    llvm::support::endian::write<T, llvm::support::endianness::little>(Buf, I);
    OS.write(Buf, sizeof(Buf));
  }
  void writeFixedString(llvm::StringRef Data) {
    OS.write(Data.data(), Data.size());
  }
  void writeFixedString(const char *Data, size_t Size) { OS.write(Data, Size); }
  void writeCString(llvm::StringRef S) {
    OS.write(S.data(), S.size());
    OS.write(0);
  }
  void writeZeroes(size_t Size) { OS.write_zeros(Size); }

private:
  llvm::raw_ostream &OS;
};

struct UniqueStringTable {
  struct StringEntry {
    uint32_t Offset;
    uint32_t Size;
  };
  std::string StringStream;
  std::vector<StringEntry> StringIDs;
  llvm::StringMap<uint32_t> IDMap;

  uint32_t intern(llvm::StringRef S) {
    uint32_t StringID = StringIDs.size();
    auto R = IDMap.try_emplace(S, StringID);
    if (!R.second)
      return R.first->second;
    auto Offset = StringStream.size();
    StringIDs.emplace_back(StringEntry{static_cast<uint32_t>(Offset),
                                       static_cast<uint32_t>(S.size())});
    StringStream.append(S);
    StringStream.push_back(0);
    return StringID;
  }

  void writeStringIDsTable(Writer &W) {
    for (auto &I : StringIDs) {
      W.write32(I.Offset);
      W.write32(I.Size);
    }
  }

  void writeStringStream(Writer &W) { W.writeFixedString(StringStream); }
};

UniqueStringTable::StringEntry getStringEntry(llvm::StringRef StringIDsTable,
                                              uint32_t StringID) {
  auto TableAddress =
      StringIDsTable.data() + StringID * sizeof(UniqueStringTable::StringEntry);
  auto Offset = llvm::support::endian::read32le(TableAddress);
  auto Size = llvm::support::endian::read32le(
      TableAddress + sizeof(UniqueStringTable::StringEntry::Size));
  return {Offset, Size};
}

llvm::StringRef getString(llvm::StringRef StringIDsTable,
                          llvm::StringRef StringStream, uint32_t StringID) {
  UniqueStringTable::StringEntry SE = getStringEntry(StringIDsTable, StringID);
  return llvm::StringRef(StringStream.data() + SE.Offset, SE.Size);
}

class Reader2 : public Reader {
public:
  Reader2(llvm::StringRef StringIDsTable, llvm::StringRef StringStream,
          llvm::StringRef Data)
      : Reader(Data), StringIDsTable(StringIDsTable),
        StringStream(StringStream) {}

  llvm::StringRef consumeInternedString() {
    auto StringID = consume32();
    if (err())
      return llvm::StringRef();
    return getString(StringIDsTable, StringStream, StringID);
  }

private:
  llvm::StringRef StringIDsTable;
  llvm::StringRef StringStream;
};

class Writer2 : public Writer {
public:
  Writer2(UniqueStringTable &Strings, llvm::raw_ostream &OS)
      : Writer(OS), Strings(Strings) {}

  void writeInternString(llvm::StringRef S) { write32(Strings.intern(S)); }

private:
  UniqueStringTable &Strings;
};

Container::Container(const void *V, size_t N) : StartAddress(V), Size(N) {
  Reader R(V, N);
  if (R.consumeFixedString(4).compare({CONTAINER_MAGIC, 4}))
    return;
  size_t NumChunks = R.consume32();
  for (size_t I = 0; I < NumChunks; I++) {
    llvm::StringRef ChunkName = R.consumeFixedString(4);
    size_t ChunkSize = R.consume32();
    size_t ChunkOffset = R.consume64();
    if (R.err())
      break;
    llvm::StringRef Data{static_cast<const char *>(StartAddress) + ChunkOffset,
                         ChunkSize};
    ChunksMap.try_emplace(ChunkName, Data);
  }
}

Container::Container(llvm::StringRef Data)
    : Container(Data.data(), Data.size()) {}

inline uint64_t getPageAlignedSize(size_t Size) {
  static unsigned int PageSize = llvm::sys::Process::getPageSizeEstimate();
  return ((Size + PageSize - 1) / PageSize) * PageSize;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const File &F) {
  size_t HeaderSize = 4 + 4 + sizeof(ChunkMetadata) * F.Chunks.size();
  size_t PgAlignedHeaderSize =
      getPageAlignedSize(HeaderSize + F.HeaderOffsetInPage);
  uint64_t Offset = PgAlignedHeaderSize - F.HeaderOffsetInPage;
  llvm::StringMap<uint64_t> ChunkOffset;
  for (auto &I : F.Chunks) {
    uint64_t PgAlignedSize = getPageAlignedSize(I.Data.size());
    ChunkOffset.try_emplace(toStringRef(I.Name), Offset);
    Offset += PgAlignedSize;
  }

  Writer W(OS);
  W.writeFixedString(CONTAINER_MAGIC, 4);
  W.write32(F.Chunks.size());
  for (auto &I : F.Chunks) {
    W.writeFixedString(toStringRef(I.Name));
    W.write32(I.Data.size());
    W.write64(ChunkOffset[toStringRef(I.Name)]);
  }
  W.writeZeroes(PgAlignedHeaderSize - HeaderSize - F.HeaderOffsetInPage);
  for (auto &I : F.Chunks) {
    W.writeFixedString(I.Data);
    W.writeZeroes(getPageAlignedSize(I.Data.size()) - I.Data.size());
  }

  return OS;
}

void writeLocation(const SymbolLocation &Loc, Writer &W) {
  W.writeCString(Loc.FileURI);
  for (const auto &Endpoint : {Loc.Start, Loc.End}) {
    W.write32(Endpoint.line());
    W.write32(Endpoint.column());
  }
}

void writeLocation(const SymbolLocation &Loc, Writer2 &W) {
  W.writeInternString(Loc.FileURI);
  for (const auto &Endpoint : {Loc.Start, Loc.End}) {
    W.write32(Endpoint.line());
    W.write32(Endpoint.column());
  }
}

SymbolLocation readLocation(Reader &R) {
  SymbolLocation Loc;
  Loc.FileURI = R.consumeCString().data();
  for (auto *Endpoint : {&Loc.Start, &Loc.End}) {
    Endpoint->setLine(R.consume32());
    Endpoint->setColumn(R.consume32());
  }
  return Loc;
}

SymbolLocation readLocation(Reader2 &R) {
  SymbolLocation Loc;
  Loc.FileURI = R.consumeInternedString().data();
  for (auto *Endpoint : {&Loc.Start, &Loc.End}) {
    Endpoint->setLine(R.consume32());
    Endpoint->setColumn(R.consume32());
  }
  return Loc;
}

IncludeGraphNode readIncludeGraphNode(Reader2 &R) {
  IncludeGraphNode IGN;
  IGN.Flags = R.consumeEnum<IncludeGraphNode::SourceFlag>();
  IGN.URI = R.consumeInternedString();
  llvm::StringRef Digest = R.consumeFixedString(IGN.Digest.size());
  std::copy(Digest.bytes_begin(), Digest.bytes_end(), IGN.Digest.begin());
  IGN.DirectIncludes.resize(R.consume32());
  for (llvm::StringRef &Include : IGN.DirectIncludes)
    Include = R.consumeInternedString();
  return IGN;
}

void writeIncludeGraphNode(const IncludeGraphNode &IGN, Writer2 &W) {
  W.writeEnum(IGN.Flags);
  W.writeInternString(IGN.URI);

  llvm::StringRef Hash(reinterpret_cast<const char *>(IGN.Digest.data()),
                       IGN.Digest.size());
  W.writeFixedString(Hash);
  W.write32(IGN.DirectIncludes.size());

  for (llvm::StringRef Include : IGN.DirectIncludes)
    W.writeInternString(Include);
}

std::string writeSymbol(const Symbol &Sym) {
  std::string Data;
  llvm::raw_string_ostream OS(Data);
  Writer W(OS);
  W.writeFixedString(Sym.ID.raw());
  W.writeEnum(Sym.SymInfo.Kind);
  W.writeEnum(Sym.SymInfo.Lang);
  W.writeCString(Sym.Name);
  W.writeCString(Sym.Scope);
  W.writeCString(Sym.TemplateSpecializationArgs);
  writeLocation(Sym.Definition, W);
  writeLocation(Sym.CanonicalDeclaration, W);
  W.write32(Sym.References);
  W.writeEnum(Sym.Flags);
  W.writeEnum(Sym.Origin);
  W.writeCString(Sym.Signature);
  W.writeCString(Sym.CompletionSnippetSuffix);
  W.writeCString(Sym.Documentation);
  W.writeCString(Sym.ReturnType);
  W.writeCString(Sym.Type);

  auto WriteInclude = [&](const Symbol::IncludeHeaderWithReferences &Include) {
    W.writeCString(Include.IncludeHeader);
    W.write32(Include.References);
  };
  W.write32(Sym.IncludeHeaders.size());
  for (const auto &Include : Sym.IncludeHeaders)
    WriteInclude(Include);

  OS.flush();
  return Data;
}

void writeSymbol(const Symbol &Sym, Writer2 &W) {
  W.writeFixedString(Sym.ID.raw());
  W.writeEnum(Sym.SymInfo.Kind);
  W.writeEnum(Sym.SymInfo.Lang);
  W.writeInternString(Sym.Name);
  W.writeInternString(Sym.Scope);
  W.writeInternString(Sym.TemplateSpecializationArgs);
  writeLocation(Sym.Definition, W);
  writeLocation(Sym.CanonicalDeclaration, W);
  W.write32(Sym.References);
  W.writeEnum(Sym.Flags);
  W.writeEnum(Sym.Origin);
  W.writeInternString(Sym.Signature);
  W.writeInternString(Sym.CompletionSnippetSuffix);
  W.writeInternString(Sym.Documentation);
  W.writeInternString(Sym.ReturnType);
  W.writeInternString(Sym.Type);

  auto WriteInclude = [&](const Symbol::IncludeHeaderWithReferences &Include) {
    W.writeInternString(Include.IncludeHeader);
    W.write32(Include.References);
  };
  W.write32(Sym.IncludeHeaders.size());
  for (const auto &Include : Sym.IncludeHeaders)
    WriteInclude(Include);
}

Symbol readSymbol(llvm::StringRef Data) {
  Reader R(Data);
  Symbol Sym;
  Sym.ID = SymbolID::fromRaw(R.consumeFixedString(SymbolID::RawSize));
  Sym.SymInfo.Kind = R.consumeEnum<index::SymbolKind>();
  Sym.SymInfo.Lang = R.consumeEnum<index::SymbolLanguage>();
  Sym.Name = R.consumeCString();
  Sym.Scope = R.consumeCString();
  Sym.TemplateSpecializationArgs = R.consumeCString();
  Sym.Definition = readLocation(R);
  Sym.CanonicalDeclaration = readLocation(R);
  Sym.References = R.consume32();
  Sym.Flags = R.consumeEnum<Symbol::SymbolFlag>();
  Sym.Origin = R.consumeEnum<SymbolOrigin>();
  Sym.Signature = R.consumeCString();
  Sym.CompletionSnippetSuffix = R.consumeCString();
  Sym.Documentation = R.consumeCString();
  Sym.ReturnType = R.consumeCString();
  Sym.Type = R.consumeCString();
  Sym.IncludeHeaders.resize(R.consume32());
  for (auto &I : Sym.IncludeHeaders) {
    I.IncludeHeader = R.consumeCString();
    I.References = R.consume32();
  }
  return Sym;
}

Symbol readSymbol(Reader2 &R) {
  Symbol Sym;
  Sym.ID = SymbolID::fromRaw(R.consumeFixedString(SymbolID::RawSize));
  Sym.SymInfo.Kind = R.consumeEnum<index::SymbolKind>();
  Sym.SymInfo.Lang = R.consumeEnum<index::SymbolLanguage>();
  Sym.Name = R.consumeInternedString();
  Sym.Scope = R.consumeInternedString();
  Sym.TemplateSpecializationArgs = R.consumeInternedString();
  Sym.Definition = readLocation(R);
  Sym.CanonicalDeclaration = readLocation(R);
  Sym.References = R.consume32();
  Sym.Flags = R.consumeEnum<Symbol::SymbolFlag>();
  Sym.Origin = R.consumeEnum<SymbolOrigin>();
  Sym.Signature = R.consumeInternedString();
  Sym.CompletionSnippetSuffix = R.consumeInternedString();
  Sym.Documentation = R.consumeInternedString();
  Sym.ReturnType = R.consumeInternedString();
  Sym.Type = R.consumeInternedString();
  Sym.IncludeHeaders.resize(R.consume32());
  for (auto &I : Sym.IncludeHeaders) {
    I.IncludeHeader = R.consumeInternedString();
    I.References = R.consume32();
  }
  return Sym;
}

// REFS ENCODING
// A refs section has data grouped by Symbol. Each symbol has:
//  - SymbolID: 8 bytes
//  - NumRefs: uint32
//  - Ref[NumRefs]
// Fields of Ref are encoded in turn, see implementation.

void writeRefs(const SymbolID &ID, llvm::ArrayRef<Ref> Refs, Writer2 &W) {
  W.writeFixedString(ID.raw());
  W.write32(Refs.size());
  for (const auto &Ref : Refs) {
    W.writeEnum<RefKind>(Ref.Kind);
    writeLocation(Ref.Location, W);
  }
}

std::pair<SymbolID, std::vector<Ref>> readRefs(Reader2 &R) {
  std::pair<SymbolID, std::vector<Ref>> Result;
  Result.first = SymbolID::fromRaw(R.consumeFixedString(SymbolID::RawSize));
  Result.second.resize(R.consume32());
  for (auto &Ref : Result.second) {
    Ref.Kind = R.consumeEnum<RefKind>();
    Ref.Location = readLocation(R);
  }
  return Result;
}

// RELATIONS ENCODING
// A relations section is a flat list of relations. Each relation has:
//  - SymbolID (subject): 8 bytes
//  - relation kind (predicate): 1 byte
//  - SymbolID (object): 8 bytes
// In the future, we might prefer a packed representation if the need arises.

void writeRelation(const Relation &R, Writer &W) {
  W.writeFixedString(R.Subject.raw());
  RelationKind Kind = symbolRoleToRelationKind(R.Predicate);
  W.writeEnum(Kind);
  W.writeFixedString(R.Object.raw());
}

Relation readRelation(Reader &R) {
  SymbolID Subject = SymbolID::fromRaw(R.consumeFixedString(SymbolID::RawSize));
  index::SymbolRole Predicate =
      relationKindToSymbolRole(R.consumeEnum<RelationKind>());
  SymbolID Object = SymbolID::fromRaw(R.consumeFixedString(SymbolID::RawSize));
  return {Subject, Predicate, Object};
}

struct CompileCommand {
  llvm::StringRef Directory;
  std::vector<llvm::StringRef> CommandLine;
};

void writeCompileCommand(const CompileCommand &Cmd, Writer2 &W) {
  W.writeInternString(Cmd.Directory);
  W.write32(Cmd.CommandLine.size());
  for (llvm::StringRef C : Cmd.CommandLine)
    W.writeInternString(C);
}

CompileCommand readCompileCommand(Reader2 R) {
  CompileCommand Cmd;
  Cmd.Directory = R.consumeInternedString();
  Cmd.CommandLine.resize(R.consume32());
  for (llvm::StringRef &C : Cmd.CommandLine)
    C = R.consumeInternedString();
  return Cmd;
}

constexpr static uint32_t Version = 12;

llvm::Expected<IndexFileIn> readContainer(llvm::StringRef Data) {
  IndexFileIn Ret;
  Container Content(Data);
  llvm::StringRef StringIDsTable, StringStream;
  {
    auto StringStreamEntry = Content.find("Strs");
    auto StringIDsTableEntry = Content.find("StrI");
    StringIDsTable = StringIDsTableEntry->getValue();
    StringStream = StringStreamEntry->getValue();
  }
  {
    auto SymbolsEntry = Content.find("Syms");
    auto SymbolsChunk = SymbolsEntry->getValue();
    Reader2 R(StringIDsTable, StringStream, SymbolsChunk);
    SymbolSlab::Builder Builder;
    while (!R.eof())
      Builder.insert(readSymbol(R));
    Ret.Symbols = std::move(Builder).build();
  }
  {
    auto SourcesEntry = Content.find("IGNs");
    auto SourcesChunk = SourcesEntry->getValue();
    Reader2 R(StringIDsTable, StringStream, SourcesChunk);
    Ret.Sources.emplace();
    while (!R.eof()) {
      auto IGN = readIncludeGraphNode(R);
      auto Entry = Ret.Sources->try_emplace(IGN.URI).first;
      Entry->getValue() = std::move(IGN);
      // We change all the strings inside the structure to point at the keys in
      // the map, since it is the only copy of the string that's going to live.
      Entry->getValue().URI = Entry->getKey();
      for (auto &Include : Entry->getValue().DirectIncludes)
        Include = Ret.Sources->try_emplace(Include).first->getKey();
    }
  }
  {
    auto RefsEntry = Content.find("Refs");
    auto RefsChunk = RefsEntry->getValue();
    Reader2 R(StringIDsTable, StringStream, RefsChunk);
    RefSlab::Builder Builder;
    while (!R.eof()) {
      auto Refs = readRefs(R);
      for (auto &I : Refs.second)
        Builder.insert(Refs.first, I);
    }
    Ret.Refs = std::move(Builder).build();
  }
  {
    auto RelationsEntry = Content.find("Rela");
    auto RelationsChunk = RelationsEntry->getValue();
    Reader2 R(StringIDsTable, StringStream, RelationsChunk);
    RelationSlab::Builder Builder;
    while (!R.eof())
      Builder.insert(readRelation(R));
    Ret.Relations = std::move(Builder).build();
  }
  {
    auto CmdEntry = Content.find("Cmds");
    auto CmdChunk = CmdEntry->getValue();
    Reader2 R(StringIDsTable, StringStream, CmdChunk);
    CompileCommand Cmd = readCompileCommand(R);
    Ret.Cmd.emplace();
    Ret.Cmd->Directory = Cmd.Directory;
    Ret.Cmd->CommandLine.reserve(Cmd.CommandLine.size());
    for (llvm::StringRef C : Cmd.CommandLine)
      Ret.Cmd->CommandLine.emplace_back(C);
  }
  return Ret;
}

void writeContainer(const IndexFileOut &Data, raw_ostream &OS) {
  assert(Data.Symbols && "An index file without symbols makes no sense!");
  File DataFile;
  UniqueStringTable Strings;

  llvm::SmallString<4> Meta;
  {
    llvm::raw_svector_ostream MetaOS(Meta);
    Writer W(MetaOS);
    W.write32(Version);
  }
  DataFile.Chunks.push_back({fourCC("meta"), Meta});

  std::string SymbolsChunk;
  std::vector<std::pair<SymbolID, uint32_t>> SymbolsOffsetTable;
  {
    llvm::raw_string_ostream SymbolsChunkOS(SymbolsChunk);
    Writer2 W(Strings, SymbolsChunkOS);
    for (const auto &Sym : *Data.Symbols) {
      auto Offset = SymbolsChunk.size();
      writeSymbol(Sym, W);
      SymbolsChunkOS.flush();
      SymbolsOffsetTable.emplace_back(Sym.ID, Offset);
    }
  }
  std::string SymbolsOffsetTableChunk;
  {
    llvm::raw_string_ostream SymbolsOffsetTableChunkOS(SymbolsOffsetTableChunk);
    Writer W(SymbolsOffsetTableChunkOS);
    for (const auto &I : SymbolsOffsetTable) {
      W.writeFixedString(I.first.raw());
      W.write32(I.second);
      W.write32(0);
    }
  }

  std::string SourcesChunk;
  if (Data.Sources) {
    llvm::raw_string_ostream SourcesChunkOS(SourcesChunk);
    Writer2 W(Strings, SourcesChunkOS);
    for (const auto &Source : *Data.Sources)
      writeIncludeGraphNode(Source.second, W);
  }

  std::string RefsChunk;
  std::vector<std::pair<SymbolID, uint32_t>> RefsOffsetTable;
  if (Data.Refs) {
    std::vector<RefSlab::value_type> SortedRefs;
    for (const auto &I : *Data.Refs)
      SortedRefs.emplace_back(I);
    llvm::sort(SortedRefs,
               [](const RefSlab::value_type &L, const RefSlab::value_type &R) {
                 return L.first < R.first;
               });
    llvm::raw_string_ostream RefsChunkOS(RefsChunk);
    Writer2 W(Strings, RefsChunkOS);
    for (const auto &I : SortedRefs) {
      auto Offset = RefsChunk.size();
      writeRefs(I.first, I.second, W);
      RefsChunkOS.flush();
      RefsOffsetTable.emplace_back(I.first, Offset);
    }
  }
  std::string RefsOffsetTableChunk;
  {
    llvm::raw_string_ostream RefsOffsetTableChunkOS(RefsOffsetTableChunk);
    Writer W(RefsOffsetTableChunkOS);
    for (const auto &I : RefsOffsetTable) {
      W.writeFixedString(I.first.raw());
      W.write32(I.second);
      W.write32(0);
    }
  }

  std::string RelationsChunk;
  if (Data.Relations) {
    llvm::raw_string_ostream RelationsChunkOS(RelationsChunk);
    Writer W(RelationsChunkOS);
    for (const auto &Relation : *Data.Relations) {
      writeRelation(Relation, W);
    }
  }

  std::string CmdChunk;
  if (Data.Cmd) {
    CompileCommand Cmd;
    Cmd.CommandLine.reserve(Data.Cmd->CommandLine.size());
    Cmd.Directory = Data.Cmd->Directory;
    for (llvm::StringRef C : Data.Cmd->CommandLine) {
      Cmd.CommandLine.emplace_back(C);
    }
    llvm::raw_string_ostream CmdChunkOS(CmdChunk);
    Writer2 W(Strings, CmdChunkOS);
    writeCompileCommand(Cmd, W);
  }

  std::string StringsChunk;
  {
    llvm::raw_string_ostream StringsChunkOS(StringsChunk);
    Writer W(StringsChunkOS);
    Strings.writeStringStream(W);
  }
  std::string StringIDsTableChunk;
  {
    llvm::raw_string_ostream StringIDsTableChunkOS(StringIDsTableChunk);
    Writer W(StringIDsTableChunkOS);
    Strings.writeStringIDsTable(W);
  }

  DataFile.Chunks.push_back({fourCC("SymO"), SymbolsOffsetTableChunk});
  DataFile.Chunks.push_back({fourCC("Syms"), SymbolsChunk});

  DataFile.Chunks.push_back({fourCC("IGNs"), SourcesChunk});

  DataFile.Chunks.push_back({fourCC("RefO"), RefsOffsetTableChunk});
  DataFile.Chunks.push_back({fourCC("Refs"), RefsChunk});

  DataFile.Chunks.push_back({fourCC("Rela"), RelationsChunk});

  DataFile.Chunks.push_back({fourCC("Cmds"), CmdChunk});

  DataFile.Chunks.push_back({fourCC("StrI"), StringIDsTableChunk});
  DataFile.Chunks.push_back({fourCC("Strs"), StringsChunk});

  OS << DataFile;
}

llvm::Optional<Symbol> getSymbolInContainer(llvm::StringRef Data, SymbolID ID) {
  Container Content(Data);
  llvm::StringRef StringIDsTable, StringStream;
  {
    auto StringStreamEntry = Content.find("Strs");
    auto StringIDsTableEntry = Content.find("StrI");
    StringIDsTable = StringIDsTableEntry->getValue();
    StringStream = StringStreamEntry->getValue();
  }

  uint32_t Offset;
  {
    struct SymbolIDEntry {
      char SymbolID[8];
      char Offset[4];
      char Padding[4];
    };
    auto SymbolOffsetTableEntry = Content.find("SymO");
    auto SymbolOffsetTableChunk = SymbolOffsetTableEntry->getValue();
    llvm::ArrayRef<SymbolIDEntry> SOETable(
        reinterpret_cast<const SymbolIDEntry *>(SymbolOffsetTableChunk.data()),
        SymbolOffsetTableChunk.size() / sizeof(SymbolIDEntry));
    auto It = std::partition_point(
        SOETable.begin(), SOETable.end(), [&ID](const SymbolIDEntry &I) {
          return SymbolID::fromRaw({I.SymbolID, SymbolID::RawSize}) < ID;
        });
    if (It == SOETable.end())
      return llvm::None;
    Offset = llvm::support::endian::read32le(It->Offset);
  }

  auto SymbolsEntry = Content.find("Syms");
  auto SymbolsChunk = SymbolsEntry->getValue();
  Reader2 R(StringIDsTable, StringStream,
            {SymbolsChunk.data() + Offset, SymbolsChunk.size() - Offset});
  return readSymbol(R);
}

llvm::Optional<std::vector<Ref>> getRefsInContainer(llvm::StringRef Data,
                                                    SymbolID ID) {
  Container Content(Data);
  llvm::StringRef StringIDsTable, StringStream;
  {
    auto StringStreamEntry = Content.find("Strs");
    auto StringIDsTableEntry = Content.find("StrI");
    StringIDsTable = StringIDsTableEntry->getValue();
    StringStream = StringStreamEntry->getValue();
  }

  uint32_t Offset;
  {
    struct SymbolIDEntry {
      char SymbolID[8];
      char Offset[4];
      char Padding[4];
    };
    auto RefsOffsetTableEntry = Content.find("RefO");
    auto RefsOffsetTableChunk = RefsOffsetTableEntry->getValue();
    llvm::ArrayRef<SymbolIDEntry> SOETable(
        reinterpret_cast<const SymbolIDEntry *>(RefsOffsetTableChunk.data()),
        RefsOffsetTableChunk.size() / sizeof(SymbolIDEntry));
    auto It = std::partition_point(
        SOETable.begin(), SOETable.end(), [&ID](const SymbolIDEntry &I) {
          return SymbolID::fromRaw({I.SymbolID, SymbolID::RawSize}) < ID;
        });
    if (It == SOETable.end() ||
        !(SymbolID::fromRaw({It->SymbolID, SymbolID::RawSize}) == ID))
      return llvm::None;
    Offset = llvm::support::endian::read32le(It->Offset);
  }

  auto RefsEntry = Content.find("Refs");
  auto RefsChunk = RefsEntry->getValue();
  Reader2 R(StringIDsTable, StringStream,
            {RefsChunk.data() + Offset, RefsChunk.size() - Offset});
  return readRefs(R).second;
}

void getRelationsInContainer(
    llvm::StringRef Data, SymbolID Subject, index::SymbolRole Predicate,
    llvm::function_ref<bool(const Relation &)> Callback) {
  struct RelationEntry {
    char Subject[8];
    char Predicate[4];
    char Object[8];
  };

  Container Content(Data);
  auto RelationsTableEntry = Content.find("Rela");
  auto RelationsTableChunk = RelationsTableEntry->getValue();
  llvm::ArrayRef<RelationEntry> RelationsTable(
      reinterpret_cast<const RelationEntry *>(RelationsTableChunk.data()),
      RelationsTableChunk.size() / sizeof(RelationEntry));
  auto It =
      std::partition_point(RelationsTable.begin(), RelationsTable.end(),
                           [&Subject, &Predicate](const RelationEntry &I) {
                             Reader R(&I, sizeof(RelationEntry));
                             Relation Rel = readRelation(R);
                             return std::tie(Rel.Subject, Rel.Predicate) <
                                    std::tie(Subject, Predicate);
                           });
  while (It != RelationsTable.end()) {
    Reader R(It, sizeof(RelationEntry));
    Relation Rel = readRelation(R);
    if (std::tie(Rel.Subject, Rel.Predicate) != std::tie(Subject, Predicate))
      break;
    if (!Callback(Rel))
      break;
    It++;
  }
}

} // namespace container
} // namespace dbindex
} // namespace clangd
} // namespace clang
