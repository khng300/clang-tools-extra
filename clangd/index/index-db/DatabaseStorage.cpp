//===--- DatabaseStorage.cpp - Dynamic on-disk symbol index. ------C++-*-===//

#include "DatabaseStorage.h"
#include "Container.h"
#include "DBTrigram.h"
#include "FuzzyMatch.h"
#include "Logger.h"
#include "Quality.h"
#include "RecordIDIterator.h"
#include "RecordIDPostingList.h"
#include "URI.h"
#include "index/Merge.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/BinaryStreamWriter.h"
#include "llvm/Support/FileSystem.h"

namespace clang {
namespace clangd {
namespace dbindex {

/// Class ID for RecordIDAllocError.
/// Actually isA() is implemented by comparing address of ErrorInfo::ID in LLVM
char LMDBIndexError::ID = 0;

std::error_code LMDBIndexError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

const char *LMDBIndexError::strerror() const {
  switch (EC) {
  case NOT_FOUND:
    return "Not found";
  case DB_ERROR:
    return "DB error";
  }
  return "Unknown";
}

/// \brief Query Tree Iterator implementation for std::vector
class VectorIterator : public Iterator {
public:
  VectorIterator(llvm::ArrayRef<DocID> V) : IDs(V), It(V.begin()) {}

  /// Check if the vector iterator reaches the end of vector
  bool reachedEnd() const override { return It == IDs.end(); }

  /// Advance the vector iterator to the next element
  void advance() override {
    assert(!reachedEnd() && "Vector iterator can't advance() at the end.");
    It++;
  }

  /// Advance the vector iterator to the next element with given ID
  void advanceTo(DocID ID) override {
    assert(!reachedEnd() && "Vector iterator can't advanceTo() at the end.");
    llvm::partition_point(llvm::make_range(It, IDs.end()),
                          [&ID](RecordID I) { return I < ID; });
  }

  /// Return the DocID under the vector iterator to the current element
  DocID peek() const override {
    assert(!reachedEnd() && "Vector iterator can't peek() at the end.");
    return *It;
  }

  float consume() override {
    assert(!reachedEnd() && "Vector iterator can't consume() at the end.");
    return 1;
  }

  size_t estimateSize() const override { return IDs.size(); }

private:
  llvm::raw_ostream &dump(llvm::raw_ostream &OS) const override {
    OS << '[';
    const char *Sep = "";
    for (const DocID Doc : IDs) {
      OS << Sep << Doc;
      Sep = " ";
    }
    return OS << ']';
  }

