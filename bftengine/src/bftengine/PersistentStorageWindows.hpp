// Concord
//
// Copyright (c) 2019 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").
// You may not use this product except in compliance with the Apache 2.0
// License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the sub-component's license, as noted in the LICENSE
// file.

#pragma once

#include "SerializableActiveWindow.hpp"
#include "SerializableActiveWindow.cpp"
#include "PrePrepareMsg.hpp"
#include "SignedShareMsgs.hpp"
#include "FullCommitProofMsg.hpp"
#include "CheckpointMsg.hpp"

namespace bftEngine {
namespace impl {

class SeqNumData {
 public:
  SeqNumData(PrePrepareMsg *prePrepare, FullCommitProofMsg *fullCommitProof,
             PrepareFullMsg *prepareFull, CommitFullMsg *commitFull,
             bool forceComplete, bool slowStart) :
      prePrepareMsg_(prePrepare), fullCommitProofMsg_(fullCommitProof),
      prepareFullMsg_(prepareFull), commitFullMsg_(commitFull),
      forceCompleted_(forceComplete), slowStarted_(slowStart) {}

  SeqNumData() = default;

  bool equals(const SeqNumData &other) const;
  void reset();
  void serialize(char *buf, uint32_t bufLen, size_t &actualSize) const;

  static SeqNumData deserialize(char *buf, uint32_t bufLen, uint32_t &actualSize);
  static uint32_t maxSize();

 private:
  static bool compareMessages(MessageBase *msg, MessageBase *otherMsg);

 public:
  PrePrepareMsg *prePrepareMsg_ = nullptr;
  FullCommitProofMsg *fullCommitProofMsg_ = nullptr;
  PrepareFullMsg *prepareFullMsg_ = nullptr;
  CommitFullMsg *commitFullMsg_ = nullptr;
  bool forceCompleted_ = false;
  bool slowStarted_ = false;
};

class CheckData {
 public:
  CheckData(CheckpointMsg *checkpoint, bool completed) :
      checkpointMsg_(checkpoint), completedMark_(completed) {}

  CheckData() = default;

  bool equals(const CheckData &other) const;
  void reset();
  void serialize(char *buf, uint32_t bufLen, size_t &actualSize) const;

  static CheckData deserialize(char *buf, uint32_t bufLen, uint32_t &actualSize);
  static uint32_t maxSize();

 public:
  CheckpointMsg *checkpointMsg_ = nullptr;
  bool completedMark_ = false;
};

typedef SerializableActiveWindow<kWorkWindowSize, 1, SeqNumData> SeqNumWindow;

typedef SerializableActiveWindow<kWorkWindowSize + checkpointWindowSize, checkpointWindowSize, CheckData> CheckWindow;
}
}  // namespace bftEngine
