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

llvm::Expected<Env> Env::create() {
  MDB_env *Handle;
  int RC = mdb_env_create(&Handle);
  if (RC != MDB_SUCCESS)
    return llvm::Expected<Env>(llvm::make_error<DBError>("Env::create", RC));
  return llvm::Expected<Env>(Env(Handle));
}

llvm::Error Env::setMapsize(mdb_size_t Size) {
  int RC = mdb_env_set_mapsize(Handle, Size);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Env::setMapsize", RC);
  return llvm::Error::success();
}

llvm::Error Env::setMaxDBs(MDB_dbi N) {
  int RC = mdb_env_set_maxdbs(Handle, N);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Env::setMaxDBs", RC);
  return llvm::Error::success();
}

llvm::Error Env::open(llvm::StringRef Path, unsigned int Flags,
                      mdb_mode_t Mode) {
  int RC = mdb_env_open(Handle, Path.data(), Flags, Mode);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Env::open", RC);
  return llvm::Error::success();
}

Txn::~Txn() {
  if (Handle)
    mdb_txn_abort(Handle);
}

llvm::Expected<Txn> Txn::begin(MDB_env *Env, MDB_txn *ParentTxn,
                               unsigned int Flags) {
  MDB_txn *Handle;
  int RC = mdb_txn_begin(Env, ParentTxn, Flags, &Handle);
  if (RC != MDB_SUCCESS)
    return llvm::Expected<Txn>(llvm::make_error<DBError>("Txn::begin", RC));
  return llvm::Expected<Txn>(Txn(Handle));
}

llvm::Error Txn::commit() {
  int RC = mdb_txn_commit(Handle);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Txn::commit", RC);
  Handle = nullptr;
  return llvm::Error::success();
}

void Txn::abort() {
  mdb_txn_abort(Handle);
  Handle = nullptr;
}

DBI::~DBI() {
  if (Handle.first != nullptr)
    mdb_dbi_close(Handle.first, Handle.second);
}

llvm::Expected<DBI> DBI::open(MDB_txn *Txn, const char *Name,
                              unsigned int Flags) {
  MDB_dbi Handle;
  int RC = mdb_dbi_open(Txn, Name, Flags, &Handle);
  if (RC != MDB_SUCCESS)
    return llvm::Expected<DBI>(llvm::make_error<DBError>("DBI::open", RC));
  return llvm::Expected<DBI>(DBI(mdb_txn_env(Txn), Handle));
}

llvm::Error DBI::get(MDB_txn *Txn, const Val &Key, Val &Data) {
  MDB_val KV = Key;
  int RC = mdb_get(Txn, Handle.second, &KV, Data);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("DBI::get", RC);
  return llvm::Error::success();
}

llvm::Error DBI::put(MDB_txn *Txn, const Val &Key, const Val &Data,
                     unsigned int Flags) {
  MDB_val KV = Key, DV = Data;
  int RC = mdb_put(Txn, Handle.second, &KV, &DV, Flags & ~MDB_RESERVE);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("DBI::put", RC);
  return llvm::Error::success();
}

llvm::Error DBI::reserve(MDB_txn *Txn, const Val &Key, Val &Data,
                         unsigned int Flags) {
  MDB_val KV = Key;
  int RC = mdb_put(Txn, Handle.second, &KV, Data, Flags | MDB_RESERVE);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("DBI::put", RC);
  return llvm::Error::success();
}

llvm::Error DBI::del(MDB_txn *Txn, const Val &Key, llvm::Optional<Val> Data) {
  MDB_val KV = Key;
  int RC = mdb_del(Txn, Handle.second, &KV,
                   Data ? static_cast<MDB_val *>(*Data) : nullptr);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("DBI::del", RC);
  return llvm::Error::success();
}

llvm::Error DBI::stat(MDB_txn *Txn, MDB_stat &Stat) {
  int RC = mdb_stat(Txn, Handle.second, &Stat);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("DBI::stat", RC);
  return llvm::Error::success();
}

Cursor::~Cursor() {
  if (Handle)
    mdb_cursor_close(Handle);
}

llvm::Expected<Cursor> Cursor::open(MDB_txn *Txn, MDB_dbi DBI) {
  MDB_cursor *Handle;
  int RC = mdb_cursor_open(Txn, DBI, &Handle);
  if (RC != MDB_SUCCESS)
    return llvm::Expected<Cursor>(
        llvm::make_error<DBError>("Cursor::open", RC));
  return llvm::Expected<Cursor>(Cursor(Handle));
}

llvm::Error Cursor::set(const Val &Key) {
  MDB_val KV = Key;
  int RC = mdb_cursor_get(Handle, &KV, nullptr, MDB_SET);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::set", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::setKey(Val &Key) {
  int RC = mdb_cursor_get(Handle, Key, nullptr, MDB_SET_KEY);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::setKey", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::setRange(Val &Key) {
  int RC = mdb_cursor_get(Handle, Key, nullptr, MDB_SET_RANGE);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::setRange", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::get(Val &Key, Val &Data, MDB_cursor_op Op) {
  int RC = mdb_cursor_get(Handle, Key, Data, Op);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::get", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::put(const Val &Key, const Val &Data, unsigned int Flags) {
  MDB_val KV = Key, DV = Data;
  int RC = mdb_cursor_put(Handle, &KV, &DV, Flags & ~MDB_RESERVE);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::put", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::reserve(const Val &Key, Val &Data, unsigned int Flags) {
  MDB_val KV = Key;
  int RC = mdb_cursor_put(Handle, &KV, Data, Flags | MDB_RESERVE);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::reserve", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::del(unsigned int Flags) {
  int RC = mdb_cursor_del(Handle, Flags);
  if (RC != MDB_SUCCESS)
    return llvm::make_error<DBError>("Cursor::del", RC);
  return llvm::Error::success();
}

llvm::Error Cursor::foreachKey(
    const Val &Key,
    llvm::function_ref<IteratorControl(const Val &, const Val &)> Callback) {
  Val K(Key), D;
  auto Err = get(K, D, MDB_SET_KEY);
  if (Err)
    return filterDBError(std::move(Err), MDB_NOTFOUND);
  do {
    if (Callback(K, D) != Continue)
      break;
  } while (!(Err = get(K, D, MDB_NEXT_DUP)));
  return filterDBError(std::move(Err), MDB_NOTFOUND);
}

llvm::Error Cursor::foreachRanged(
    const Val &Prefix,
    llvm::function_ref<IteratorControl(const Val &, const Val &)> Callback) {
  Val K(Prefix), D;
  auto Err = get(K, D, MDB_SET_RANGE);
  if (Err)
    return filterDBError(std::move(Err), MDB_NOTFOUND);
  do {
    llvm::StringRef Ks = K;
    if (!Ks.startswith(Prefix))
      break;
    if (Callback(K, D) != Continue)
      break;
  } while (!(Err = get(K, D, MDB_NEXT)));
  return filterDBError(std::move(Err), MDB_NOTFOUND);
}

} // namespace lmdb