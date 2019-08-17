#include "index/index-db/RecordID.h"
#include "llvm/Support/Endian.h"

namespace clang {
namespace clangd {
namespace dbindex {

/// The maximal length of an extent allowed
static constexpr uint64_t MaximumLength = UINT64_MAX;

/// Extent representing a range of free ID
struct FreeIdExtent {
  /// Starting ID that is free
  RecordID ID;
  /// Length of the extent
  RecordID Length;

  /// Initialize the extent
  FreeIdExtent(RecordID ID, RecordID Length) : ID(ID), Length(Length) {}
};

/// Class ID for RecordIDAllocError.
/// Actually isA() is implemented by comparing address of ErrorInfo::ID in LLVM
char RecordIDAllocError::ID = 0;

std::error_code RecordIDAllocError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

const char *RecordIDAllocError::strerror() const {
  switch (EC) {
  case NO_FREE_RECORDIDS:
    return "No free records available";
  case DB_ERROR:
    return "DB error";
  }
  return "Unknown";
}

llvm::Expected<RecordIDAllocator>
RecordIDAllocator::open(lmdb::Txn &Txn, llvm::StringRef AllocatorName) {
  auto DBI = lmdb::DBI::open(Txn, AllocatorName.data(), 0);
  if (!DBI) {
    if (DBI.getError() != lmdb::makeErrorCode(MDB_NOTFOUND))
      return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
    DBI =
        lmdb::DBI::open(Txn, AllocatorName.data(), MDB_CREATE | MDB_INTEGERKEY);
    if (!DBI)
      return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
    FreeIdExtent Extent(0, MaximumLength);
    lmdb::Val Key(&Extent.ID), Data(&Extent.Length);
    if (auto Err = DBI->put(Txn, Key, Data, 0))
      return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
  }

  RecordIDAllocator Allocator;
  Allocator.DBI = std::move(*DBI);
  return Allocator;
}

/// Allocate IDs from the free extent database
///
/// Currently the routine is very simple - it looks up the first extent in the
/// free ID database and return the starting ID of the found extent to the
/// caller. It then returns the minimal consecutive run of IDs allocated.
///
/// The first field of returned pair is ID, and the second field is length.
llvm::Expected<std::pair<RecordID, RecordID>>
RecordIDAllocator::Allocate(lmdb::Txn &Txn, RecordID Length) {
  lmdb::Val Key, Data;
  llvm::ErrorOr<lmdb::Cursor> Cursor = lmdb::Cursor::open(Txn, DBI);
  if (!Cursor)
    return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);

  auto Err = Cursor->get(Key, Data, MDB_FIRST);
  if (Err == lmdb::makeErrorCode(MDB_NOTFOUND))
    return llvm::make_error<RecordIDAllocError>(
        RecordIDAllocError::NO_FREE_RECORDIDS);
  else if (Err)
    return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);

  FreeIdExtent Extent(*Key.data<RecordID>(), *Data.data<RecordID>());
  RecordID AllocatedID = Extent.ID;
  RecordID AllocatedLength = std::min(Extent.Length, Length);
  if (auto Err = Cursor->del())
    return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
  Extent.ID += AllocatedLength;
  Extent.Length -= AllocatedLength;
  if (Extent.Length) {
    Key.assign(&Extent.ID, sizeof(RecordID));
    Data.assign(&Extent.Length, sizeof(RecordID));
    if (auto Err = Cursor->put(Key, Data, 0))
      return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
  }

  return std::make_pair(AllocatedID, AllocatedLength);
}

/// Check if the two extents are consecutive (providing that #a must be smaller
/// than #b)
static inline bool AllocatorCheckConsecutive(const FreeIdExtent &A,
                                             const FreeIdExtent &B) {
  return A.ID + A.Length == B.ID;
}

/// Check if two extents overlap
static inline bool AllocatorCheckExtentOverlap(const FreeIdExtent &Extent,
                                               const FreeIdExtent &NewExtent) {
  return NewExtent.ID <= Extent.ID + Extent.Length - 1 &&
         Extent.ID <= NewExtent.ID + NewExtent.Length - 1;
}

/// Free an ID to the free extent database
///
/// Double freeing of an ID is prohibited.
llvm::Error RecordIDAllocator::Free(lmdb::Txn &Txn, RecordID ID,
                                    RecordID Length) {
  llvm::ErrorOr<lmdb::Cursor> Cursor = lmdb::Cursor::open(Txn, DBI);
  if (!Cursor)
    return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);

