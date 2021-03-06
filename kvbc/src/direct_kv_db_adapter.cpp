// Copyright 2018 VMware, all rights reserved

// @file direct_kv_db_adapter.cpp
//
// Data is stored in the form of key value pairs. However, the key used is a
// composite database key. Its composition is : Key Type | Key | Block Id
//
// Block Id is functionally equivalent to the version of the block.
//
// For the purposes of iteration, the keys appear in the following order:
//    Ascending order of Key Type
//    -> Ascending order of Key
//       -> Descending order of Block Id

#include "Logger.hpp"
#include "sliver.hpp"
#include "status.hpp"
#include "kv_types.hpp"
#include "block.h"
#include "db_interfaces.h"
#include "direct_kv_db_adapter.h"
#include "bcstatetransfer/SimpleBCStateTransfer.hpp"
#include "hex_tools.h"

#include <string.h>

#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <utility>

using concordlogger::Logger;
using concordUtils::Status;
using concordUtils::Sliver;
using concord::storage::ObjectId;
using concordUtils::HexPrintBuffer;

namespace concord::kvbc {
inline namespace v1DirectKeyValue {
/**
 * @brief Helper function that generates a composite database Key of type Block.
 *
 * For such Database keys, the "key" component is an empty sliver.
 *
 * @param _blockid BlockId object of the block id that needs to be
 *                 incorporated into the composite database key.
 * @return Sliver object of the generated composite database key.
 */
Sliver KeyGenerator::blockKey(const BlockId &blockId) const {
  return genDbKey(EDBKeyType::E_DB_KEY_TYPE_BLOCK, Sliver(), blockId);
}

/**
 * @brief Helper function that generates a composite database Key of type Key.
 *
 * @param _key Sliver object of the "key" component.
 * @param _blockid BlockId object of the block id that needs to be incorporated
 *                 into the composite database Key.
 * @return Sliver object of the generated composite database key.
 */
Sliver KeyGenerator::dataKey(const Key &key, const BlockId &blockId) const {
  return genDbKey(EDBKeyType::E_DB_KEY_TYPE_KEY, key, blockId);
}

/*
 * If key a is smaller than key b, return a negative number; if larger, return a
 * positive number; if equal, return zero.
 *
 * Comparison is done by decomposed parts. Types are compared first, followed by
 * type specific comparison. */
int DBKeyComparator::composedKeyComparison(const char *_a_data,
                                           size_t _a_length,
                                           const char *_b_data,
                                           size_t _b_length) {
  EDBKeyType aType = DBKeyManipulator::extractTypeFromKey(_a_data);
  EDBKeyType bType = DBKeyManipulator::extractTypeFromKey(_b_data);
  if (aType != bType) return (int)aType - (int)bType;

  switch (aType) {
    case EDBKeyType::E_DB_KEY_TYPE_BFT_ST_KEY:
    case EDBKeyType::E_DB_KEY_TYPE_BFT_ST_PENDING_PAGE_KEY:
    case EDBKeyType::E_DB_KEY_TYPE_BFT_METADATA_KEY: {
      // Compare object IDs.
      ObjectId aObjId = DBKeyManipulator::extractObjectIdFromKey(_a_data, _a_length);
      ObjectId bObjId = DBKeyManipulator::extractObjectIdFromKey(_b_data, _b_length);
      return (aObjId > bObjId) ? 1 : (bObjId > aObjId) ? -1 : 0;
    }
    case EDBKeyType::E_DB_KEY_TYPE_BFT_ST_CHECKPOINT_DESCRIPTOR_KEY: {
      uint64_t aChkpt, bChkpt;
      aChkpt = DBKeyManipulator::extractCheckPointFromKey(_a_data, _a_length);
      bChkpt = DBKeyManipulator::extractCheckPointFromKey(_b_data, _b_length);
      return (aChkpt > bChkpt) ? 1 : (bChkpt > aChkpt) ? -1 : 0;
    }
    case EDBKeyType::E_DB_KEY_TYPE_BFT_ST_RESERVED_PAGE_STATIC_KEY:
    case EDBKeyType::E_DB_KEY_TYPE_BFT_ST_RESERVED_PAGE_DYNAMIC_KEY: {
      // Pages are sorted in ascending order, checkpoints in descending order
      uint32_t aPageId, bPageId;
      uint64_t aChkpt, bChkpt;
      std::tie(aPageId, aChkpt) = DBKeyManipulator::extractPageIdAndCheckpointFromKey(_a_data, _a_length);
      std::tie(bPageId, bChkpt) = DBKeyManipulator::extractPageIdAndCheckpointFromKey(_b_data, _b_length);
      if (aPageId != bPageId) return (aPageId > bPageId) ? 1 : (bPageId > aPageId) ? -1 : 0;
      return (aChkpt < bChkpt) ? 1 : (aChkpt > bChkpt) ? -1 : 0;
    }
    case EDBKeyType::E_DB_KEY_TYPE_KEY: {
      int keyComp = DBKeyManipulator::compareKeyPartOfComposedKey(_a_data, _a_length, _b_data, _b_length);
      if (keyComp != 0) return keyComp;
      // Extract the block ids to compare so that endianness of environment does not matter.
      BlockId aId = DBKeyManipulator::extractBlockIdFromKey(_a_data, _a_length);
      BlockId bId = DBKeyManipulator::extractBlockIdFromKey(_b_data, _b_length);
      // Block ids are sorted in reverse order when part of a composed key (key + blockId)
      return (bId > aId) ? 1 : (aId > bId) ? -1 : 0;
    }
    case EDBKeyType::E_DB_KEY_TYPE_BLOCK: {
      // Extract the block ids to compare so that endianness of environment does not matter.
      BlockId aId = DBKeyManipulator::extractBlockIdFromKey(_a_data, _a_length);
      BlockId bId = DBKeyManipulator::extractBlockIdFromKey(_b_data, _b_length);
      // Block ids are sorted in ascending order when part of a block key
      return (aId > bId) ? 1 : (bId > aId) ? -1 : 0;
    }
    default:
      LOG_ERROR(logger(), "invalid key type: " << (char)aType);
      assert(false);
      throw std::runtime_error("DBKeyComparator::composedKeyComparison: invalid key type");
  }  // switch
}

/**
 * @brief Generates a Composite Database Key from a Sliver object.
 *
 * Merges the key type, data of the key and the block id to generate a composite
 * database key.
 *
 * Format : Key Type | Key | Block Id

 * @param _type Composite Database key type (Either of First, Key, Block or
 *              Last).
 * @param _key Sliver object of the key.
 * @param _blockId BlockId object.
 * @return Sliver object of the generated composite database key.
 */
Sliver DBKeyManipulatorBase::genDbKey(EDBKeyType _type, const Key &_key, BlockId _blockId) {
  size_t sz = sizeof(EDBKeyType) + sizeof(BlockId) + _key.length();
  char *out = new char[sz];
  size_t offset = 0;
  copyToAndAdvance(out, &offset, sz, (char *)&_type, sizeof(EDBKeyType));
  copyToAndAdvance(out, &offset, sz, (char *)_key.data(), _key.length());
  copyToAndAdvance(out, &offset, sz, (char *)&_blockId, sizeof(BlockId));
  return Sliver(out, sz);
}

/**
 * @brief Extracts the Block Id of a composite database key.
 *
 * Returns the block id part of the Sliver object passed as a parameter.
 *
 * @param _key The Sliver object of the composite database key whose block id
 *             gets returned.
 * @return The block id of the composite database key.
 */
BlockId DBKeyManipulator::extractBlockIdFromKey(const Key &_key) {
  return extractBlockIdFromKey(_key.data(), _key.length());
}

/**
 * @brief Extracts the Block Id of a composite database key.
 *
 * Returns the block id part of the buffer passed as a parameter.
 *
 * @param _key_data The buffer containing the composite database key whose block id
 *                  gets returned.
 * @param _key_length The number of bytes in the _key_data buffer.
 * @return The block id of the composite database key.
 */
BlockId DBKeyManipulator::extractBlockIdFromKey(const char *_key_data, size_t _key_length) {
  const auto offset = _key_length - sizeof(BlockId);
  const auto id = *reinterpret_cast<const BlockId *>(_key_data + offset);

  LOG_TRACE(logger(),
            "Got block ID " << id << " from key " << (HexPrintBuffer{_key_data, _key_length}) << ", offset " << offset);
  return id;
}

/**
 * @brief Extracts the type of a composite database key.
 *
 * Returns the data part of the Sliver object passed as a parameter.
 *
 * @param _key The Sliver object of the composite database key whose type gets
 *             returned.
 * @return The type of the composite database key.
 */
EDBKeyType DBKeyManipulator::extractTypeFromKey(const Key &_key) { return extractTypeFromKey(_key.data()); }

/**
 * @brief Extracts the type of a composite database key.
 *
 * Returns the data part of the buffer passed as a parameter.
 *
 * @param _key_data The buffer containing the composite database key whose
 *                  type gets returned.
 * @return The type of the composite database key.
 */
EDBKeyType DBKeyManipulator::extractTypeFromKey(const char *_key_data) {
  static_assert(sizeof(EDBKeyType) == 1, "Let's avoid byte-order problems.");
  assert((_key_data[0] < (char)EDBKeyType::E_DB_KEY_TYPE_LAST) &&
         (_key_data[0] >= (char)EDBKeyType::E_DB_KEY_TYPE_FIRST));
  return (EDBKeyType)_key_data[0];
}

/**
 * @brief Extracts the Metadata Object Id of a composite database key.
 *
 * Returns the object id part of the Sliver object passed as a parameter.
 *
 * @param _key The Sliver object of the composite database key whose object id
 *             gets returned.
 * @return The object id of the composite database key.
 */
ObjectId DBKeyManipulator::extractObjectIdFromKey(const Key &_key) {
  return extractObjectIdFromKey(_key.data(), _key.length());
}

/**
 * @brief Extracts the Metadata Object Id of a composite database key.
 *
 * Returns the object id part of the buffer passed as a parameter.
 *
 * @param _key_data The buffer containing the composite database key whose object id
 *                  gets returned.
 * @param _key_length The number of bytes in the _key_data buffer.
 * @return The object id of the composite database key.
 */
ObjectId DBKeyManipulator::extractObjectIdFromKey(const char *_key_data, size_t _key_length) {
  assert(_key_length >= sizeof(ObjectId));
  size_t offset = _key_length - sizeof(ObjectId);
  ObjectId id = *(ObjectId *)(_key_data + offset);

  LOG_TRACE(
      logger(),
      "Got object ID " << id << " from key " << (HexPrintBuffer{_key_data, _key_length}) << ", offset " << offset);
  return id;
}

/**
 * @brief Extracts the key from a composite database key.
 *
 * @param _composedKey Sliver object of the composite database key.
 * @return Sliver object of the key extracted from the composite database key.
 */
Sliver DBKeyManipulator::extractKeyFromKeyComposedWithBlockId(const Key &_composedKey) {
  size_t sz = _composedKey.length() - sizeof(BlockId) - sizeof(EDBKeyType);
  Sliver out = Sliver(_composedKey, sizeof(EDBKeyType), sz);
  LOG_TRACE(logger(), "Got key " << out << " from composed key " << _composedKey);
  return out;
}

/**
 * @brief Compare the part of the composed key that would have been returned
 * from extractKeyFromKeyComposedWithBlockId.
 *
 * This is an optimization, to avoid creating sub-Slivers that will be destroyed
 * immediately in the RocksKeyComparator.
 *
 * @param a_data Pointer to buffer containing a composed key.
 * @param a_length Number of bytes in a_data.
 * @param b_data Pointer to buffer containing a composed key.
 * @param b_length Number of bytes in b_data.
 */
int DBKeyManipulator::compareKeyPartOfComposedKey(const char *a_data,
                                                  size_t a_length,
                                                  const char *b_data,
                                                  size_t b_length) {
  assert(a_length >= sizeof(BlockId) + sizeof(EDBKeyType));
  assert(b_length >= sizeof(BlockId) + sizeof(EDBKeyType));

  const char *a_key = a_data + sizeof(EDBKeyType);
  const size_t a_key_length = a_length - sizeof(BlockId) - sizeof(EDBKeyType);
  const char *b_key = b_data + sizeof(EDBKeyType);
  const size_t b_key_length = b_length - sizeof(BlockId) - sizeof(EDBKeyType);

  int result = memcmp(a_key, b_key, std::min(a_key_length, b_key_length));
  if (a_key_length == b_key_length) {
    return result;
  }

  if (result == 0) {
    if (a_key_length < b_key_length) {
      return -1;
    } else {
      return 1;
    }
  }

  return result;
}

Sliver DBKeyManipulator::extractKeyFromMetadataKey(const Key &_composedKey) {
  size_t sz = _composedKey.length() - sizeof(EDBKeyType);
  Sliver out = Sliver(_composedKey, sizeof(EDBKeyType), sz);
  LOG_TRACE(logger(), "Got metadata key " << out << " from composed key " << _composedKey);
  return out;
}

bool DBKeyManipulator::isKeyContainBlockId(const Key &_composedKey) {
  return (_composedKey.length() > sizeof(BlockId) + sizeof(EDBKeyType));
}

/**
 * @brief Converts a key value pair consisting using a composite database key
 * into a key value pair using a "simple" key - one without the key type
 * included.
 *
 * @param _p Key value pair consisting of a composite database key
 * @return Key value pair consisting of a simple key.
 */
KeyValuePair DBKeyManipulator::composedToSimple(KeyValuePair _p) {
  if (_p.first.length() == 0) {
    return _p;
  }
  Key key;
  if (isKeyContainBlockId(_p.first))
    key = extractKeyFromKeyComposedWithBlockId(_p.first);
  else
    key = extractKeyFromMetadataKey(_p.first);

  return KeyValuePair(key, _p.second);
}

/**
 * @brief Generates a Composite Database Key from objectId.
 *
 * Merges the key type and the block id to generate a composite
 * database key for E_DB_KEY_TYPE_METADATA_KEY.
 *
 * Format : Key Type | Object Id
 *
 * @return Sliver object of the generated composite database key.
 */

Sliver DBKeyManipulator::generateMetadataKey(ObjectId objectId) {
  size_t keySize = sizeof(EDBKeyType) + sizeof(objectId);
  auto keyBuf = new char[keySize];
  size_t offset = 0;
  EDBKeyType keyType = EDBKeyType::E_DB_KEY_TYPE_BFT_METADATA_KEY;
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&keyType, sizeof(EDBKeyType));
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&objectId, sizeof(objectId));
  return Sliver(keyBuf, keySize);
}
/*
 * Format : Key Type | Object Id
 */
