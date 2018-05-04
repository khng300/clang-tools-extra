#ifndef __MULTIPLE_STORE_H__
#define __MULTIPLE_STORE_H__

#include <map>

#include <leveldb/comparator.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

namespace multiplestore {

using DbID = uint64_t;
using Status = leveldb::Status;
using Options = leveldb::Options;
using ReadOptions = leveldb::ReadOptions;
using WriteOptions = leveldb::WriteOptions;
using Slice = leveldb::Slice;
using Comparator = leveldb::Comparator;

class Iterator;
class WriteBatch;

class MultiDB {
 public:
  static Status Open(const Options& options,
                     const std::map<DbID, Comparator*>* comparators,
                     const std::string& name,
                     MultiDB** dbptr);

  MultiDB();
  virtual ~MultiDB();

  // Zero-length key is not allowed in MultiDB.
  Status Put(const WriteOptions& options,
             DbID dbid,
             const Slice& key,
             const Slice& value);

  Status Delete(const WriteOptions& options, DbID dbid, const Slice& key);

  Status Write(const WriteOptions& options, WriteBatch* updates);

  Status Get(const ReadOptions& options,
             DbID dbid,
             const Slice& key,
             std::string* value);

  Iterator* NewIterator(const ReadOptions& options, DbID dbid);

 private:
  leveldb::DB* db;
  Comparator* internal_comparator;

  // No copying allowed
  MultiDB(const MultiDB&);
  void operator=(const MultiDB&);
};

class WriteBatch {
 public:
  // Store the mapping "key->value" in the database.
  // Zero-length key is not allowed in MultiDB.
  void Put(DbID dbid, const Slice& key, const Slice& value);

  // If the database contains a mapping for "key", erase it.  Else do nothing.
  void Delete(DbID dbid, const Slice& key);

  void Clear();

  // Support for iterating over the contents of a batch.
  class Handler {
   public:
    virtual ~Handler();
    virtual void Put(DbID dbid, const Slice& key, const Slice& value) = 0;
    virtual void Delete(DbID dbid, const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

 private:
  friend Status MultiDB::Write(const WriteOptions& options,
                               WriteBatch* updates);

  leveldb::WriteBatch base_write_batch;
};

class Iterator {
 public:
  Iterator();
  ~Iterator();

  bool Valid() const;

  void SeekToFirst();

  void SeekToLast();

  void Seek(const Slice& target);

  void Next();
  void Prev();
  Slice key();
  Slice value();
  Status status();

 private:
  friend Iterator* MultiDB::NewIterator(const ReadOptions& options, DbID dbid);

  DbID dbid;
  bool valid;
  leveldb::Iterator* base_iter;
};

}  // namespace multiplestore

#endif  // __MULTIPLE_STORE_H__