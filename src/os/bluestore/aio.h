// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once
# include <libaio.h>

#include <boost/intrusive/list.hpp>
#include <boost/container/small_vector.hpp>

#include "include/buffer.h"
#include "include/types.h"

struct aio_t {
  struct iocb iocb{};  // must be first element; see shenanigans in aio_queue_t
  void *priv;
  int fd;
  boost::container::small_vector<iovec,4> iov;
  uint64_t offset, length;
  int rval;
  bufferlist bl;  ///< write payload (so that it remains stable for duration)

  boost::intrusive::list_member_hook<> queue_item;

  aio_t(void *p, int f) : priv(p), fd(f), offset(0), length(0), rval(-1000) {
  }

  void pwritev(uint64_t _offset, uint64_t len) {
    offset = _offset;
    length = len;
    io_prep_pwritev(&iocb, fd, &iov[0], iov.size(), offset);
  }
  void pread(uint64_t _offset, uint64_t len) {
    offset = _offset;
    length = len;
    bufferptr p = buffer::create_page_aligned(length);
    io_prep_pread(&iocb, fd, p.c_str(), length, offset);
    bl.append(std::move(p));
  }

  int get_return_value() {
    return rval;
  }
};

std::ostream& operator<<(std::ostream& os, const aio_t& aio);

typedef boost::intrusive::list<
  aio_t,
  boost::intrusive::member_hook<
    aio_t,
    boost::intrusive::list_member_hook<>,
    &aio_t::queue_item> > aio_list_t;

struct aio_queue_t {
  int max_iodepth;
  io_context_t ctx;

  typedef list<aio_t>::iterator aio_iter;

  explicit aio_queue_t(unsigned max_iodepth)
    : max_iodepth(max_iodepth),
      ctx(0) {
  }
  ~aio_queue_t() {
    assert(ctx == 0);
  }

  int init() {
    assert(ctx == 0);
    int r = io_setup(max_iodepth, &ctx);
    if (r < 0) {
      if (ctx) {
	io_destroy(ctx);
	ctx = 0;
      }
    }
    return r;
  }
  void shutdown() {
    if (ctx) {
      int r = io_destroy(ctx);
      assert(r == 0);
      ctx = 0;
    }
  }

  int submit_batch(aio_iter begin, aio_iter end, uint16_t aios_size, 
		   void *priv, int *retries);
  int get_next_completed(int timeout_ms, aio_t **paio, int max);
};
