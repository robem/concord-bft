// Copyright 2018-2019 VMware, all rights reserved

#pragma once

#include "kv_types.hpp"
#include "status.hpp"

namespace concord::kvbc {
/**
 *
 */
class ILocalKeyValueStorageReadOnly {
 public:
  // convenience where readVersion==latest, and block is not needed?
  virtual concordUtils::Status get(const Key& key, Value& outValue) const = 0;
  virtual concordUtils::Status get(BlockId readVersion, const Key& key, Value& outValue, BlockId& outBlock) const = 0;

  virtual BlockId getLastBlock() const = 0;
  virtual concordUtils::Status getBlockData(BlockId blockId, SetOfKeyValuePairs& outBlockData) const = 0;
  // TODO(GG): explain motivation
  virtual concordUtils::Status mayHaveConflictBetween(const Key& key,
                                                      BlockId fromBlock,
                                                      BlockId toBlock,
                                                      bool& outRes) const = 0;

  virtual void monitor() const = 0;

  virtual ~ILocalKeyValueStorageReadOnly() = default;
};

/**
 *
 */
class IBlocksAppender {
 public:
  virtual concordUtils::Status addBlock(const SetOfKeyValuePairs& updates, BlockId& outBlockId) = 0;

  virtual ~IBlocksAppender() = default;
};

/** Defines interface for blockchain data keys generation
 *
 */
class IDataKeyGenerator {
 public:
  virtual concordUtils::Sliver blockKey(const BlockId&) const = 0;
  virtual concordUtils::Sliver dataKey(const Key&, const BlockId&) const = 0;

  virtual ~IDataKeyGenerator() = default;
};

}  // namespace concord::kvbc