  llvm::ArrayRef<DocID> IDs;
  decltype(IDs)::iterator It;
};

/// Names for allocators and databases
const char *LMDBIndex::RECORDID_ALLOCATOR_RECORDS = "IDAllocator for records";
const char *LMDBIndex::DB_SHARDS = "INDEXDB.SHARDS";
const char *LMDBIndex::DB_FILEPATH_INFO = "INDEXDB.FILEPATH";
const char *LMDBIndex::DB_SYMBOLID_TO_SHARDS = "INDEXDB.SYMBOLID_SHARDS";
const char *LMDBIndex::DB_SYMBOLID_TO_REF_RECS = "INDEXDB.SYMBOLID_REFS";
const char *LMDBIndex::DB_SYMBOLID_TO_RELATION_RECS =
    "INDEXDB.SYMBOLID_RELATIONS";
const char *LMDBIndex::DB_SYMBOLID_TO_SYMBOLS = "INDEXDB.SYMBOLID_SYMBOLS";
const char *LMDBIndex::DB_TRIGRAM_TO_SYMBOLID = "INDEXDB.TRIGRAM_SYMBOLID";
const char *LMDBIndex::DB_SCOPE_TO_SYMBOLID = "INDEXDB.SCOPE_SYMBOLID";

/// Hash Subject-Predicate
static FileDigest makeSubjectPredicateDigest(SymbolID Subject,
                                             index::SymbolRole Predicate) {
  struct SPEntry {
    char Subject[8];
    char Predicate[4];
  } SP;
  std::copy(Subject.raw().begin(), Subject.raw().end(), SP.Subject);
  llvm::support::endian::write32(SP.Predicate, static_cast<uint32_t>(Predicate),
                                 llvm::support::endianness::little);
  return digest({reinterpret_cast<char *>(&SP), sizeof(SP)});
}

/// Own \Sym by copying the strings inside to \p StringStore
static void OwnSymbol(Symbol &Sym, llvm::StringSaver &StringStore) {
  visitStrings(Sym, [&StringStore](llvm::StringRef &Str) {
    Str = StringStore.save(Str);
  });
}

std::unique_ptr<LMDBIndex> LMDBIndex::open(PathRef Path) {
  // If the database environment directory exists we
  // do not need a new one
  if (llvm::sys::fs::create_directory(Path, true))
    return nullptr;
  llvm::ErrorOr<lmdb::Env> Env = lmdb::Env::create();
  if (!Env)
    return nullptr;

  // The size of database is now 512G in maximum
  auto Err = Env->setMapsize(1ull << 39);
  if (Err)
    return nullptr;
  // Now we need 10 databases for our use including 1 allocator
  Err = Env->setMaxDBs(10);
  if (Err)
    return nullptr;
  // No metasync during open as we do not need durability
  Err = Env->open(Path, MDB_NOMETASYNC);
  if (Err)
    return nullptr;

  // Now starts a transaction so that we can persist changes to database,
  // i.e. creation of ID allocators and databases
  llvm::ErrorOr<lmdb::Txn> Txn = lmdb::Txn::begin(*Env);
  if (!Txn)
    return nullptr;

  llvm::Expected<RecordIDAllocator> IDAllocator =
      RecordIDAllocator::open(*Txn, LMDBIndex::RECORDID_ALLOCATOR_RECORDS);
  if (llvm::errorToBool(IDAllocator.takeError()))
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBIShards =
      lmdb::DBI::open(*Txn, LMDBIndex::DB_SHARDS, MDB_CREATE);
  if (!DBIShards)
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBISymbolIDToShards = lmdb::DBI::open(
      *Txn, LMDBIndex::DB_SYMBOLID_TO_SHARDS, MDB_CREATE | MDB_DUPSORT);
  if (!DBISymbolIDToShards)
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBISymbolIDToRefShards = lmdb::DBI::open(
      *Txn, LMDBIndex::DB_SYMBOLID_TO_REF_RECS, MDB_CREATE | MDB_DUPSORT);
  if (!DBISymbolIDToRefShards)
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBISymbolIDToRelationShards = lmdb::DBI::open(
      *Txn, LMDBIndex::DB_SYMBOLID_TO_RELATION_RECS, MDB_CREATE | MDB_DUPSORT);
  if (!DBISymbolIDToRelationShards)
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBISymbolIDToSymbols =
      lmdb::DBI::open(*Txn, LMDBIndex::DB_SYMBOLID_TO_SYMBOLS, MDB_CREATE);
  if (!DBISymbolIDToSymbols)
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBITrigramToSymbolID = lmdb::DBI::open(
      *Txn, LMDBIndex::DB_TRIGRAM_TO_SYMBOLID, MDB_CREATE | MDB_DUPSORT);
  if (!DBITrigramToSymbolID)
    return nullptr;
  llvm::ErrorOr<lmdb::DBI> DBIScopeToSymbolID = lmdb::DBI::open(
      *Txn, LMDBIndex::DB_SCOPE_TO_SYMBOLID, MDB_CREATE | MDB_DUPSORT);
  if (!DBIScopeToSymbolID)
    return nullptr;

  // Commit the changes to storage in one go
  Err = Txn->commit();
  if (Err)
    return nullptr;

  auto LMDBIndexPtr = llvm::make_unique<LMDBIndex>();
  LMDBIndexPtr->DBEnv = std::move(*Env);
  LMDBIndexPtr->IDAllocator = std::move(*IDAllocator);
  LMDBIndexPtr->DBIShards = std::move(*DBIShards);
  LMDBIndexPtr->DBISymbolIDToShards = std::move(*DBISymbolIDToShards);
  LMDBIndexPtr->DBISymbolIDToRefShards = std::move(*DBISymbolIDToRefShards);
  LMDBIndexPtr->DBISymbolIDToRelationShards =
      std::move(*DBISymbolIDToRelationShards);
  LMDBIndexPtr->DBISymbolIDToSymbols = std::move(*DBISymbolIDToSymbols);
  LMDBIndexPtr->DBITrigramToSymbolID = std::move(*DBITrigramToSymbolID);
  LMDBIndexPtr->DBIScopeToSymbolID = std::move(*DBIScopeToSymbolID);
  return LMDBIndexPtr;
}

llvm::Expected<IndexFileIn> LMDBIndex::get(llvm::StringRef FilePath) {
  auto Txn = lmdb::Txn::begin(DBEnv, nullptr, MDB_RDONLY);
  if (!Txn)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);

