//===--- LevelDBIndex.cpp - Indexes based on LevelDB. -------------- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "LevelDBIndex.h"
#include "../FuzzyMatch.h"
#include "Logger.h"
#include "Merge.h"
#include "SymbolCollector.h"
#include "SymbolYAML.h"
#include "clang/Index/IndexingAction.h"
#include <queue>

namespace clang {
namespace clangd {

static multiplestore::DbID SymbolIDDbID = 0;
static multiplestore::DbID PathDbID = 1;

//
// TODO: Boundary Check
//
void ComposeKey(const llvm::StringRef FirstKey, const llvm::StringRef SecondKey,
                std::string &CompositedKey) {
  CompositedKey.resize(sizeof(uint64_t) << 1);
  uint64_t *LengthField = reinterpret_cast<uint64_t *>(&CompositedKey[0]);
  *LengthField = FirstKey.size();
  *(LengthField + 1) = SecondKey.size();
  CompositedKey.append(FirstKey.data(), FirstKey.size());
  CompositedKey.append(SecondKey.data(), SecondKey.size());
};

void DecomposeKey(const llvm::StringRef CompositedKey, std::string &FirstKey,
                  std::string &SecondKey) {
  const uint64_t *LengthField =
      reinterpret_cast<const uint64_t *>(CompositedKey.data());
  const char *ContentStart = CompositedKey.data() + (sizeof(uint64_t) << 1);
  FirstKey.assign(ContentStart, *LengthField);
  SecondKey.assign(ContentStart + *LengthField, *(LengthField + 1));
}

class IndexComparator : public multiplestore::Comparator {
public:
  int Compare(const multiplestore::Slice &a,
              const multiplestore::Slice &b) const {
    std::string FirstKey[2], SecondKey[2];
    DecomposeKey({a.data(), a.size()}, FirstKey[0], SecondKey[0]);
    DecomposeKey({b.data(), b.size()}, FirstKey[1], SecondKey[1]);

    int cmp = FirstKey[0].compare(FirstKey[1]);
    if (cmp)
      return cmp;
    return SecondKey[0].compare(SecondKey[1]);
  }

  const char *Name() const { return "LevelDBIndex"; }

  void FindShortestSeparator(std::string *start,
                             const multiplestore::Slice &limit) const {}

