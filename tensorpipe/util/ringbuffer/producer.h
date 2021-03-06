/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <tensorpipe/util/ringbuffer/ringbuffer.h>

namespace tensorpipe {
namespace util {
namespace ringbuffer {

///
/// Producer of data for a RingBuffer.
///
/// Provides methods to write data into a ringbuffer.
///
class Producer {
 public:
  Producer() = delete;

  explicit Producer(RingBuffer& rb)
      : header_{rb.getHeader()}, data_{rb.getData()} {
    TP_THROW_IF_NULLPTR(data_);
  }

  Producer(const Producer&) = delete;
  Producer(Producer&&) = delete;

  Producer& operator=(const Producer&) = delete;
  Producer& operator=(Producer&&) = delete;

  ~Producer() noexcept {
    TP_THROW_ASSERT_IF(inTx());
  }

  size_t getSize() const {
    return header_.kDataPoolByteSize;
  }

  //
  // Transaction based API.
  //
  // Only one writer can have an active transaction at any time.
  // *InTx* operations that fail do not cancel transaction.
  //
  bool inTx() const noexcept {
    return inTx_;
  }

  [[nodiscard]] ssize_t startTx() noexcept {
    if (unlikely(inTx())) {
      return -EBUSY;
    }
    if (header_.beginWriteTransaction()) {
      return -EAGAIN;
    }
    inTx_ = true;
    TP_DCHECK_EQ(tx_size_, 0);
    return 0;
  }

  [[nodiscard]] ssize_t commitTx() noexcept {
    if (unlikely(!inTx())) {
      return -EINVAL;
    }
    header_.incHead(tx_size_);
    tx_size_ = 0;
    // <in_write_tx> flags that we are in a transaction,
    // so enforce no stores pass it.
    inTx_ = false;
    header_.endWriteTransaction();
    return 0;
  }

  [[nodiscard]] ssize_t cancelTx() noexcept {
    if (unlikely(!inTx())) {
      return -EINVAL;
    }
    tx_size_ = 0;
    // <in_write_tx> flags that we are in a transaction,
    // so enforce no stores pass it.
    inTx_ = false;
    header_.endWriteTransaction();
    return 0;
  }

  struct Buffer {
    uint8_t* ptr{nullptr};
    size_t len{0};
  };

  // The first item is negative in case of error, otherwise it contains how many
  // elements of the array are valid (0, 1 or 2). The elements are ptr+len pairs
  // of contiguous areas of the ringbuffer that, chained together, represent a
  // slice of the requested size (or less if not enough data is available, and
  // allowPartial is set to true).
  template <bool allowPartial>
  [[nodiscard]] std::pair<ssize_t, std::array<Buffer, 2>> accessContiguousInTx(
      size_t size) noexcept {
    std::array<Buffer, 2> result;

    if (unlikely(!inTx())) {
      return {-EINVAL, result};
    }

    if (unlikely(size == 0)) {
      return {0, result};
    }

    const uint64_t head = header_.readHead();
    const uint64_t tail = header_.readTail();
    TP_DCHECK_LE(head - tail, header_.kDataPoolByteSize);

    const size_t avail = header_.kDataPoolByteSize - (head - tail) - tx_size_;
    TP_DCHECK_GE(avail, 0);

    if (!allowPartial && avail < size) {
      return {-ENOSPC, result};
    }

    if (avail == 0) {
      return {0, result};
    }

    size = std::min(size, avail);

    const uint64_t start = (head + tx_size_) & header_.kDataModMask;
    const uint64_t end = (start + size) & header_.kDataModMask;

    tx_size_ += size;

    // end == 0 is the same as end == bufferSize, in which case it doesn't wrap.
    const bool wrap = (start >= end && end > 0);
    if (likely(!wrap)) {
      result[0] = {.ptr = data_ + start, .len = size};
      return {1, result};
    } else {
      result[0] = {.ptr = data_ + start,
                   .len = header_.kDataPoolByteSize - start};
      result[1] = {.ptr = data_, .len = end};
      return {2, result};
    }
  }

  // Copy data from the provided buffer into the ringbuffer, up to the given
  // size (only copy less data if allowPartial is set to true).
  template <bool allowPartial>
  [[nodiscard]] ssize_t writeInTx(
      const void* buffer,
      const size_t size) noexcept {
    ssize_t numBuffers;
    std::array<Buffer, 2> buffers;
    std::tie(numBuffers, buffers) = accessContiguousInTx<allowPartial>(size);

    if (unlikely(numBuffers < 0)) {
      return numBuffers;
    }

    if (unlikely(numBuffers == 0)) {
      // Nothing to do.
      return 0;
    } else if (likely(numBuffers == 1)) {
      std::memcpy(buffers[0].ptr, buffer, buffers[0].len);
      return buffers[0].len;
    } else if (likely(numBuffers == 2)) {
      std::memcpy(buffers[0].ptr, buffer, buffers[0].len);
      std::memcpy(
          buffers[1].ptr,
          reinterpret_cast<const uint8_t*>(buffer) + buffers[0].len,
          buffers[1].len);
      return buffers[0].len + buffers[1].len;
    } else {
      TP_THROW_ASSERT() << "Bad number of buffers: " << numBuffers;
      // Dummy return to make the compiler happy.
      return -EINVAL;
    }
  }

  //
  // High-level atomic operations.
  //

  // Copy data from the provided buffer into the ringbuffer, exactly the given
  // size. Take care of opening and closing the transaction.
  [[nodiscard]] ssize_t write(const void* buffer, size_t size) noexcept {
    auto ret = startTx();
    if (0 > ret) {
      return ret;
    }

    ret = writeInTx</*allowPartial=*/false>(buffer, size);
    if (0 > ret) {
      auto r = cancelTx();
      TP_DCHECK_EQ(r, 0);
      return ret;
    }
    TP_DCHECK_EQ(ret, size);

    ret = commitTx();
    TP_DCHECK_EQ(ret, 0);

    return size;
  }

 private:
  RingBufferHeader& header_;
  uint8_t* const data_;
  unsigned tx_size_ = 0;
  bool inTx_{false};
};

} // namespace ringbuffer
} // namespace util
} // namespace tensorpipe
