//===--- DatabaseStorage.h - Dynamic on-disk symbol index. ------- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_DATABASESTORAGE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_DATABASESTORAGE_H

#include "../../Headers.h"
#include "../../Path.h"
#include "../Index.h"
#include "../Serialization.h"
#include "RecordID.h"
#include "lmdb-cxx.h"
#include <mutex>

namespace clang {
namespace clangd {
namespace dbindex {

/// \brief Indexing Database based on LMDB
class LMDBIndex {
  /// \brief Names for RecordID allocators and databases
  static const char *RECORDID_ALLOCATOR_RECORDS;
  static const char *DB_RECORDS;
  static const char *DB_FILEPATH_INFO;
  static const char *DB_FILEID_TO_SYMBOL_RECS;
  static const char *DB_FILEID_TO_REF_RECS;
  static const char *DB_SYMBOLID_TO_SYMBOLS;
  static const char *DB_SYMBOLID_TO_REFS;
  static const char *DB_TRIGRAM_TO_SYMBOLID;
  static const char *DB_SCOPE_TO_SYMBOLID;

  /// \brief Friends
  friend class LMDBSymbolIndex;

public:
  /// \brief opens an LMDB Index at \p Path
  static std::unique_ptr<LMDBIndex> open(PathRef Path);

  /// \brief Get FileDigest interface for a file
  llvm::Optional<FileDigest> getFileDigest(llvm::StringRef FilePath);

  /// \brief Update interface for a file
  llvm::Error update(llvm::StringRef FilePath, FileDigest Digest,
                     const SymbolSlab *SS, const RefSlab *RS);

private:
  /// \brief inserts serialized data, then returns RecID
  llvm::Expected<RecordID> addRecord(lmdb::Txn &Txn,
                                     llvm::ArrayRef<uint8_t> Data);

  /// \brief deletes record with \p RecID
  llvm::Error delRecord(lmdb::Txn &Txn, RecordID ID);

  /// \brief updates data of record with \p RecID
  llvm::Error updateRecord(lmdb::Txn &Txn, RecordID RecID,
                           llvm::ArrayRef<uint8_t> Data);

  /// \brief get data with \p RecID
  llvm::Expected<llvm::ArrayRef<uint8_t>> getRecord(lmdb::Txn &Txn,
                                                    RecordID RecID);

  /// \brief updates in the unit of a file
  llvm::Error updateFile(llvm::StringRef FilePath, FileDigest Digest,
                         const SymbolSlab *SS, const RefSlab *RS);

  /// \brief get Symbol with a specified SymbolID. Perform merging of decl/defs
  /// in place
  llvm::Optional<Symbol> GetSymbol(lmdb::Txn &Txn, SymbolID ID,
                                   llvm::StringSaver &Strings);

  /// Environment handle
  lmdb::Env DBEnv;
  /// Allocator database
  RecordIDAllocator IDAllocator;
  /// Record database handle
  lmdb::DBI DBIRecords;
  /// Key-RecordID database handle
  lmdb::DBI DBIFilePathToFileInfo;
  lmdb::DBI DBIFileIDToSymbolRecs;
  lmdb::DBI DBIFileIDToRefRecs;
  lmdb::DBI DBISymbolIDToSymbols;
  lmdb::DBI DBISymbolIDToRefs;
  lmdb::DBI DBITrigramToSymbolID;
  lmdb::DBI DBIScopeToSymbolID;
};

/// \brief SymbolIndex interface exported from Indexing database
class LMDBSymbolIndex : public SymbolIndex {
public:
  LMDBSymbolIndex(LMDBIndex *DBIndex) : DBIndex(DBIndex) {}

  bool
  fuzzyFind(const FuzzyFindRequest &Req,
            llvm::function_ref<void(const Symbol &)> Callback) const override;

  void lookup(const LookupRequest &Req,
              llvm::function_ref<void(const Symbol &)> Callback) const override;

  void refs(const RefsRequest &Req,
            llvm::function_ref<void(const Ref &)> Callback) const override;

  size_t estimateMemoryUsage() const override { return 0; };

private:
  LMDBIndex *DBIndex;
};

} // namespace dbindex
} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_DATABASESTORAGE_H