  lmdb::Val ShardContent;
  auto Err = DBIShards.get(*Txn, lmdb::Val(digest(FilePath)), ShardContent);
  if (Err == lmdb::makeErrorCode(MDB_NOTFOUND))
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::NOT_FOUND);
  else if (Err)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  auto Ret = container::readContainer(ShardContent);
  Txn->commit();
  return Ret;
}

llvm::Error LMDBIndex::update(llvm::StringRef FilePath,
                              const IndexFileOut &Shard) {
  const auto TimerStart = std::chrono::high_resolution_clock::now();
  auto Err = updateFile(FilePath, Shard);
  const auto TimerStop = std::chrono::high_resolution_clock::now();
  const auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      TimerStop - TimerStart);
  log("Update of {0} took {1}. Error: {2}. Indexed ({3} symbols, {4} "
      "refs)\n",
      FilePath, Duration, Err, Shard.Symbols ? Shard.Symbols->size() : 0,
      Shard.Refs ? Shard.Refs->numRefs() : 0);
  return Err;
}

llvm::Error LMDBIndex::removeSymbolFromStore(lmdb::Txn &Txn, SymbolID ID) {
  lmdb::Val V;
  auto Err = DBISymbolIDToSymbols.get(Txn, ID.raw(), V);
  if (Err)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  Symbol S = container::readSymbol(V);
  std::vector<Token> Trigrams = generateIdentifierTrigrams(S.Name);
  // Remove trigram tokens corresponding to the Symbol
  for (auto &I : Trigrams) {
    Err = DBITrigramToSymbolID.del(Txn, I.Data, {ID.raw()});
    if (Err)
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  }
  if (S.Scope.size()) {
    // In case the Symbol has scope, remove scope tokens corresponding to
    // the Symbol
    Err = DBIScopeToSymbolID.del(Txn, S.Scope, {ID.raw()});
    if (Err)
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  }
  // Remove the corresponding Symbol from Symbols database
  Err = DBISymbolIDToSymbols.del(Txn, ID.raw(), llvm::None);
  if (Err)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  return llvm::Error::success();
};

