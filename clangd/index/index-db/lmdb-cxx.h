//===-------------- lmdb-cxx.h - C++ wrapper to LMDB ------------- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_LMDB_CXX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_LMDB_CXX_H

#include "lmdb/libraries/liblmdb/lmdb.h"
#include "llvm/Support/Error.h"

namespace lmdb {

const char *version(int *Major, int *Minor, int *Patch);

/// This class wraps an LMDB error
class DBError final : public llvm::ErrorInfo<DBError> {
public:
  // Used by ErrorInfo::classID.
  static char ID;

  // Construct an error with origin and return code
  DBError(const char *const Origin, int RC) : Origin(Origin), RC(RC) {}

  // Log the error to output stream
  void log(llvm::raw_ostream &OS) const override {
    OS << "'" << Origin << "': " << mdb_strerror(RC) << '\n';
  }

  // Convert the error to std::error_code
  std::error_code convertToErrorCode() const override;

  // Get message related to the error
  const char *strerror() const;

  // Get RC code
  int returnCode() const { return RC; }

private:
  const char *Origin;
  int RC;
};

class error_category : public std::error_category {
  virtual const char *name() const noexcept { return "LMDB error"; }

  std::string message(int errcode) const { return mdb_strerror(errcode); }
};

extern error_category ErrorCategory;

inline std::error_code makeErrorCode(int ErrCode) {
  return std::error_code(ErrCode, ErrorCategory);
}

inline std::error_code successErrorCode() { return makeErrorCode(MDB_SUCCESS); }

template <typename T, typename B> struct HandleWrapper {
  B getHandle() { return static_cast<T>(*this); }
};

/// This class wraps an MDB_env
class Env : public HandleWrapper<Env, MDB_env *> {
  // Default unix mode specified when opening the database
  static constexpr mdb_mode_t DefaultMode = 0666;

public:
  // Constructor
  Env() = default;

  // Constructor
  Env(Env &&E) { *this = std::move(E); }

  // Destructor
  virtual ~Env();

  // Expose underlying handle
  operator MDB_env *() const { return Handle; }

  // Move assignment
  Env &operator=(Env &&From) {
    if (&From == this)
      return *this;
    Env::~Env();
    Handle = From.Handle;
    From.Handle = nullptr;
    return *this;
  }

  // Create an MDB_env
  static llvm::ErrorOr<Env> create();

  // Set the size of the memory map to use for this environment.
  std::error_code setMapsize(mdb_size_t Size);

  // Set maximum number of named databases for this environment.
  std::error_code setMaxDBs(MDB_dbi N);

  // Open the environment
  std::error_code open(llvm::StringRef Path, unsigned int Flags,
                       mdb_mode_t Mode = DefaultMode);

private:
  // Constructor
  Env(MDB_env *P) { Handle = P; }

  // Handle to the environment
  MDB_env *Handle = nullptr;
};

/// This class wraps an MDB_txn
class Txn : public HandleWrapper<Txn, MDB_txn *> {
public:
  // Constructor
  Txn() = default;

  // Constructor
  Txn(Txn &&T) { *this = std::move(T); }

  // Destructor
  ~Txn();

  // Expose underlying handle
  operator MDB_txn *() const { return Handle; }

  // Move assignment
  Txn &operator=(Txn &&From) {
    if (&From == this)
      return *this;
    Txn::~Txn();
    Handle = From.Handle;
    From.Handle = nullptr;
    return *this;
  }

  // Begin an LMDB transaction
  static llvm::ErrorOr<Txn> begin(MDB_env *Env, MDB_txn *ParentTxn = nullptr,
                                  unsigned int Flags = 0);

  // Commit
  std::error_code commit();

  // Abort
  void abort();

private:
  // Constructor
  Txn(MDB_txn *P) : Handle(P) {}

  // Handle to the transaction
  MDB_txn *Handle = nullptr;
};

/// This class wraps an MDB_val
class Val {
public:
  // Constructor
  Val() {
    Value.mv_data = nullptr;
    Value.mv_size = 0;
  }

  // Constructor
  Val(const Val &V) : Value(V.Value) {}

  // Constructor
  Val(Val &&) = default;

  // Constructor
  Val(const MDB_val &V) : Value(V) {}

  // Constructor
  Val(const std::string &S) {
    Value.mv_data = const_cast<char *>(S.data());
    Value.mv_size = S.size();
  }

  // Constructor
  Val(llvm::StringRef S) {
    Value.mv_data = const_cast<char *>(S.data());
    Value.mv_size = S.size();
  }

  // Constructor
  Val(llvm::ArrayRef<uint8_t> A) {
    Value.mv_data = const_cast<uint8_t *>(A.data());
    Value.mv_size = A.size();
  }

  // Constructor
  Val(const void *P, size_t S) { assign(P, S); }

  // Constructor
  template <typename T> Val(const T *S) {
    Value.mv_data = const_cast<T *>(S);
    Value.mv_size = sizeof(T);
  }

  // Move assignment
  Val &operator=(Val &&) = default;

  // Assign
  void assign(const void *P, size_t S) {
    Value.mv_data = const_cast<void *>(P);
    Value.mv_size = S;
  }

  // To MDB_val
  operator MDB_val() const { return Value; }

  // To MDB_val *
  operator MDB_val *() { return &Value; }

