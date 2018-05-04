#include "multiple-store.h"

namespace multiplestore {

class RawKeyBuffer {
  struct Header {
    // DbID field
    uint8_t db_id[sizeof(DbID)];
  };
  std::string buffer;

 public:
  RawKeyBuffer() { buffer.resize(sizeof(Header)); }

  RawKeyBuffer(DbID dbid, const Slice& slice) : RawKeyBuffer() {
    SetDbID(dbid);
    SetData(slice);
  }

  RawKeyBuffer(const Slice& raw_key) { buffer = raw_key.ToString(); }

  DbID GetDbID() {
    const Header* key_header = reinterpret_cast<const Header*>(&buffer[0]);
    return (((DbID)(key_header->db_id[0]) << 56) |
            ((DbID)(key_header->db_id[1]) << 48) |
            ((DbID)(key_header->db_id[2]) << 40) |
            ((DbID)(key_header->db_id[3]) << 32) |
            ((DbID)(key_header->db_id[4]) << 24) |
            ((DbID)(key_header->db_id[5]) << 16) |
            ((DbID)(key_header->db_id[6]) << 8) | key_header->db_id[7]);
  }

  void SetDbID(DbID dbid) {
    Header* key_header = reinterpret_cast<Header*>(&buffer[0]);
    key_header->db_id[0] = dbid >> 56;
    key_header->db_id[1] = dbid >> 48;
    key_header->db_id[2] = dbid >> 40;
    key_header->db_id[3] = dbid >> 32;
    key_header->db_id[4] = dbid >> 24;
    key_header->db_id[5] = dbid >> 16;
    key_header->db_id[6] = dbid >> 8;
    key_header->db_id[7] = dbid;
  }

  Slice GetRawKey() { return Slice(buffer.data(), buffer.size()); }
  size_t GetSize() { return buffer.size() - sizeof(Header); }

  Slice GetData() {
    if (!GetSize())
      return Slice();
    return Slice(buffer.data() + sizeof(Header), GetSize());
  }

  void SetData(const std::string& data) {
    buffer.resize(sizeof(Header));
    buffer.append(data);
  }

  void SetData(const Slice& slice) {
    buffer.resize(sizeof(Header) + slice.size());
    if (!slice.empty())
      memcpy(&buffer[sizeof(Header)], slice.data(), slice.size());
  }

  void SetData(const void* data, size_t size) {
    buffer.resize(sizeof(Header) + size);
    memcpy(&buffer[sizeof(Header)], data, size);
  }
};

class InternalHandler : public leveldb::WriteBatch::Handler {
 public:
  InternalHandler(WriteBatch::Handler* handler) : handler(handler) {}
  ~InternalHandler() {}
  void Put(const Slice& key, const Slice& value) {
    RawKeyBuffer raw_key(key);
    handler->Put(raw_key.GetDbID(), raw_key.GetData(), value);
  }
  void Delete(const Slice& key) {
    RawKeyBuffer raw_key(key);
    handler->Delete(raw_key.GetDbID(), raw_key.GetData());
  }

 private:
  WriteBatch::Handler* handler;
};

Iterator::Iterator() : base_iter(NULL) {}

Iterator::~Iterator() {
  if (base_iter)
    delete base_iter;
}

bool Iterator::Valid() const {
  return valid;
}

void Iterator::SeekToFirst() {
  RawKeyBuffer raw_key(dbid, {});
  valid = false;

  base_iter->Seek(raw_key.GetRawKey());
  if (!base_iter->Valid())
    return;

  raw_key = RawKeyBuffer(base_iter->key());
  if (raw_key.GetDbID() != dbid)
    return;
  valid = true;
}

void Iterator::SeekToLast() {
  RawKeyBuffer raw_key(dbid + 1, {});
  valid = false;

  base_iter->Seek(raw_key.GetRawKey());
  if (base_iter->Valid()) {
    base_iter->Prev();
    if (!base_iter->Valid())
      return;
  } else
    return;

  raw_key = RawKeyBuffer(base_iter->key());
  if (raw_key.GetDbID() != dbid)
    return;
  valid = true;
}

void Iterator::Seek(const Slice& target) {
  RawKeyBuffer raw_key(dbid, target);
  valid = false;

  base_iter->Seek(raw_key.GetRawKey());
  if (!base_iter->Valid())
    return;

  raw_key = RawKeyBuffer(base_iter->key());
  if (raw_key.GetDbID() != dbid)
    return;
  valid = true;
}

void Iterator::Next() {
  valid = false;

  base_iter->Next();
  if (!base_iter->Valid())
    return;

  RawKeyBuffer raw_key(base_iter->key());
  if (raw_key.GetDbID() != dbid)
    return;
  valid = true;
}

void Iterator::Prev() {
  valid = false;

  base_iter->Prev();
  if (!base_iter->Valid())
    return;

  RawKeyBuffer raw_key(base_iter->key());
  if (raw_key.GetDbID() != dbid)
    return;
  valid = true;
}

Slice Iterator::key() {
  assert(valid);
  RawKeyBuffer raw_key(base_iter->key());
  return Slice(raw_key.GetData());
}

Slice Iterator::value() {
  assert(valid);
  return base_iter->value();
}

Status Iterator::status() {
  return base_iter->status();
}

class InternalComparator : public leveldb::Comparator {
 public:
  InternalComparator(const std::map<DbID, Comparator*>* comparators_map) {
    for (const auto& pair : *comparators_map)
      this->comparators_map.insert(pair);
  }
  ~InternalComparator() {}