llvm::Error LMDBIndex::updateSymbolToStore(lmdb::Txn &Txn, Symbol &S) {
  // Generate trigram tokens corresponding to the unqualified name of
  // the symbol. Then, insert trigram tokens to SymbolID associations.

  // Check whether the Symbol exists in SymbolID -> Symbol database
  // If the Symbol exists in the database, skip the process of updating
  // trigrams and scope of the corresponding Symbol
  lmdb::Val V;
  auto Err = DBISymbolIDToSymbols.get(Txn, S.ID.raw(), V);
  if (Err == lmdb::makeErrorCode(MDB_NOTFOUND)) {
    std::vector<Token> Trigrams = generateIdentifierTrigrams(S.Name);
    for (auto &TI : Trigrams) {
      // For each trigrams of the identifier name, update the Trigram ->
      // SymbolIDs database
      Err = DBITrigramToSymbolID.put(Txn, TI.Data, S.ID.raw(), MDB_NODUPDATA);
      if (Err && Err != lmdb::makeErrorCode(MDB_KEYEXIST))
        return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    }
    if (S.Scope.size()) {
      // In case the symbol has parent scope, insert unqualified name to
      // qualified name association.
      Err = DBIScopeToSymbolID.put(Txn, S.Scope, S.ID.raw(), MDB_NODUPDATA);
      if (Err && Err != lmdb::makeErrorCode(MDB_KEYEXIST))
        return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    }
  } else if (Err)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  // Update SymbolID -> Symbol database
  Err = DBISymbolIDToSymbols.put(Txn, S.ID.raw(), container::writeSymbol(S), 0);
  if (Err)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  return llvm::Error::success();
};