  // Changes *key to a short string >= *key.
  // Simple comparator implementations may return with *key unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  void FindShortSuccessor(std::string *key) const {}
};

static SymbolSlab indexAST(ParsedAST *AST) {
  assert(AST && "AST must not be nullptr!");
  SymbolCollector::Options CollectorOpts;
  // FIXME(ioeric): we might also want to collect include headers. We would need
  // to make sure all includes are canonicalized (with CanonicalIncludes), which
  // is not trivial given the current way of collecting symbols: we only have
  // AST at this point, but we also need preprocessor callbacks (e.g.
  // CommentHandler for IWYU pragma) to canonicalize includes.
  CollectorOpts.CollectIncludePath = false;
  CollectorOpts.CountReferences = false;

  SymbolCollector Collector(std::move(CollectorOpts));
  Collector.setPreprocessor(AST->getPreprocessorPtr());
  index::IndexingOptions IndexOpts;
  // We only need declarations, because we don't count references.
  IndexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::DeclarationsOnly;
  IndexOpts.IndexFunctionLocals = false;

  index::indexTopLevelDecls(AST->getASTContext(), AST->getTopLevelDecls(),
                            Collector, IndexOpts);
  return Collector.takeSymbols();
}

bool LevelDBIndex::open(PathRef Path) {
  std::map<multiplestore::DbID, multiplestore::Comparator *> comparators_map;
  multiplestore::MultiDB *MultiDB;
  Comparator.reset(new IndexComparator);
  comparators_map[SymbolIDDbID] = Comparator.get();
  comparators_map[PathDbID] = Comparator.get();
  multiplestore::Options options;
  options.create_if_missing = true;

  multiplestore::Status s = multiplestore::MultiDB::Open(
      options, &comparators_map, Path.str(), &MultiDB);
  if (!s.ok()) {
    log("Failed to open database, reason: " + s.ToString());
    return false;
  }
  IndexDB.reset(MultiDB);
  return true;
}

LevelDBIndex::~LevelDBIndex() {}

void LevelDBIndex::update(PathRef Path, ParsedAST *AST) {
  assert(IndexDB);

  multiplestore::WriteBatch DbUpdates;
  if (!AST) {
    multiplestore::Iterator *Iter = IndexDB->NewIterator({}, PathDbID);
    std::string CompositedKey;
    ComposeKey(Path, {}, CompositedKey);
    for (Iter->Seek(CompositedKey); Iter->Valid(); Iter->Next()) {
      CompositedKey = Iter->key().ToString();

      std::string ResultPathStr, ResultSerializedID;
      DecomposeKey(CompositedKey, ResultPathStr, ResultSerializedID);
      if (Path.compare(ResultPathStr))
        break;

      DbUpdates.Delete(PathDbID, CompositedKey);
      ComposeKey(ResultSerializedID, ResultPathStr, CompositedKey);
      DbUpdates.Delete(SymbolIDDbID, CompositedKey);

      log("Removed Symbol ID: " + ResultSerializedID);
    }
    delete Iter;
  } else {
    auto Slab = indexAST(AST);
    for (const auto &S : Slab) {
      std::string SerializedID = S.ID.str(), CompositedKey;
      std::string SerializedSymbol = SymbolToYAML(S);

      ComposeKey(Path, SerializedID, CompositedKey);
      DbUpdates.Put(PathDbID, CompositedKey, {});
      ComposeKey(SerializedID, Path, CompositedKey);
      DbUpdates.Put(SymbolIDDbID, CompositedKey, SerializedSymbol);
      log("Inserted Symbol ID: " + SerializedID +
          ", Inserted SymbolName: " + S.Name);
    }
  }
  IndexDB->Write({}, &DbUpdates);
}

void LevelDBIndex::lookup(
    const LookupRequest &Req,
    llvm::function_ref<void(const Symbol &)> Callback) const {
  assert(IndexDB);

  multiplestore::Iterator *Iter = IndexDB->NewIterator({}, SymbolIDDbID);
  llvm::BumpPtrAllocator Arena;
  for (const auto &ID : Req.IDs) {
    Arena.Reset();

    std::string SerializedID = ID.str(), CompositedKey;
    ComposeKey(SerializedID, {}, CompositedKey);
    log("Wanted Symbol ID: " + SerializedID);

    Iter->Seek(CompositedKey);
    if (!Iter->Valid())
      continue;

    Symbol::Details Scratch;
    SymbolSlab::Builder UniqueSymbols;
    do {
      std::string ResultSerializedID, ResultPathStr;
      CompositedKey = Iter->key().ToString();
      DecomposeKey(CompositedKey, ResultSerializedID, ResultPathStr);
      if (ResultSerializedID != SerializedID)
        break;

      std::string SerializedSymbol = Iter->value().ToString();
      llvm::yaml::Input Input(SerializedSymbol, &Arena);
      Symbol ResultSym = SymbolFromYAML(Input, Arena);
      if (const Symbol *Existing = UniqueSymbols.find(ID))
        UniqueSymbols.insert(mergeSymbol(*Existing, ResultSym, &Scratch));
      else
        UniqueSymbols.insert(ResultSym);
    } while (Iter->Next(), Iter->Valid());

    SymbolSlab Slab = std::move(UniqueSymbols).build();
    for (const auto &Sym : Slab) {
      log("SymbolName: " + Sym.Name + ", SymbolID: " + SerializedID);
      Callback(Sym);
    }
  }

  delete Iter;
}

bool LevelDBIndex::fuzzyFind(
    const FuzzyFindRequest &Req,
    llvm::function_ref<void(const Symbol &)> Callback) const {
  assert(IndexDB);
  assert(!StringRef(Req.Query).contains("::") &&
         "There must be no :: in query.");

  multiplestore::Iterator *Iter = IndexDB->NewIterator({}, SymbolIDDbID);
  std::priority_queue<std::pair<float, std::unique_ptr<SymbolSlab>>> Top;
  FuzzyMatcher Filter(Req.Query);
  llvm::BumpPtrAllocator Arena;
  bool More = false;
  {
    for (Iter->SeekToFirst(); Iter->Valid();) {
      std::string SerializedID;
      SymbolID ID;
      Symbol::Details Scratch;
      SymbolSlab::Builder UniqueSymbols;
      do {
        std::string ResultSerializedID, ResultPathStr;
        std::string CompositedKey = Iter->key().ToString();
        DecomposeKey(CompositedKey, ResultSerializedID, ResultPathStr);
        if (SerializedID.empty()) {
          SerializedID = ResultSerializedID;
          SerializedID >> ID;
        } else if (ResultSerializedID != SerializedID)
          break;

        std::string SerializedSymbol = Iter->value().ToString();
        llvm::yaml::Input Input(SerializedSymbol, &Arena);
        Symbol ResultSym = SymbolFromYAML(Input, Arena);
        if (const Symbol *Existing = UniqueSymbols.find(ID))
          UniqueSymbols.insert(mergeSymbol(*Existing, ResultSym, &Scratch));
        else
          UniqueSymbols.insert(ResultSym);
      } while (Iter->Next(), Iter->Valid());

      const Symbol *Sym = UniqueSymbols.find(ID);

      // Exact match against all possible scopes.
      if (!Req.Scopes.empty() && !llvm::is_contained(Req.Scopes, Sym->Scope))
        continue;

      if (auto Score = Filter.match(Sym->Name)) {
        Top.emplace(-*Score, new SymbolSlab(std::move(UniqueSymbols).build()));
        if (Top.size() > Req.MaxCandidateCount) {
          More = true;
          Top.pop();
        }
      }
    }
    for (; !Top.empty(); Top.pop())
      Callback(*Top.top().second->begin());
  }
  return More;
}

} // namespace clangd
} // namespace clang