Sliver DBKeyManipulator::generateStateTransferKey(ObjectId objectId) {
  size_t keySize = sizeof(EDBKeyType) + sizeof(objectId);
  auto keyBuf = new char[keySize];
  size_t offset = 0;
  EDBKeyType keyType = EDBKeyType::E_DB_KEY_TYPE_BFT_ST_KEY;
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&keyType, sizeof(EDBKeyType));
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&objectId, sizeof(objectId));
  return Sliver(keyBuf, keySize);
}
/**
 * Format : Key Type | Page Id
 */
Sliver DBKeyManipulator::generateSTPendingPageKey(uint32_t pageid) {
  size_t keySize = sizeof(EDBKeyType) + sizeof(pageid);
  auto keyBuf = new char[keySize];
  size_t offset = 0;
  auto keyType = EDBKeyType::E_DB_KEY_TYPE_BFT_ST_PENDING_PAGE_KEY;
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&keyType, sizeof(EDBKeyType));
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&pageid, sizeof(pageid));
  return Sliver(keyBuf, keySize);
}
/**
 * Format : Key Type | Checkpoint
 */
Sliver DBKeyManipulator::generateSTCheckpointDescriptorKey(uint64_t chkpt) {
  size_t keySize = sizeof(EDBKeyType) + sizeof(chkpt);
  auto keyBuf = new char[keySize];
  size_t offset = 0;
  auto keyType = EDBKeyType::E_DB_KEY_TYPE_BFT_ST_CHECKPOINT_DESCRIPTOR_KEY;
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&keyType, sizeof(EDBKeyType));
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&chkpt, sizeof(chkpt));
  return Sliver(keyBuf, keySize);
}
/**
 * Format : Key Type | Page Id | Checkpoint
 */