llvm::Error LMDBIndex::updateFile(llvm::StringRef FilePath,
                                  const IndexFileOut &Shard) {
  llvm::DenseSet<SymbolID> TouchedSyms;
  auto Txn = lmdb::Txn::begin(DBEnv);
  if (!Txn)
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);

  llvm::Optional<IndexFileIn> OldShard;
  {
    lmdb::Val OldShardContent;
    auto Err =
        DBIShards.get(*Txn, lmdb::Val(digest(FilePath)), OldShardContent);
    if (Err && Err != lmdb::makeErrorCode(MDB_NOTFOUND))
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    else if (!Err) {
      auto IndexFile = container::readContainer(OldShardContent);
      if (!IndexFile)
        return IndexFile.takeError();
      OldShard = std::move(*IndexFile);
    }
  }
  if (OldShard && Shard.Sources) {
    bool Modified = false;
    // If digest of old and new shard are the same we skip index updating
    for (auto &I : *OldShard->Sources) {
      const auto Target = Shard.Sources->find(I.first());
      if (Target == Shard.Sources->end() ||
          Target->second.Digest != I.second.Digest) {
        Modified = true;
        break;
      }
    }
    Modified |= Shard.Sources->size() != OldShard->Sources->size();
    if (!Modified)
      return llvm::Error::success();
  }

  // Current we use hashed \p FilePath as file ID.
  FileDigest FileID = digest(FilePath);

  if (OldShard) {
    // Delete all the \p SS related to this file
    if (OldShard->Symbols) {
      for (auto &S : *OldShard->Symbols) {
        if (auto Err =
                DBISymbolIDToShards.del(*Txn, S.ID.raw(), lmdb::Val(FileID)))
          return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
        TouchedSyms.insert(S.ID);
      }
    }
    // Delete all the \p RS related to this file
    if (OldShard->Refs) {
      for (auto &RP : *OldShard->Refs) {
        SymbolID ID = RP.first;
        if (auto Err =
                DBISymbolIDToRefShards.del(*Txn, ID.raw(), lmdb::Val(FileID)))
          return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
      }
    }
    // Delete all \p Relation related to this file
    if (OldShard->Relations) {
      for (auto &R : *Shard.Relations) {
        auto SPDigest = makeSubjectPredicateDigest(R.Subject, R.Predicate);
        if (DBISymbolIDToRelationShards.del(*Txn, llvm::toStringRef(SPDigest),
                                            lmdb::Val(FileID)))
          return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
      }
    }
  }
  if (!Shard.Sources) {
    // IF no Sources is given, that implies the file is removed.
    if (OldShard) {
      auto Err = DBIShards.del(*Txn, lmdb::Val(FileID), llvm::None);
      if (Err)
        return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    }
    auto Err = Txn->commit();
    if (Err)
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    return llvm::Error::success();
  }

  std::string ShardContent;
  {
    llvm::raw_string_ostream OS(ShardContent);
    container::writeContainer(Shard, OS);
  }
  // Put serialized shard into shard database
  if (DBIShards.put(*Txn, lmdb::Val(FileID), ShardContent, 0))
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  if (Shard.Symbols) {
    // Build SymbolID -> hashed ShardIdentifier association
    for (auto &S : *Shard.Symbols) {
      if (DBISymbolIDToShards.put(*Txn, S.ID.raw(), lmdb::Val(FileID), 0))
        return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
      TouchedSyms.insert(S.ID);
    }
  }
  if (Shard.Refs) {
    // Insert SymbolID -> hashed ShardIdentifier mappings
    for (auto &RP : *Shard.Refs) {
      SymbolID ID = RP.first;
      if (DBISymbolIDToRefShards.put(*Txn, ID.raw(), lmdb::Val(FileID), 0))
        return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    }
  }
  if (Shard.Relations) {
    // Insert Subject:Predicate -> hashed ShardIdentifier mappings
    for (auto &R : *Shard.Relations) {
      auto SPDigest = makeSubjectPredicateDigest(R.Subject, R.Predicate);
      if (DBISymbolIDToRelationShards.put(*Txn, llvm::toStringRef(SPDigest),
                                          lmdb::Val(FileID), 0))
        return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    }
  }

  // Iterate the touched Symbols and see whether the corresponding Symbol in
  // Symbols database should be updated or removed
  for (auto &I : TouchedSyms) {
    auto Cursor = lmdb::Cursor::open(*Txn, DBISymbolIDToShards);
    if (!Cursor)
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);

    llvm::BumpPtrAllocator MemPool;
    llvm::StringSaver Strings(MemPool);
    std::error_code E2 = lmdb::successErrorCode();

    // Check if the SymbolID still exists. If it does not, remove the
    // inverted index and scope index related to this symbol. If it does,
    // merge the symbol records corresponding to the same SymbolID to provide
    // a Symbol for query.

    llvm::Optional<Symbol> Sym;
    if (auto Err = Cursor->foreachKey(
            I.raw(), [&](const lmdb::Val &, const lmdb::Val &V) {
              auto FID = *V.data<FileDigest>();
              IndexFileIn Idx;
              Symbol S;
              if (FID != FileID) {
                lmdb::Val ShardContent;
                E2 = DBIShards.get(*Txn, lmdb::Val(FID), ShardContent);
                if (E2)
                  return lmdb::IteratorControl::Stop;
                auto Result = container::getSymbolInContainer(ShardContent, I);
                if (!Result)
                  return lmdb::IteratorControl::Stop;
                S = *Result;
              } else
                S = *Shard.Symbols->find(I);

              // We must be able to find the symbol in the shard otherwise it
              // indicates inconsistencies.
              OwnSymbol(S, Strings);
              if (!Sym)
                Sym = S;
              else
                Sym = mergeSymbol(*Sym, S);
              return lmdb::IteratorControl::Continue;
            }))
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    if (E2)
      return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
    if (!Sym) {
      // No SymbolID -> hashed ShardIdentifier mapping exists, thus clean up the
      // trigram inverted index and scope index
      if (auto Err = removeSymbolFromStore(*Txn, I))
        return Err;
    } else {
      // There exists SymbolID -> hashed ShardIdentifier mappings, thus update
      // the Symbol in Symbols database
      if (auto Err = updateSymbolToStore(*Txn, *Sym))
        return Err;
    }
  }

  // The final step is to commit all the changes made
  if (Txn->commit())
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::DB_ERROR);
  return llvm::Error::success();
}