  // To llvm::StringRef
  operator llvm::StringRef() const {
    return llvm::StringRef(reinterpret_cast<char *>(Value.mv_data),
                           Value.mv_size);
  }

  // To llvm::ArrayRef<uint8_t>
  operator llvm::ArrayRef<uint8_t>() const {
    return llvm::ArrayRef<uint8_t>(reinterpret_cast<uint8_t *>(Value.mv_data),
                                   Value.mv_size);
  }

  // To llvm::MutableArrayRef<uint8_t>
  operator llvm::MutableArrayRef<uint8_t>() {
    return llvm::MutableArrayRef<uint8_t>(
        reinterpret_cast<uint8_t *>(Value.mv_data), Value.mv_size);
  }

  // Get data pointer
  char *data() { return reinterpret_cast<char *>(Value.mv_data); }

  // Get data pointer
  const char *data() const { return reinterpret_cast<char *>(Value.mv_data); }

  // Get data pointer
  template <typename T> T *data() {
    return reinterpret_cast<T *>(Value.mv_data);
  }

  // Get data pointer
  template <typename T> const T *data() const {
    return reinterpret_cast<T *>(Value.mv_data);
  }

  // Get size
  size_t size() const { return Value.mv_size; }

private:
  MDB_val Value;
};

/// This class wraps an MDB_dbi
class DBI : public HandleWrapper<DBI, std::pair<MDB_env *, MDB_dbi>> {
  static constexpr MDB_dbi InvalidDBI = -1u;

public:
  // Constructor
  DBI() : Handle(nullptr, InvalidDBI){};

  // Constructor
  DBI(DBI &&T) : DBI() { *this = std::move(T); }

  // Destructor
  ~DBI();

  // Expose underlying handle
  operator std::pair<MDB_env *, MDB_dbi>() const { return Handle; }

  // Expose underlying environment handle
  operator MDB_env *() const { return Handle.first; }

  // Expose underlying dbi handle
  operator MDB_dbi() const { return Handle.second; }

  // Move assignment
  DBI &operator=(DBI &&From) {
    if (&From == this)
      return *this;
    DBI::~DBI();
    Handle = From.Handle;
    From.Handle = std::pair<MDB_env *, MDB_dbi>();
    return *this;
  }

  // Open the database
  static llvm::ErrorOr<DBI> open(MDB_txn *Txn, const char *Name,
                                 unsigned int Flags);

  // Get key-value pair from database
  std::error_code get(MDB_txn *Txn, const Val &Key, Val &Data);

  // Put key-value pair to database
  std::error_code put(MDB_txn *Txn, const Val &Key, const Val &Data,
                      unsigned int Flags);

  // Reserve key-value pair to database. Address to a mutable buffer will be
  // return at \p Data
  std::error_code reserve(MDB_txn *Txn, const Val &Key, Val &Data,
                          unsigned int Flags);

  // Delete key-value pair to database
  std::error_code del(MDB_txn *Txn, const Val &Key, llvm::Optional<Val> Data);

  // Get statistics of database
  std::error_code stat(MDB_txn *Txn, MDB_stat &Stat);

private:
  // Constructor
  DBI(MDB_env *E, MDB_dbi I) : Handle(E, I){};

  // Handle to the database
  std::pair<MDB_env *, MDB_dbi> Handle;
};

/// This enumerates the control value for iterator
enum IteratorControl { Continue = 0, Stop = 1 };

/// This class wraps an MDB_cursor
class Cursor : public HandleWrapper<Cursor, MDB_cursor *> {
public:
  // Constructor
  Cursor() = default;

  // Constructor
  Cursor(Cursor &&C) : Handle(nullptr) { *this = std::move(C); }

  // Destructor
  ~Cursor();

  // Expose underlying handle
  operator MDB_cursor *() const { return Handle; }

  // Move assignment
  Cursor &operator=(Cursor &&From) {
    if (&From == this)
      return *this;
    Cursor::~Cursor();
    Handle = From.Handle;
    From.Handle = nullptr;
    return *this;
  }

  // Open a cursor
  static llvm::ErrorOr<Cursor> open(MDB_txn *Txn, MDB_dbi DBI);

  // Set the cursor to certain position
  std::error_code set(const Val &Key);

  // Set the cursor to certain position and get the pointer to
  // the key
  std::error_code setKey(Val &Key);

  // Set the cursor to certain position with key greater than
  // or equal to specified key
  std::error_code setRange(Val &Key);

  // Get pointer to data at certain position
  std::error_code get(Val &Key, Val &Data, MDB_cursor_op Op);

  // Put data
  std::error_code put(const Val &Key, const Val &Data, unsigned int Flags);

  // Put key and reserve space for users to put data later
  std::error_code reserve(const Val &Key, Val &Data, unsigned int Flags);

  // Delete data pointed by the cursor
  std::error_code del(unsigned int Flags = 0);

  // Ranged foreach
  std::error_code foreachKey(
      const Val &Key,
      llvm::function_ref<IteratorControl(const Val &, const Val &)> Callback);

  // Ranged foreach
  std::error_code foreachRanged(
      const Val &Prefix,
      llvm::function_ref<IteratorControl(const Val &, const Val &)> Callback);

private:
  // Constructor
  Cursor(MDB_cursor *C) : Handle(C){};

  // Handle to the cursor
  MDB_cursor *Handle = nullptr;
};

} // namespace lmdb

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_DATABASESTORAGE_LMDB_CXX_H