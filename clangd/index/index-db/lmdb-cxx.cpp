//===------------- lmdb-cxx.cpp - C++ wrapper to LMDB ------------ C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------===//

#include "lmdb-cxx.h"

namespace lmdb {

char DBError::ID = 0;
constexpr MDB_dbi DBI::InvalidDBI;
error_category ErrorCategory;

const char *version(int *Major, int *Minor, int *Patch) {
  return mdb_version(Major, Minor, Patch);
}

std::error_code DBError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

const char *DBError::strerror() const { return mdb_strerror(RC); }

Env::~Env() {
  if (Handle)
    mdb_env_close(Handle);
}

llvm::ErrorOr<Env> Env::create() {
  MDB_env *Handle;
  int RC = mdb_env_create(&Handle);
  if (RC != MDB_SUCCESS)
    return makeErrorCode(RC);
  return Env(Handle);
}

std::error_code Env::setMapsize(mdb_size_t Size) {
  int RC = mdb_env_set_mapsize(Handle, Size);
  return makeErrorCode(RC);
}

std::error_code Env::setMaxDBs(MDB_dbi N) {
  int RC = mdb_env_set_maxdbs(Handle, N);
  return makeErrorCode(RC);
}

std::error_code Env::open(llvm::StringRef Path, unsigned int Flags,
                          mdb_mode_t Mode) {
  int RC = mdb_env_open(Handle, Path.data(), Flags, Mode);
  return makeErrorCode(RC);
}

Txn::~Txn() {
  if (Handle)
    mdb_txn_abort(Handle);
}

llvm::ErrorOr<Txn> Txn::begin(MDB_env *Env, MDB_txn *ParentTxn,
                              unsigned int Flags) {
  MDB_txn *Handle;
  int RC = mdb_txn_begin(Env, ParentTxn, Flags, &Handle);
  if (RC != MDB_SUCCESS)
    return makeErrorCode(RC);
  return Txn(Handle);
}

std::error_code Txn::commit() {
  int RC = mdb_txn_commit(Handle);
  Handle = nullptr;
  return makeErrorCode(RC);
}

void Txn::abort() {
  mdb_txn_abort(Handle);
  Handle = nullptr;
}

DBI::~DBI() {
  if (Handle.first != nullptr)
    mdb_dbi_close(Handle.first, Handle.second);
}

llvm::ErrorOr<DBI> DBI::open(MDB_txn *Txn, const char *Name,
                             unsigned int Flags) {
  MDB_dbi Handle;
  int RC = mdb_dbi_open(Txn, Name, Flags, &Handle);
  if (RC != MDB_SUCCESS)
    return makeErrorCode(RC);
  return DBI(mdb_txn_env(Txn), Handle);
}

std::error_code DBI::get(MDB_txn *Txn, const Val &Key, Val &Data) {
  MDB_val KV = Key;
  int RC = mdb_get(Txn, Handle.second, &KV, Data);
  return makeErrorCode(RC);
}

std::error_code DBI::put(MDB_txn *Txn, const Val &Key, const Val &Data,
                         unsigned int Flags) {
  MDB_val KV = Key, DV = Data;
  int RC = mdb_put(Txn, Handle.second, &KV, &DV, Flags & ~MDB_RESERVE);
  return makeErrorCode(RC);
}

std::error_code DBI::reserve(MDB_txn *Txn, const Val &Key, Val &Data,
                             unsigned int Flags) {
  MDB_val KV = Key;
  int RC = mdb_put(Txn, Handle.second, &KV, Data, Flags | MDB_RESERVE);
  return makeErrorCode(RC);
}

std::error_code DBI::del(MDB_txn *Txn, const Val &Key,
                         llvm::Optional<Val> Data) {
  MDB_val KV = Key;
  int RC = mdb_del(Txn, Handle.second, &KV,
                   Data ? static_cast<MDB_val *>(*Data) : nullptr);
  return makeErrorCode(RC);
}

std::error_code DBI::stat(MDB_txn *Txn, MDB_stat &Stat) {
  int RC = mdb_stat(Txn, Handle.second, &Stat);
  return makeErrorCode(RC);
}

Cursor::~Cursor() {
  if (Handle)
    mdb_cursor_close(Handle);
}

llvm::ErrorOr<Cursor> Cursor::open(MDB_txn *Txn, MDB_dbi DBI) {
  MDB_cursor *Handle;
  int RC = mdb_cursor_open(Txn, DBI, &Handle);
  if (RC != MDB_SUCCESS)
    return makeErrorCode(RC);
  return Cursor(Handle);
}

std::error_code Cursor::set(const Val &Key) {
  MDB_val KV = Key;
  int RC = mdb_cursor_get(Handle, &KV, nullptr, MDB_SET);
  return makeErrorCode(RC);
}

std::error_code Cursor::setKey(Val &Key) {
  int RC = mdb_cursor_get(Handle, Key, nullptr, MDB_SET_KEY);
  return makeErrorCode(RC);
}

std::error_code Cursor::setRange(Val &Key) {
  int RC = mdb_cursor_get(Handle, Key, nullptr, MDB_SET_RANGE);
  return makeErrorCode(RC);
}

std::error_code Cursor::get(Val &Key, Val &Data, MDB_cursor_op Op) {
  int RC = mdb_cursor_get(Handle, Key, Data, Op);
  return makeErrorCode(RC);
}

std::error_code Cursor::put(const Val &Key, const Val &Data,
                            unsigned int Flags) {
  MDB_val KV = Key, DV = Data;
  int RC = mdb_cursor_put(Handle, &KV, &DV, Flags & ~MDB_RESERVE);
  return makeErrorCode(RC);
}

std::error_code Cursor::reserve(const Val &Key, Val &Data, unsigned int Flags) {
  MDB_val KV = Key;
  int RC = mdb_cursor_put(Handle, &KV, Data, Flags | MDB_RESERVE);
  return makeErrorCode(RC);
}

std::error_code Cursor::del(unsigned int Flags) {
  int RC = mdb_cursor_del(Handle, Flags);
  return makeErrorCode(RC);
}

std::error_code Cursor::foreachKey(
    const Val &Key,
    llvm::function_ref<IteratorControl(const Val &, const Val &)> Callback) {
  Val K(Key), D;
  auto Err = get(K, D, MDB_SET_KEY);
  if (Err == lmdb::makeErrorCode(MDB_NOTFOUND))
    return successErrorCode();
  else if (Err)
    return Err;
  do {
    if (Callback(K, D) != Continue)
      break;
  } while (!(Err = get(K, D, MDB_NEXT_DUP)));
  if (Err && Err.value() != MDB_NOTFOUND)
    return Err;
  return successErrorCode();
}

std::error_code Cursor::foreachRanged(
    const Val &Prefix,
    llvm::function_ref<IteratorControl(const Val &, const Val &)> Callback) {
  Val K(Prefix), D;
  auto Err = get(K, D, MDB_SET_RANGE);
  if (Err == lmdb::makeErrorCode(MDB_NOTFOUND))
    return successErrorCode();
  else if (Err)
    return Err;
  do {
    llvm::StringRef Ks = K;
    if (!Ks.startswith(Prefix))
      break;
    if (Callback(K, D) != Continue)
      break;
  } while (!(Err = get(K, D, MDB_NEXT)));
  if (Err && Err.value() != MDB_NOTFOUND)
    return Err;
  return successErrorCode();
}

} // namespace lmdb