llvm::Expected<Symbol> LMDBIndex::getSymbol(lmdb::Txn &Txn, SymbolID ID) {
  lmdb::Val V;
  if (auto Err = DBISymbolIDToSymbols.get(Txn, ID.raw(), V))
    return llvm::make_error<LMDBIndexError>(LMDBIndexError::NOT_FOUND);
  return container::readSymbol(V);
};

bool LMDBSymbolIndex::fuzzyFind(
    const FuzzyFindRequest &Req,
    llvm::function_ref<void(const Symbol &)> Callback) const {
  auto Txn = lmdb::Txn::begin(DBIndex->DBEnv, nullptr, MDB_RDONLY);
  if (!Txn)
    return false;

  Corpus CorpusSet(-1ull);
  std::vector<std::unique_ptr<Iterator>> Criteria;
  FuzzyMatcher Filter(Req.Query);
  bool More = !Req.Query.empty() && Req.Query.size() < 3;

  std::vector<std::vector<DocID>> Toks;
  std::vector<std::unique_ptr<Iterator>> TrigramIterators;
  for (const auto &I : generateQueryTrigrams(Req.Query)) {
    llvm::ErrorOr<lmdb::Cursor> Cursor =
        lmdb::Cursor::open(*Txn, DBIndex->DBITrigramToSymbolID);
    if (!Cursor)
      return false;
    std::vector<DocID> SymbolIDs;
    if (Cursor->foreachKey(I.Data,
                           [&SymbolIDs](const lmdb::Val &, const lmdb::Val &V) {
                             SymbolIDs.push_back(*V.data<DocID>());
                             return lmdb::IteratorControl::Continue;
                           }))
      return false;
    llvm::sort(SymbolIDs, std::less<DocID>());
    Toks.emplace_back(std::move(SymbolIDs));
    TrigramIterators.emplace_back(
        llvm::make_unique<VectorIterator>(Toks.back()));
  }
  Criteria.push_back(CorpusSet.intersect(move(TrigramIterators)));

  std::vector<std::unique_ptr<Iterator>> ScopeIterators;
  for (const auto &I : Req.Scopes) {
    llvm::ErrorOr<lmdb::Cursor> Cursor =
        lmdb::Cursor::open(*Txn, DBIndex->DBIScopeToSymbolID);
    if (!Cursor)
      return false;
    std::vector<DocID> SymbolIDs;
    if (Cursor->foreachKey(I,
                           [&SymbolIDs](const lmdb::Val &, const lmdb::Val &V) {
                             SymbolIDs.push_back(*V.data<DocID>());
                             return lmdb::IteratorControl::Continue;
                           }))
      return false;
    if (SymbolIDs.size()) {
      llvm::sort(SymbolIDs, std::less<DocID>());
      Toks.emplace_back(std::move(SymbolIDs));
      ScopeIterators.emplace_back(
          llvm::make_unique<VectorIterator>(Toks.back()));
    }
  }
  if (Req.AnyScope)
    ScopeIterators.push_back(
        CorpusSet.boost(CorpusSet.all(), ScopeIterators.empty() ? 1.0 : 0.2));
  Criteria.push_back(CorpusSet.unionOf(move(ScopeIterators)));

  using IDAndScore = std::pair<Symbol, float>;
  auto Compare = [](const IDAndScore &LHS, const IDAndScore &RHS) {
    return LHS.second > RHS.second;
  };

  auto Root = CorpusSet.intersect(move(Criteria));
  if (Req.Limit)
    Root = CorpusSet.limit(move(Root), *Req.Limit * 100);
  TopN<IDAndScore, decltype(Compare)> Top(
      Req.Limit ? *Req.Limit : std::numeric_limits<size_t>::max(), Compare);
  for (; !Root->reachedEnd(); Root->advance()) {
    auto DocID = Root->peek();
    auto SymID =
        SymbolID::fromRaw({reinterpret_cast<char *>(&DocID), sizeof(DocID)});
    auto Sym = DBIndex->getSymbol(*Txn, SymID);
    if (!Sym) {
      llvm::consumeError(Sym.takeError());
      return false;
    }
    auto Score = Filter.match(Sym->Name);
    if (Score)
      More |= Top.push({*Sym, *Score * quality(*Sym)});
  }
  for (const auto &I : std::move(Top).items()) {
    Callback(I.first);
  }

  Txn->commit();
  return More;
}