Sliver DBKeyManipulator::generateSTReservedPageStaticKey(uint32_t pageid, uint64_t chkpt) {
  return generateReservedPageKey(EDBKeyType::E_DB_KEY_TYPE_BFT_ST_RESERVED_PAGE_STATIC_KEY, pageid, chkpt);
}
/**
 * Format : Key Type | Page Id | Checkpoint
 */
Sliver DBKeyManipulator::generateSTReservedPageDynamicKey(uint32_t pageid, uint64_t chkpt) {
  return generateReservedPageKey(EDBKeyType::E_DB_KEY_TYPE_BFT_ST_RESERVED_PAGE_DYNAMIC_KEY, pageid, chkpt);
}
/**
 * Format : Key Type | Page Id | Checkpoint
 */
Sliver DBKeyManipulator::generateReservedPageKey(EDBKeyType keyType, uint32_t pageid, uint64_t chkpt) {
  size_t keySize = sizeof(EDBKeyType) + sizeof(pageid) + sizeof(chkpt);
  auto keyBuf = new char[keySize];
  size_t offset = 0;
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&keyType, sizeof(EDBKeyType));
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&pageid, sizeof(pageid));
  copyToAndAdvance(keyBuf, &offset, keySize, (char *)&chkpt, sizeof(chkpt));
  return Sliver(keyBuf, keySize);
}