  int Compare(const Slice& a, const Slice& b) const {
    RawKeyBuffer raw_key_a(a), raw_key_b(b);
    if (raw_key_a.GetDbID() < raw_key_b.GetDbID())
      return -1;
    if (raw_key_a.GetDbID() > raw_key_b.GetDbID())
      return 1;
    if (!raw_key_a.GetSize())
      return -1;
    if (!raw_key_b.GetSize())
      return 1;
    auto comparator = comparators_map.find(raw_key_a.GetDbID());
    if (comparator == comparators_map.end())
      return raw_key_a.GetData().compare(raw_key_b.GetData());
    return comparator->second->Compare(raw_key_a.GetData(),
                                       raw_key_b.GetData());
  };

  const char* Name() const { return "MultiDB"; };

  void FindShortestSeparator(std::string* start, const Slice& limit) const {};

  void FindShortSuccessor(std::string* key) const {};

 private:
  std::map<DbID, Comparator*> comparators_map;
};

void WriteBatch::Put(DbID dbid, const Slice& key, const Slice& value) {
  RawKeyBuffer raw_key(dbid, key);
  base_write_batch.Put(raw_key.GetRawKey(), value);
}

void WriteBatch::Delete(DbID dbid, const Slice& key) {
  RawKeyBuffer raw_key(dbid, key);
  base_write_batch.Delete(raw_key.GetRawKey());
}

Status WriteBatch::Iterate(Handler* handler) const {
  InternalHandler internal_handler(handler);
  return base_write_batch.Iterate(&internal_handler);
}

void WriteBatch::Clear() {
  base_write_batch.Clear();
}

MultiDB::MultiDB() : db(NULL), internal_comparator(NULL){};

Status MultiDB::Open(const Options& options,
                     const std::map<DbID, Comparator*>* comparators,
                     const std::string& name,
                     MultiDB** dbptr) {
  Options internal_options = options;
  MultiDB* multi_db = new MultiDB();
  if (comparators) {
    multi_db->internal_comparator = new InternalComparator(comparators);
    internal_options.comparator = multi_db->internal_comparator;
  }

  Status s = leveldb::DB::Open(internal_options, name, &multi_db->db);
  if (!s.ok()) {
    delete multi_db;
    *dbptr = NULL;
    return s;
  }

  *dbptr = multi_db;
  return Status::OK();
}

MultiDB::~MultiDB() {
  if (internal_comparator)
    delete internal_comparator;
  if (db)
    delete db;
}

Status MultiDB::Put(const WriteOptions& options,
                    DbID dbid,
                    const Slice& key,
                    const Slice& value) {
  WriteBatch write_batch;
  write_batch.Put(dbid, key, value);
  return Write(options, &write_batch);
}

Status MultiDB::Delete(const WriteOptions& options,
                       DbID dbid,
                       const Slice& key) {
  WriteBatch write_batch;
  write_batch.Delete(dbid, key);
  return Write(options, &write_batch);
}

Status MultiDB::Write(const WriteOptions& options, WriteBatch* updates) {
  return db->Write(options, &updates->base_write_batch);
}

Status MultiDB::Get(const ReadOptions& options,
                    DbID dbid,
                    const Slice& key,
                    std::string* value) {
  RawKeyBuffer raw_key(dbid, key);
  return db->Get(options, raw_key.GetRawKey(), value);
}

Iterator* MultiDB::NewIterator(const ReadOptions& options, DbID dbid) {
  Iterator* iter = new Iterator();
  iter->dbid = dbid;
  iter->base_iter = db->NewIterator(options);
  iter->valid = false;
  if (!iter->base_iter) {
    delete iter;
    return NULL;
  }
  return iter;
}

}  // namespace multiplestore