void LMDBSymbolIndex::lookup(
    const LookupRequest &Req,
    llvm::function_ref<void(const Symbol &)> Callback) const {
  llvm::ErrorOr<lmdb::Txn> Txn =
      lmdb::Txn::begin(DBIndex->DBEnv, nullptr, MDB_RDONLY);
  if (!Txn)
    return;

  for (auto &ID : Req.IDs) {
    auto Sym = DBIndex->getSymbol(*Txn, ID);
    if (!Sym) {
      llvm::consumeError(Sym.takeError());
      return;
    }
    Callback(*Sym);
  }

  Txn->commit();
}

void LMDBSymbolIndex::refs(
    const RefsRequest &Req,
    llvm::function_ref<void(const Ref &)> Callback) const {
  auto Txn = lmdb::Txn::begin(DBIndex->DBEnv, nullptr, MDB_RDONLY);
  if (!Txn)
    return;

  for (auto &ID : Req.IDs) {
    llvm::ErrorOr<lmdb::Cursor> Cursor =
        lmdb::Cursor::open(*Txn, DBIndex->DBISymbolIDToRefShards);
    if (!Cursor)
      return;

    std::error_code E2 = lmdb::successErrorCode();
    // Look up all Refs for the SymbolID
    auto Err = Cursor->foreachKey(
        ID.raw(), [&](const lmdb::Val &, const lmdb::Val &V) {
          lmdb::Val ShardContent;
          E2 = DBIndex->DBIShards.get(*Txn, lmdb::Val(V), ShardContent);
          if (E2)
            return lmdb::IteratorControl::Stop;
          auto Refs = container::getRefsInContainer(ShardContent, ID);
          for (const auto &I : *Refs)
            Callback(I);
          return lmdb::IteratorControl::Continue;
        });
    if (E2)
      break;
    if (Err)
      break;
  }

  Txn->commit();
}

void LMDBSymbolIndex::relations(
    const RelationsRequest &Req,
    llvm::function_ref<void(const SymbolID &, const Symbol &)> Callback) const {
  auto Txn = lmdb::Txn::begin(DBIndex->DBEnv, nullptr, MDB_RDONLY);
  if (!Txn)
    return;

  for (auto &Subject : Req.Subjects) {
    llvm::ErrorOr<lmdb::Cursor> Cursor =
        lmdb::Cursor::open(*Txn, DBIndex->DBISymbolIDToRelationShards);
    if (!Cursor)
      return;

    LookupRequest LookupReq;
    auto SPDigest = makeSubjectPredicateDigest(Subject, Req.Predicate);
    std::vector<SymbolID> Objects;
    std::error_code E2 = lmdb::successErrorCode();
    auto Err = Cursor->foreachKey(
        llvm::toStringRef(SPDigest),
        [&](const lmdb::Val &, const lmdb::Val &V) {
          lmdb::Val ShardContent;
          E2 = DBIndex->DBIShards.get(*Txn, lmdb::Val(V), ShardContent);
          if (E2)
            return lmdb::IteratorControl::Stop;
          container::getRelationsInContainer(
              ShardContent, Subject, Req.Predicate, [&](const Relation &Rel) {
                LookupReq.IDs.insert(Rel.Object);
                return true;
              });
          return lmdb::IteratorControl::Continue;
        });
    if (E2)
      break;
    if (Err)
      break;

    lookup(LookupReq, [&](const Symbol &Sym) { Callback(Subject, Sym); });
  }

  Txn->commit();
}

} // namespace dbindex
} // namespace clangd
} // namespace clang