bool DBKeyManipulator::copyToAndAdvance(char *_buf, size_t *_offset, size_t _maxOffset, char *_src, size_t _srcSize) {
  if (!_buf && !_offset && !_src) assert(false);

  if (*_offset >= _maxOffset && _srcSize > 0) assert(false);

  memcpy(_buf + *_offset, _src, _srcSize);
  *_offset += _srcSize;

  return true;
}

uint64_t DBKeyManipulator::extractCheckPointFromKey(const char *_key_data, size_t _key_length) {
  assert(_key_length >= sizeof(uint64_t));
  uint64_t chkp = *(uint64_t *)(_key_data + 1);

  LOG_TRACE(logger(), "checkpoint " << chkp << " from key " << (HexPrintBuffer{_key_data, _key_length}));
  return chkp;
}

std::pair<uint32_t, uint64_t> DBKeyManipulator::extractPageIdAndCheckpointFromKey(const char *_key_data,
                                                                                  size_t _key_length) {
  assert(_key_length >= sizeof(uint32_t) + sizeof(uint64_t));

  uint32_t pageId = *(uint32_t *)(_key_data + 1);
  uint64_t chkp = *(uint64_t *)(_key_data + sizeof(uint32_t) + 1);
  LOG_TRACE(logger(),
            "pageId " << pageId << " checkpoint " << chkp << " from key " << (HexPrintBuffer{_key_data, _key_length}));
  return std::make_pair(pageId, chkp);
}

