#include "index/databasestorage/RecordID.h"
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
  }
  return "Unknown";
}

llvm::Expected<RecordIDAllocator>
RecordIDAllocator::open(lmdb::Txn &Txn, llvm::StringRef AllocatorName) {
  auto DBI = lmdb::DBI::open(Txn, AllocatorName.data(), 0);
  DBI = llvm::handleExpected(
      std::move(DBI),
      [&]() -> decltype(DBI) {
        auto DBI = lmdb::DBI::open(Txn, AllocatorName.data(),
                                   MDB_CREATE | MDB_INTEGERKEY);
        if (!DBI)
          return DBI;
        FreeIdExtent Extent(0, MaximumLength);
        lmdb::Val Key(&Extent.ID), Data(&Extent.Length);
        if (auto Err = DBI->put(Txn, Key, Data, 0))
          return std::move(Err);
        return DBI;
      },
      [&](std::unique_ptr<lmdb::DBError> ErrorInfo) -> llvm::Error {
        if (ErrorInfo->returnCode() != MDB_NOTFOUND)
          return llvm::Error(std::move(ErrorInfo));
        return llvm::Error::success();
      });
  if (!DBI)
    return llvm::Expected<RecordIDAllocator>(DBI.takeError());

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
  lmdb::Cursor Cursor;
  {
    auto Expected = lmdb::Cursor::open(Txn, DBI);
    if (!Expected)
      return Expected.takeError();
    Cursor = std::move(*Expected);
  }
  if (auto Err = Cursor.get(Key, Data, MDB_FIRST)) {
    Err = llvm::handleErrors(
        std::move(Err),
        [&](std::unique_ptr<lmdb::DBError> ErrorInfo) -> llvm::Error {
          if (ErrorInfo->returnCode() == MDB_NOTFOUND)
            return llvm::Error(llvm::make_unique<RecordIDAllocError>(
                RecordIDAllocError::NO_FREE_RECORDIDS));
          return llvm::Error(std::move(ErrorInfo));
        });
    return std::move(Err);
  }

  FreeIdExtent Extent(*Key.data<RecordID>(), *Data.data<RecordID>());
  RecordID AllocatedID = Extent.ID;
  RecordID AllocatedLength = std::min(Extent.Length, Length);
  if (auto Err = Cursor.del())
    return std::move(Err);
  Extent.ID += AllocatedLength;
  Extent.Length -= AllocatedLength;
  if (Extent.Length) {
    Key.assign(&Extent.ID, sizeof(RecordID));
    Data.assign(&Extent.Length, sizeof(RecordID));
    if (auto Err = Cursor.put(Key, Data, 0))
      return std::move(Err);
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
  lmdb::Cursor Cursor;
  {
    auto Expected = lmdb::Cursor::open(Txn, DBI);
    if (!Expected)
      return Expected.takeError();
    Cursor = std::move(*Expected);
  }

  FreeIdExtent Extent(ID, 0);
  lmdb::Val ExtentKey(&Extent.ID), ExtentData;
  bool AllocatorFull = false;
  // First find an free extent with its ID greater than #ID
  auto Err = Cursor.get(ExtentKey, ExtentData, MDB_SET_RANGE);
  if (Err) {
    Err = lmdb::filterDBError(std::move(Err), MDB_NOTFOUND);
    if (Err)
      return Err;
    // Get the last free extent in the database instead
    Err = Cursor.get(ExtentKey, ExtentData, MDB_LAST);
    if (Err) {
      Err = lmdb::filterDBError(std::move(Err), MDB_NOTFOUND);
      if (Err)
        return Err;
      AllocatorFull = true;
    }
  }

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
        Err = Cursor.del();
        if (Err)
          return Err;
      }
      // We don't need to check the next extent in this case, as we can only
      // reach there if there is no more extent greater than #ID (Recall that we
      // failed the first lookup)
    } else {
      // Check if we can merge the extent greater than #NewExtent
      if (AllocatorCheckConsecutive(NewExtent, Extent)) {
        NewExtent.Length += Extent.Length;
        Err = Cursor.del();
        if (Err)
          return Err;
      }

      // Check if merging with extents preceding #NewExtent is possible
      Err = Cursor.get(ExtentKey, ExtentData, MDB_PREV);
      if (Err) {
        Err = lmdb::filterDBError(std::move(Err), MDB_NOTFOUND);
        if (Err)
          return Err;
      } else {
        Extent.ID = *ExtentKey.data<RecordID>();
        Extent.Length = *ExtentData.data<RecordID>();
        // Sanity check - the range to be freed must not be in database
        assert(!AllocatorCheckExtentOverlap(Extent, NewExtent));
        // Check if we can merge the extent less than #NewExtent
        if (AllocatorCheckConsecutive(Extent, NewExtent)) {
          NewExtent.ID = Extent.ID;
          NewExtent.Length += Extent.Length;
          Err = Cursor.del();
          if (Err)
            return Err;
        }
      }
    }
  }
  // Insert the resulting new extent
  ExtentKey.assign(&NewExtent.ID, sizeof(RecordID));
  ExtentData.assign(&NewExtent.Length, sizeof(RecordID));
  return Cursor.put(ExtentKey, ExtentData, 0);
}

} // namespace dbindex
} // namespace clangd
} // namespace clang
