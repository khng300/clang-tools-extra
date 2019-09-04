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

/// This class wraps an error for RecordID allocator
class LMDBIndexError final : public llvm::ErrorInfo<LMDBIndexError> {
public:
  /// Error Code
  enum ErrorCode {
    NOT_FOUND,
    DB_ERROR,
  };

  /// Used by ErrorInfo::classID.
  static char ID;

  /// Construct an error with origin and return code
  LMDBIndexError(ErrorCode EC) : EC(EC) {}

  /// Log the error to output stream
  void log(llvm::raw_ostream &OS) const override { OS << strerror() << '\n'; }

  /// Convert the error to std::error_code
  std::error_code convertToErrorCode() const override;

  /// Get message related to the error
  const char *strerror() const;

  /// Get error code
  ErrorCode errorCode() const { return EC; }

private:
  /// Error code
  ErrorCode EC;
};

/// \brief Indexing Database based on LMDB
class LMDBIndex {
  /// \brief Names for RecordID allocators and databases
  static const char *RECORDID_ALLOCATOR_RECORDS;
  static const char *DB_SHARDS;
  static const char *DB_FILEPATH_INFO;
  static const char *DB_SYMBOLID_TO_SHARDS;
  static const char *DB_SYMBOLID_TO_REF_RECS;
  static const char *DB_SYMBOLID_TO_RELATION_RECS;
  static const char *DB_SYMBOLID_TO_SYMBOLS;
  static const char *DB_TRIGRAM_TO_SYMBOLID;
  static const char *DB_SCOPEDIGEST_TO_SYMBOLID;

  /// \brief Friends
  friend class LMDBSymbolIndex;

public:
  /// \brief opens an LMDB Index at \p Path
  static std::unique_ptr<LMDBIndex> open(PathRef Path);

  /// \brief get shard with corresponding \p FilePath
  llvm::Expected<IndexFileIn> get(llvm::StringRef FilePath);

  /// \brief update interface for a file
  llvm::Error update(llvm::StringRef FilePath, const IndexFileOut &Shard);

private:
  /// \brief remove Symbol with given SymbolID and the associations with it from
  /// database
  llvm::Error removeSymbolFromStore(lmdb::Txn &Txn, SymbolID ID);

  /// \brief update Symbol with given SymbolID and the associations with it if
  /// not already existed in database
  llvm::Error updateSymbolToStore(lmdb::Txn &Txn, Symbol &S);

  /// \brief updates in the unit of a file
  llvm::Error updateFile(llvm::StringRef FilePath, const IndexFileOut &Shard);

  /// \brief get Symbol with a specified SymbolID. Perform merging of decl/defs
  /// in place
  llvm::Expected<Symbol> getSymbol(lmdb::Txn &Txn, SymbolID ID);

  /// Environment handle
  lmdb::Env DBEnv;
  /// Allocator database
  RecordIDAllocator IDAllocator;
  /// Shards database handle
  lmdb::DBI DBIShards;
  /// Key-RecordID database handle
  lmdb::DBI DBISymbolIDToShards;
  lmdb::DBI DBISymbolIDToRefShards;
  lmdb::DBI DBISymbolIDToRelationShards;
  lmdb::DBI DBISymbolIDToSymbols;
  lmdb::DBI DBITrigramToSymbolID;
  lmdb::DBI DBIScopeDigestToSymbolID;
};

/// \brief SymbolIndex interface exported from Indexing database
class LMDBSymbolIndex : public SymbolIndex {
public:
  LMDBSymbolIndex(std::shared_ptr<LMDBIndex> DBIndex) : DBIndex(DBIndex) {}

  bool
  fuzzyFind(const FuzzyFindRequest &Req,
            llvm::function_ref<void(const Symbol &)> Callback) const override;

  void lookup(const LookupRequest &Req,
              llvm::function_ref<void(const Symbol &)> Callback) const override;

  void refs(const RefsRequest &Req,
            llvm::function_ref<void(const Ref &)> Callback) const override;

  void relations(const RelationsRequest &Req,
                 llvm::function_ref<void(const SymbolID &, const Symbol &)>
                     Callback) const override;

  size_t estimateMemoryUsage() const override { return 0; };

private:
  std::shared_ptr<LMDBIndex> DBIndex;
};

} // namespace dbindex
} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_DATABASESTORAGE_H