DBAdapter::DBAdapter(std::shared_ptr<storage::IDBClient> db, std::unique_ptr<IDataKeyGenerator> keyGen)
    : DBAdapterBase{db}, keyGen_{std::move(keyGen)} {}

Status DBAdapter::addBlock(const SetOfKeyValuePairs &kv, BlockId blockId) {
  bftEngine::SimpleBlockchainStateTransfer::StateTransferDigest stDigest;
  if (blockId > 1) {
    Sliver parentBlockData;
    bool found;
    getBlockById(blockId - 1, parentBlockData, found);
    if (!found || parentBlockData.length() == 0) {
      //(IG): panic, data corrupted
      LOG_FATAL(logger_, "addBlock: no block or block data for id " << blockId - 1);
      std::exit(1);
    }

    bftEngine::SimpleBlockchainStateTransfer::computeBlockDigest(
        blockId - 1, reinterpret_cast<const char *>(parentBlockData.data()), parentBlockData.length(), &stDigest);
  } else {
    std::memset(stDigest.content, 0, bftEngine::SimpleBlockchainStateTransfer::BLOCK_DIGEST_SIZE);
  }

  SetOfKeyValuePairs outKv;
  const auto block = block::create(kv, outKv, stDigest.content);
  return addBlockAndUpdateMultiKey(outKv, blockId, block);
}

