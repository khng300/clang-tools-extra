#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_RECORDID_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_RECORDID_H

#include "index/index-db/lmdb-cxx.h"

namespace clang {
namespace clangd {
namespace dbindex {

/// Type of id
using RecordID = uint64_t;

/// This class wraps an error for RecordID allocator
class RecordIDAllocError final : public llvm::ErrorInfo<RecordIDAllocError> {
public:
  /// Error Code
  enum ErrorCode {
    NO_FREE_RECORDIDS,
	DB_ERROR,
  };

  /// Used by ErrorInfo::classID.
  static char ID;

  /// Construct an error with origin and return code
  RecordIDAllocError(ErrorCode EC) : EC(EC) {}

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

/// Allocator interface. For any failure lmdb::DBError will
/// be returned.
struct RecordIDAllocator final {
  // Constructor
  RecordIDAllocator() = default;

  // Constructor
  RecordIDAllocator(RecordIDAllocator &&A) { *this = std::move(A); }

  // Close the allocator
  ~RecordIDAllocator(){};

  // Move assignment
  RecordIDAllocator &operator=(RecordIDAllocator &&A) {
    RecordIDAllocator::~RecordIDAllocator();
    DBI = std::move(A.DBI);
    return *this;
  }

  // Open/create the allocator in the database given.
  static llvm::Expected<RecordIDAllocator>
  open(lmdb::Txn &Txn, llvm::StringRef AllocatorName = "RecordID Allocator");

  // Allocate an ID
  llvm::Expected<std::pair<RecordID, RecordID>> Allocate(lmdb::Txn &Txn,
                                                         RecordID Length);

  // Free an ID
  llvm::Error Free(lmdb::Txn &Txn, RecordID ID, RecordID Length);

private:
  // dbi of the allocator
  lmdb::DBI DBI;
};

} // namespace dbindex
} // namespace clangd
} // namespace clang

#endif // __ALLOCATOR_H__