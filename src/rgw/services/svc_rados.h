// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "rgw/rgw_service.h"
#include "common/async/yield_context.h"

using RGWAccessListFilter =
  std::function<bool(std::string_view, std::string_view)>;

inline auto RGWAccessListFilterPrefix(std::string prefix) {
  return [prefix = std::move(prefix)](std::string_view,
                                      std::string_view key) -> bool {
           return (prefix.compare(key.substr(0, prefix.size())) == 0);
         };
}

class RGWSI_RADOS : public RGWServiceInstance
{
  librados::Rados rados;

  int do_start() override;

  int open_pool_ctx(const rgw_pool& pool, librados::IoCtx& io_ctx);
  int pool_iterate(librados::IoCtx& ioctx,
                   librados::NObjectIterator& iter,
                   uint32_t num, vector<rgw_bucket_dir_entry>& objs,
                   const RGWAccessListFilter& filter,
                   bool *is_truncated);

public:
  RGWSI_RADOS(CephContext *cct) : RGWServiceInstance(cct) {}

  void init() {}

  uint64_t instance_id();

  struct ObjRef {
    rgw_raw_obj obj;
    librados::IoCtx ioctx;
  };

  class Obj {
    friend class RGWSI_RADOS;

    RGWSI_RADOS *rados_svc{nullptr};
    ObjRef ref;

    void init(const rgw_raw_obj& obj);

    Obj(RGWSI_RADOS *_rados_svc, const rgw_raw_obj& _obj)
      : rados_svc(_rados_svc) {
      init(_obj);
    }

  public:
    Obj() {}

    int open();

    int operate(librados::ObjectWriteOperation *op, optional_yield y);
    int operate(librados::ObjectReadOperation *op, bufferlist *pbl,
                optional_yield y);
    int aio_operate(librados::AioCompletion *c, librados::ObjectWriteOperation *op);
    int aio_operate(librados::AioCompletion *c, librados::ObjectReadOperation *op,
                    bufferlist *pbl);

    int watch(uint64_t *handle, librados::WatchCtx2 *ctx);
    int aio_watch(librados::AioCompletion *c, uint64_t *handle, librados::WatchCtx2 *ctx);
    int unwatch(uint64_t handle);
    int notify(bufferlist& bl, uint64_t timeout_ms,
               bufferlist *pbl, optional_yield y);
    void notify_ack(uint64_t notify_id,
                    uint64_t cookie,
                    bufferlist& bl);

    uint64_t get_last_version();

    ObjRef& get_ref() { return ref; }
    const ObjRef& get_ref() const { return ref; }
  };

  class Pool {
    friend class RGWSI_RADOS;

    RGWSI_RADOS *rados_svc{nullptr};
    rgw_pool pool;

    Pool(RGWSI_RADOS *_rados_svc,
         const rgw_pool& _pool) : rados_svc(_rados_svc),
                                  pool(_pool) {}

    Pool(RGWSI_RADOS *_rados_svc) : rados_svc(_rados_svc) {}
  public:
    Pool() {}

    int create();
    int create(const std::vector<rgw_pool>& pools, std::vector<int> *retcodes);
    int lookup();

    struct List {
      Pool& pool;

      struct Ctx {
        bool initialized{false};
        librados::IoCtx ioctx;
        librados::NObjectIterator iter;
        RGWAccessListFilter filter;
      } ctx;

      List(Pool& _pool) : pool(_pool) {}

      int init(const string& marker, RGWAccessListFilter&& filter = nullptr);
      int get_next(int max,
                   std::list<string> *oids,
                   bool *is_truncated);
    };

    List op() {
      return List(*this);
    }

    friend List;
  };

  int watch_flush();

  Obj obj(const rgw_raw_obj& o) {
    return Obj(this, o);
  }

  Pool pool() {
    return Pool(this);
  }

  Pool pool(const rgw_pool& p) {
    return Pool(this, p);
  }

  friend Obj;
  friend Pool;
  friend Pool::List;
};