Status DBAdapter::addBlock(const concordUtils::Sliver &block, BlockId blockId) {
  SetOfKeyValuePairs keys;
  if (block.length() > 0) {
    keys = block::getData(block);
  }
  return addBlockAndUpdateMultiKey(keys, blockId, block);
}

Status DBAdapter::addBlockAndUpdateMultiKey(const SetOfKeyValuePairs &_kvMap, BlockId _block, const Sliver &_blockRaw) {
  SetOfKeyValuePairs updatedKVMap;
  for (auto &it : _kvMap) {
    Sliver composedKey = keyGen_->dataKey(it.first, _block);
    LOG_TRACE(logger_,
              "Updating composed key " << composedKey << " with value " << it.second << " in block " << _block);
    updatedKVMap[composedKey] = it.second;
  }
  updatedKVMap[keyGen_->blockKey(_block)] = _blockRaw;
  return db_->multiPut(updatedKVMap);
}

/**
 * @brief Deletes a key value pair from the database.
 *
 * Deletes the key value pair corresponding to the composite database key of
 * type "Key" generated from the key and block id provided as parameters to this
 * function.
 *
 * @param _key The key whose composite version needs to be deleted.
 * @param _blockId The block id (version) of the key to delete.
 * @return Status of the operation.
 */
Status DBAdapter::delKey(const Sliver &_key, BlockId _blockId) {
  Sliver composedKey = keyGen_->dataKey(_key, _blockId);
  LOG_TRACE(logger_, "Deleting key " << _key << " block id " << _blockId);
  Status s = db_->del(composedKey);
  return s;
}

/**
 * @brief Deletes a key value pair from the database.
 *
 * Deletes the key value pair corresponding to the composite database key of
 * type "Block" generated from the block id provided as a parameter to this
 * function.
 *
 * @param _blockId The ID of the block to be deleted.
 * @return Status of the operation.
 */
Status DBAdapter::delBlock(BlockId _blockId) {
  Sliver dbKey = keyGen_->blockKey(_blockId);
  Status s = db_->del(dbKey);
  return s;
}

void DBAdapter::deleteBlockAndItsKeys(BlockId blockId) {
  Sliver blockRaw;
  bool found = false;
  Status s = getBlockById(blockId, blockRaw, found);
  if (!s.isOK()) {
    LOG_FATAL(logger_, "Failed to read block id: " << blockId);
    exit(1);
  }
  KeysVector keysVec;
  if (found && blockRaw.length() > 0) {
    const auto numOfElements = ((block::detail::Header *)blockRaw.data())->numberOfElements;
    auto *entries = (block::detail::Entry *)(blockRaw.data() + sizeof(block::detail::Header));
    for (size_t i = 0u; i < numOfElements; i++) {
      keysVec.push_back(keyGen_->dataKey(Key(blockRaw, entries[i].keyOffset, entries[i].keySize), blockId));
    }
  }
  if (found) {
    keysVec.push_back(keyGen_->blockKey(blockId));
  }
  s = db_->multiDel(keysVec);
  if (!s.isOK()) {
    LOG_FATAL(logger_, "Failed to delete block id: " << blockId);
    exit(1);
  }
}

/**
 * @brief Searches for record in the database by the read version.
 *
 * Read version is used as the block id for generating a composite database key
 * which is then used to lookup in the database.
 *
 * @param readVersion BlockId object signifying the read version with which a
 *                    lookup needs to be done.
 * @param key Sliver object of the key.
 * @param outValue Sliver object where the value of the lookup result is stored.
 * @param outBlock BlockId object where the read version of the result is
 *                         stored.
 * @return Status OK
 */