  FreeIdExtent Extent(ID, 0);
  lmdb::Val ExtentKey(&Extent.ID), ExtentData;
  bool AllocatorFull = false;
  // First find an free extent with its ID greater than #ID
  auto Err = Cursor->get(ExtentKey, ExtentData, MDB_SET_RANGE);
  if (Err == lmdb::makeErrorCode(MDB_NOTFOUND)) {
    // Get the last free extent in the database instead
    Err = Cursor->get(ExtentKey, ExtentData, MDB_LAST);
    if (Err == lmdb::makeErrorCode(MDB_NOTFOUND))
      AllocatorFull = true;
    else if (Err)
      return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
  } else if (Err)
    return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);

  // #NewExtent is the extent to be inserted into the database
  FreeIdExtent NewExtent(ID, Length);
  if (!AllocatorFull) {
    // There is at least one free extent presented in the database
    Extent.ID = *ExtentKey.data<RecordID>();
    Extent.Length = *ExtentData.data<RecordID>();
    // Sanity check - the range to be freed must not be in database
    assert(!AllocatorCheckExtentOverlap(Extent, NewExtent));
    if (ID > Extent.ID) {
      // Check if we can merge the extent smaller than #NewExtent
      if (AllocatorCheckConsecutive(Extent, NewExtent)) {
        NewExtent.ID = Extent.ID;
        NewExtent.Length += Extent.Length;
        Err = Cursor->del();
        if (Err)
          return llvm::make_error<RecordIDAllocError>(
              RecordIDAllocError::DB_ERROR);
      }
      // We don't need to check the next extent in this case, as we can only
      // reach there if there is no more extent greater than #ID (Recall that we
      // failed the first lookup)
    } else {
      // Check if we can merge the extent greater than #NewExtent
      if (AllocatorCheckConsecutive(NewExtent, Extent)) {
        NewExtent.Length += Extent.Length;
        Err = Cursor->del();
        if (Err)
          return llvm::make_error<RecordIDAllocError>(
              RecordIDAllocError::DB_ERROR);
      }

      // Check if merging with extents preceding #NewExtent is possible
      Err = Cursor->get(ExtentKey, ExtentData, MDB_PREV);
      if (Err && Err != lmdb::makeErrorCode(MDB_NOTFOUND))
        return llvm::make_error<RecordIDAllocError>(
            RecordIDAllocError::DB_ERROR);
      else if (!Err) {
        Extent.ID = *ExtentKey.data<RecordID>();
        Extent.Length = *ExtentData.data<RecordID>();
        // Sanity check - the range to be freed must not be in database
        assert(!AllocatorCheckExtentOverlap(Extent, NewExtent));
        // Check if we can merge the extent less than #NewExtent
        if (AllocatorCheckConsecutive(Extent, NewExtent)) {
          NewExtent.ID = Extent.ID;
          NewExtent.Length += Extent.Length;
          Err = Cursor->del();
          if (Err)
            return llvm::make_error<RecordIDAllocError>(
                RecordIDAllocError::DB_ERROR);
        }
      }
    }
  }
  // Insert the resulting new extent
  ExtentKey.assign(&NewExtent.ID, sizeof(RecordID));
  ExtentData.assign(&NewExtent.Length, sizeof(RecordID));
  Err = Cursor->put(ExtentKey, ExtentData, 0);
  if (Err)
    return llvm::make_error<RecordIDAllocError>(RecordIDAllocError::DB_ERROR);
  return llvm::Error::success();
}

} // namespace dbindex
} // namespace clangd
} // namespace clang