Status DBAdapter::getKeyByReadVersion(BlockId readVersion,
                                      const Sliver &key,
                                      Sliver &outValue,
                                      BlockId &outBlock) const {
  LOG_TRACE(logger_, "Getting value of key " << key << " for read version " << readVersion);
  storage::IDBClient::IDBClientIterator *iter = db_->getIterator();
  Sliver foundKey, foundValue;
  Sliver searchKey = keyGen_->dataKey(key, readVersion);
  KeyValuePair p = iter->seekAtLeast(searchKey);
  foundKey = DBKeyManipulator::composedToSimple(p).first;
  foundValue = p.second;

  LOG_TRACE(logger_, "Found key " << foundKey << " and value " << foundValue);

  if (!iter->isEnd()) {
    BlockId currentReadVersion = DBKeyManipulator::extractBlockIdFromKey(p.first);

    // TODO(JGC): Ask about reason for version comparison logic
    if (currentReadVersion <= readVersion && foundKey == key) {
      outValue = foundValue;
      outBlock = currentReadVersion;
    } else {
      outValue = Sliver();
      outBlock = 0;
    }
  } else {
    outValue = Sliver();
    outBlock = 0;
  }

  db_->freeIterator(iter);

  // TODO(GG): maybe return status of the operation?
  return Status::OK();
}

/**
 * @brief Looks up data by block id.
 *
 * Constructs a composite database key using the block id to fire a get request.
 *
 * @param _blockId BlockId object used for looking up data.
 * @param _blockRaw Sliver object where the result of the lookup is stored.
 * @param _found true if lookup successful, else false.
 * @return Status of the operation.
 */
Status DBAdapter::getBlockById(BlockId _blockId, Sliver &_blockRaw, bool &_found) const {
  Sliver key = keyGen_->blockKey(_blockId);
  Status s = db_->get(key, _blockRaw);
  if (s.isNotFound()) {
    _found = false;
    return Status::OK();
  }

  _found = true;
  return s;
}

// TODO(SG): Add status checks with getStatus() on iterator.
// TODO(JGC): unserstand difference between .second and .data()

/**
 * @brief Used to retrieve the latest block.
 *
 * Searches for the key with the largest block id component.
 *
 * @return Block ID of the latest block.
 */
BlockId DBAdapter::getLatestBlock() const {
  // Note: RocksDB stores keys in a sorted fashion as per the logic provided in
  // a custom comparator (for our case, refer to the `composedKeyComparison`
  // method above). In short, keys of type 'block' are stored first followed by
  // keys of type 'key'. All keys of type 'block' are sorted in ascending order
  // of block ids.

  // Generate maximal key for type 'block'.
  const auto maxKey = keyGen_->blockKey(std::numeric_limits<BlockId>::max());
  auto iter = db_->getIteratorGuard();
  // Since we use the maximal key, SeekAtLeast will take the iterator
  // to one position beyond the key corresponding to the largest block id.
  iter->seekAtLeast(maxKey);

  // Read the previous key.
  const auto key = iter->previous().first;
  if (key.empty()) {  // no blocks
    return 0;
  }

  const auto blockId = DBKeyManipulator::extractBlockIdFromKey(key);
  LOG_TRACE(logger_, "Latest block ID " << blockId);
  return blockId;
}

/**
 * @brief Used to retrieve the last reachable block
 *
 * Searches for the last reachable block.
 * From ST perspective, this is maximal block number N
 * such that all blocks 1 <= i <= N exist.
 * In the normal state, it should be equal to last block ID
 *
 * @return Block ID of the last reachable block.
 */
BlockId DBAdapter::getLastReachableBlock() const {
  storage::IDBClient::IDBClientIterator *iter = db_->getIterator();

  BlockId lastReachableId = 0;
  Sliver blockKey = keyGen_->blockKey(1);
  KeyValuePair kvp = iter->seekAtLeast(blockKey);
  if (kvp.first.length() == 0) {
    db_->freeIterator(iter);
    return 0;
  }

  while (!iter->isEnd() && (DBKeyManipulator::extractTypeFromKey(kvp.first) == EDBKeyType::E_DB_KEY_TYPE_BLOCK)) {
    BlockId id = DBKeyManipulator::extractBlockIdFromKey(kvp.first);
    if (id == lastReachableId + 1) {
      lastReachableId++;
      kvp = iter->next();
    } else {
      break;
    }
  }

  db_->freeIterator(iter);
  return lastReachableId;
}

}  // namespace v1DirectKeyValue
}  // namespace concord::kvbc
