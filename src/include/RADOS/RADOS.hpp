// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2018 Red Hat <contact@redhat.com>
 * Author: Adam C. Emerson
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <cstddef>
#include <memory>
#include <tuple>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include <boost/asio.hpp>

#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/uuid/uuid.hpp>


// HATE. LET ME TELL YOU HOW MUCH I'VE COME TO HATE BOOST.SYSTEM SINCE
// I BEGAN TO LIVE.

#include <boost/system/error_code.hpp>

#include "include/function2.hpp"

// Needed for type erasure and template support. We can't really avoid
// it.

#include "common/async/completion.h"

// These are needed for RGW, but in general as a 'shiny new interface'
// we should try to use forward declarations and provide standard alternatives.

#include "include/ceph_fs.h"
#include "include/rados/rados_types.hpp"
#include "include/buffer.h"
#include "include/object.h"

#include "common/ceph_time.h"

// Figure out exactly what pool stat info we want to expose to clients
// and get rid of this.

#include "osd/osd_types.h"

#include "librados/ListObjectImpl.h"

#ifndef RADOS_UNLEASHED_HPP
#define RADOS_UNLEASHED_HPP

class CephContext;

namespace RADOS {
class Object;
}
namespace std {
template<>
struct hash<RADOS::Object>;
}

namespace RADOS {

class RADOS;

// Exists mostly so that repeated operations on the same object don't
// have to pay for the string copy to construct an object_t.

class Object {
  friend RADOS;
  friend std::hash<Object>;

public:
  Object(std::string_view s);
  Object(std::string&& s);
  Object(const std::string& s);
  ~Object();

  Object(const Object& o);
  Object& operator =(const Object& o);

  Object(Object&& o);
  Object& operator =(Object&& o);

  operator std::string_view() const;

  friend bool operator <(const Object& lhs, const Object& rhs);
  friend bool operator <=(const Object& lhs, const Object& rhs);
  friend bool operator >=(const Object& lhs, const Object& rhs);
  friend bool operator >(const Object& lhs, const Object& rhs);

  friend bool operator ==(const Object& lhs, const Object& rhs);
  friend bool operator !=(const Object& lhs, const Object& rhs);

private:

  static constexpr std::size_t impl_size = 4 * 8;
  std::aligned_storage_t<impl_size> impl;
};

// Not the same as the librados::IoCtx, but it does gather together
// some of the same metadata. Since we're likely to do multiple
// operations in the same pool or namespace, it doesn't make sense to
// redo a bunch of lookups and string copies.

struct IOContext {
  friend RADOS;

  IOContext();
  explicit IOContext(std::int64_t pool);
  IOContext(std::int64_t _pool, std::string_view _ns);
  IOContext(std::int64_t _pool, std::string&& _ns);
  ~IOContext();

  IOContext(const IOContext& rhs);
  IOContext& operator =(const IOContext& rhs);

  IOContext(IOContext&& rhs);
  IOContext& operator =(IOContext&& rhs);

  std::int64_t pool() const;
  void pool(std::int64_t _pool);

  std::string_view ns() const;
  void ns(std::string_view _ns);
  void ns(std::string&& _ns);

  // Because /some fool/ decided to disallow optional references,
  // you'd have to construct a string in an optional which I would then
  // take an optional reference to. Thus a separate 'clear' method.
  std::optional<std::string_view> key() const;
  void key(std::string_view _key);
  void key(std::string&& _key);
  void clear_key();

  std::optional<std::int64_t> hash() const;
  void hash(std::int64_t _hash);
  void clear_hash();

  std::optional<std::uint64_t> read_snap() const;
  void read_snap(std::optional<std::uint64_t> _snapid);

  // I can't actually move-construct here since snapid_t is its own
  // separate class type, not an alias.
  std::optional<
    std::pair<std::uint64_t,
	      std::vector<std::uint64_t>>> write_snap_context() const;
  void write_snap_context(std::optional<
			  std::pair<std::uint64_t,
			              std::vector<std::uint64_t>>> snapc);
private:

  static constexpr std::size_t impl_size = 16 * 8;
  std::aligned_storage_t<impl_size> impl;
};

class Op {
  friend RADOS;

public:

  Op(const Op&) = delete;
  Op& operator =(const Op&) = delete;
  Op(Op&&);
  Op& operator =(Op&&);
  ~Op();

  void set_excl();
  void set_failok();
  void set_fadvise_random();
  void set_fadvise_sequential();
  void set_fadvise_willneed();
  void set_fadvise_dontneed();
  void set_fadvise_nocache();

  void cmpext(uint64_t off, buffer::list&& cmp_bl, std::size_t* s);
  void cmpxattr(std::string_view name, uint8_t op, const bufferlist& val);
  void cmpxattr(std::string_view name, uint8_t op, std::uint64_t val);
  void assert_version(uint64_t ver);
  void assert_exists();
  void cmp_omap(const boost::container::flat_map<
		  std::string,
		  std::pair<ceph::buffer::list, int>>& assertions);

  using Signature = void(boost::system::error_code);
  using Completion = ceph::async::Completion<Signature>;
protected:
  Op();
  static constexpr std::size_t impl_size = 20 * 8;
  std::aligned_storage_t<impl_size> impl;
};

// This class is /not/ thread-safe. If you want you can wrap it in
// something that locks it.

class ReadOp : public Op {
  friend RADOS;

public:

  using Op::Op;
  using Op::operator =;

  void read(size_t off, uint64_t len, ceph::buffer::list* out);
  void getxattr(std::string_view name, ceph::buffer::list* out);
  void get_omap_header(ceph::buffer::list*);

  void sparse_read(uint64_t off, uint64_t len,
		   ceph::buffer::list* out,
		   std::vector<std::pair<std::uint64_t, std::uint64_t>>* extents);

  void stat(std::uint64_t* size, ceph::real_time* mtime);

  void get_omap_keys(std::optional<std::string_view> start_after,
		     std::uint64_t max_return,
		     boost::container::flat_set<std::string>* keys,
		     bool* truncated);


  void get_xattrs(boost::container::flat_map<std::string,
					     ceph::buffer::list>* kv);

  void get_omap_vals(std::optional<std::string_view> start_after,
		     std::optional<std::string_view> filter_prefix,
		     uint64_t max_return,
		     boost::container::flat_map<std::string,
		                                ceph::buffer::list>* kv,
		     bool* truncated);

  void get_omap_vals_by_keys(const boost::container::flat_set<std::string>& keys,
			     boost::container::flat_map<std::string,
			                                ceph::buffer::list>* kv);

  void list_watchers(std::vector<obj_watch_t>* watchers);
  void list_snaps(librados::snap_set_t* snaps);

  void exec(std::string_view cls, std::string_view method,
	    const bufferlist& inbl,
	    ceph::buffer::list* out);

private:
  ReadOp();
};

class WriteOp : public Op {
  friend RADOS;
public:

  using Op::Op;
  using Op::operator =;

  WriteOp(const WriteOp&) = delete;
  WriteOp& operator =(const WriteOp&) = delete;
  WriteOp(WriteOp&&);
  WriteOp& operator =(WriteOp&&);

  ~WriteOp();

  void set_mtime(ceph::real_time t);
  void create(bool exclusive);
  void write(uint64_t off, bufferlist&& bl);
  void write_full(bufferlist&& bl);
  void writesame(std::uint64_t off, std::uint64_t write_len, bufferlist&& bl);
  void append(bufferlist&& bl);
  void remove();
  void truncate(uint64_t off);
  void zero(uint64_t off, uint64_t len);
  void rmxattr(std::string_view name);
  void setxattr(std::string_view name,
		bufferlist&& bl);
  void rollback(uint64_t snapid);
  void set_omap(const boost::container::flat_map<std::string, bufferlist>& map);
  void set_omap_header(bufferlist&& bl);
  void clear_omap();
  void rm_omap_keys(const boost::container::flat_set<std::string>& to_rm);
  void set_alloc_hint(uint64_t expected_object_size,
		      uint64_t expected_write_size,
		      uint32_t flags);
  void exec(std::string_view cls, std::string_view method, const bufferlist& inbl);
private:
  WriteOp();
};

// Come back and refactor this layer properly at some point.
using EnumeratedObject = librados::ListObjectImpl;
class EnumerationCursor {
public:
  static EnumerationCursor begin();
  static EnumerationCursor end();

  EnumerationCursor(const EnumerationCursor&);
  EnumerationCursor& operator =(const EnumerationCursor&);
  EnumerationCursor(EnumerationCursor&&);
  EnumerationCursor& operator =(EnumerationCursor&&);
  ~EnumerationCursor();

  friend bool operator ==(const EnumerationCursor& lhs,
			  const EnumerationCursor& rhs);
  friend bool operator !=(const EnumerationCursor& lhs,
			  const EnumerationCursor& rhs);
  friend bool operator <(const EnumerationCursor& lhs,
			 const EnumerationCursor& rhs);
  friend bool operator <=(const EnumerationCursor& lhs,
			  const EnumerationCursor& rhs);
  friend bool operator >=(const EnumerationCursor& lhs,
			  const EnumerationCursor& rhs);
  friend bool operator >(const EnumerationCursor& lhs,
			 const EnumerationCursor& rhs);

private:
  EnumerationCursor();
  friend RADOS;
  static constexpr std::size_t impl_size = 16 * 8;
  std::aligned_storage_t<impl_size> impl;
};

class RADOS
{
public:
  static constexpr std::tuple<uint32_t, uint32_t, uint32_t> version() {
    return {0, 0, 1};
  }

  RADOS(boost::asio::io_context& ioctx);
  RADOS(boost::asio::io_context& ioctx, std::string_view id);
  RADOS(boost::asio::io_context& ioctx, std::string_view name,
	std::string_view cluster);
  RADOS(boost::asio::io_context& ioctx, CephContext* cct);

  RADOS(const RADOS&) = delete;
  RADOS& operator =(const RADOS&) = delete;
  RADOS(RADOS&&) = delete;
  RADOS& operator =(RADOS&&) = delete;

  CephContext* cct();

  ReadOp make_ReadOp();
  WriteOp make_WriteOp();

  using executor_type = boost::asio::io_context::executor_type;
  executor_type get_executor();

  template<typename CompletionToken>
  auto execute(const Object& o, const IOContext& ioc, ReadOp&& op,
	       CompletionToken&& token, version_t* objver = nullptr) {
    boost::asio::async_completion<CompletionToken, Op::Signature> init(token);
    execute(o, ioc, std::move(op),
	    ReadOp::Completion::create(get_executor(),
				       std::move(init.completion_handler)),
	    objver);
    return init.result.get();
  }

  template<typename CompletionToken>
  auto execute(const Object& o, const IOContext& ioc, WriteOp&& op,
	       CompletionToken&& token, version_t* objver = nullptr) {
    boost::asio::async_completion<CompletionToken, Op::Signature> init(token);
    execute(o, ioc, std::move(op),
	    Op::Completion::create(get_executor(),
				   std::move(init.completion_handler)),
	    objver);
    return init.result.get();
  }

  template<typename CompletionToken>
  auto execute(const Object& o, std::int64_t pool,
	       ReadOp&& op,
	       CompletionToken&& token,
	       std::optional<std::string_view> ns = {},
	       std::optional<std::string_view> key = {},
	       version_t* objver = nullptr) {
    boost::asio::async_completion<CompletionToken, Op::Signature> init(token);
    execute(o, pool, std::move(op),
	    ReadOp::Completion::create(get_executor(),
				       std::move(init.completion_handler)),
	    ns, key, objver);
    return init.result.get();
  }

  template<typename CompletionToken>
  auto execute(const Object& o, std::int64_t pool, WriteOp&& op,
	       CompletionToken&& token,
	       std::optional<std::string_view> ns = {},
	       std::optional<std::string_view> key = {},
	       version_t* objver = nullptr) {
    boost::asio::async_completion<CompletionToken, Op::Signature> init(token);
    execute(o, pool, std::move(op),
	    Op::Completion::create(get_executor(),
				   std::move(init.completion_handler)),
	    ns, key, objver);
    return init.result.get();
  }

  boost::uuids::uuid get_fsid() const noexcept;

  using LookupPoolSig = void(boost::system::error_code,
			     std::int64_t);
  using LookupPoolComp = ceph::async::Completion<LookupPoolSig>;
  template<typename CompletionToken>
  auto lookup_pool(std::string name,
		   CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, LookupPoolSig> init(token);
    lookup_pool(std::move(name),
		LookupPoolComp::create(get_executor(),
				       std::move(init.completion_handler)));
    return init.result.get();
  }

  std::optional<uint64_t> get_pool_alignment(int64_t pool_id);
  std::vector<std::pair<std::int64_t, std::string>> list_pools();

  using SimpleOpSig = void(boost::system::error_code);
  using SimpleOpComp = ceph::async::Completion<SimpleOpSig>;
  template<typename CompletionToken>
  auto create_pool_snap(int64_t pool, std::string_view snapName,
			CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    create_pool_snap(pool, snapName,
		     SimpleOpComp::create(get_executor(),
					  std::move(init.completion_handler)));
    return init.result.get();
  }

  using SMSnapSig = void(boost::system::error_code, snapid_t);
  using SMSnapComp = ceph::async::Completion<SMSnapSig>;
  template<typename CompletionToken>
  auto allocate_selfmanaged_snap(int64_t pool,
				 CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SMSnapSig> init(token);
    allocate_selfmanaged_snap(pool,
			      SMSnapComp::create(
				get_executor(),
				std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto delete_pool_snap(int64_t pool, std::string_view snapName,
			CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    delete_pool_snap(pool, snapName,
		     SimpleOpComp::create(get_executor(),
					  std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto delete_selfmanaged_snap(int64_t pool, std::string_view snapName,
			       CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    delete_selfmanaged_snap(pool, snapName,
			    SimpleOpComp::create(
			      get_executor(),
			      std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto create_pool(std::string_view name, std::optional<int> crush_rule,
		   CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    create_pool(name, crush_rule,
		SimpleOpComp::create(get_executor(),
				     std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto delete_pool(std::string_view name,
		   CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    delete_pool(name,
		SimpleOpComp::create(get_executor(),
				     std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto delete_pool(int64_t pool,
		   CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    delete_pool(pool,
		SimpleOpComp::create(get_executor(),
				     std::move(init.completion_handler)));
    return init.result.get();
  }

  using PoolStatSig = void(boost::system::error_code,
			   boost::container::flat_map<std::string,
			                              pool_stat_t>, bool);
  using PoolStatComp = ceph::async::Completion<PoolStatSig>;
  template<typename CompletionToken>
  auto stat_pools(const std::vector<std::string>& pools,
		  CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, PoolStatSig> init(token);
    stat_pools(pools,
	       PoolStatComp::create(get_executor(),
				    std::move(init.completion_handler)));
    return init.result.get();
  }

  using StatFSSig = void(boost::system::error_code,
			 ceph_statfs);
  using StatFSComp = ceph::async::Completion<StatFSSig>;
  template<typename CompletionToken>
  auto statfs(std::optional<int64_t> pool,
	      CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, StatFSSig> init(token);
    ceph_statfs(pool, StatFSComp::create(get_executor(),
					 std::move(init.completion_handler)));
    return init.result.get();
  }

  using WatchCB = fu2::unique_function<void(boost::system::error_code,
					    uint64_t notify_id,
					    uint64_t cookie,
					    uint64_t notifier_id,
					    bufferlist&& bl)>;

  using WatchSig = void(boost::system::error_code ec,
			uint64_t cookie);
  using WatchComp = ceph::async::Completion<WatchSig>;
  template<typename CompletionToken>
  auto watch(const Object& o, const IOContext& ioc,
	     std::optional<std::chrono::seconds> timeout,
	     WatchCB&& cb, CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, WatchSig> init(token);
    watch(o, ioc, timeout, std::move(cb),
	  WatchComp::create(get_executor(),
			    std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto watch(const Object& o, std::int64_t pool,
	     std::optional<std::chrono::seconds> timeout,
	     WatchCB&& cb, CompletionToken&& token,
	     std::optional<std::string_view> ns = {},
	     std::optional<std::string_view> key = {}) {
    boost::asio::async_completion<CompletionToken, WatchSig> init(token);
    watch(o, pool, timeout, std::move(cb),
	  WatchComp::create(get_executor(),
			    std::move(init.completion_handler)),
	  ns, key);
    return init.result.get();
  }

  template<typename CompletionToken>
  auto notify_ack(const Object& o,
		  const IOContext& ioc,
		  uint64_t notify_id,
		  uint64_t cookie,
		  bufferlist&& bl,
		  CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    notify_ack(o, ioc, notify_id, cookie, std::move(bl),
	       SimpleOpComp::create(get_executor(),
				    std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto notify_ack(const Object& o,
		  std::int64_t pool,
		  uint64_t notify_id,
		  uint64_t cookie,
		  bufferlist&& bl,
		  CompletionToken&& token,
		  std::optional<std::string_view> ns = {},
		  std::optional<std::string_view> key = {}) {
    boost::asio::async_completion<CompletionToken, WatchSig> init(token);
    notify_ack(o, pool, notify_id, cookie, std::move(bl),
	       SimpleOpComp::create(get_executor(),
				    std::move(init.completion_handler)),
	       ns, key);
    return init.result.get();
  }

  template<typename CompletionToken>
  auto unwatch(uint64_t cookie, const IOContext& ioc,
	       CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    unwatch(cookie, ioc,
	    SimpleOpComp::create(get_executor(),
				 std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto unwatch(uint64_t cookie, std::int64_t pool,
	       CompletionToken&& token,
	       std::optional<std::string_view> ns = {},
	       std::optional<std::string_view> key = {}) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    unwatch(cookie, pool,
	    SimpleOpComp::create(get_executor(),
				 std::move(init.completion_handler)),
	    ns, key);
    return init.result.get();
  }

  // This is one of those places where having to force everything into
  // a .cc file is really infuriating. If we had modules, that would
  // let us separate out the implementation details without
  // sacrificing all the benefits of templates.
  using VoidOpSig = void();
  using VoidOpComp = ceph::async::Completion<VoidOpSig>;
  template<typename CompletionToken>
  auto flush_watch(CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, VoidOpSig> init(token);
    flush_watch(VoidOpComp::create(get_executor(),
				   std::move(init.completion_handler)));
    return init.result.get();
  }

  using NotifySig = void(boost::system::error_code, bufferlist);
  using NotifyComp = ceph::async::Completion<NotifySig>;
  template<typename CompletionToken>
  auto notify(const Object& oid, const IOContext& ioc, bufferlist&& bl,
	      std::optional<std::chrono::milliseconds> timeout,
	      CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, NotifySig> init(token);
    notify(oid, ioc, std::move(bl), timeout,
	   NotifyComp::create(get_executor(),
			      std::move(init.completion_handler)));

    return init.result.get();
  }

  template<typename CompletionToken>
  auto notify(const Object& oid, std::int64_t pool, bufferlist&& bl,
	      std::optional<std::chrono::milliseconds> timeout,
	      CompletionToken&& token,
	      std::optional<std::string_view> ns = {},
	      std::optional<std::string_view> key = {}) {
    boost::asio::async_completion<CompletionToken, NotifySig> init(token);
    notify(oid, pool, bl, timeout,
	   NotifyComp::create(get_executor(),
			      std::move(init.completion_handler)),
	   ns, key);

    return init.result.get();
  }

  template<typename CompletionToken>
  auto enumerate_objects(const IOContext& ioc, const EnumerationCursor& begin,
			 const EnumerationCursor& end, const std::uint32_t max,
			 const bufferlist& filter,
			 std::vector<EnumeratedObject>* ls,
			 EnumerationCursor* cursor,
			 CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    enumerate_objects(ioc, begin, end, max, filter,
		      ls, cursor,
		      SimpleOpComp::create(get_executor(),
					   std::move(init.completion_handler)));
    return init.result.get();
  }

  template<typename CompletionToken>
  auto enumerate_objects(std::int64_t pool, const EnumerationCursor& begin,
			 const EnumerationCursor& end, const std::uint32_t max,
			 const bufferlist& filter,
			 std::vector<EnumeratedObject>* ls,
			 EnumerationCursor* cursor,
			 CompletionToken&& token,
			 std::optional<std::string_view> ns = {},
			 std::optional<std::string_view> key = {}) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    enumerate_objects(pool, begin, end, max, filter,
		      ls, cursor,
		      SimpleOpComp::create(get_executor(),
					   std::move(init.completion_handler)),
		      ns, key);
    return init.result.get();
  }

  using CommandSig = void(boost::system::error_code,
			  std::string, ceph::bufferlist);
  using CommandComp = ceph::async::Completion<CommandSig>;
  template<typename CompletionToken>
  auto osd_command(int osd, std::vector<std::string>&& cmd,
		   ceph::bufferlist&& in, CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, CommandSig> init(token);
    osd_command(osd, std::move(cmd), std::move(in),
		CommandComp::create(get_executor(),
				      std::move(init.completion_handler)));
    return init.result.get();
  }
  template<typename CompletionToken>
  auto pg_command(pg_t pg, std::vector<std::string>&& cmd,
		  ceph::bufferlist&& in, CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, CommandSig> init(token);
    pg_command(pg, std::move(cmd), std::move(in),
	       CommandComp::create(get_executor(),
				      std::move(init.completion_handler)));
    return init.result.get();
  }
  template<typename CompletionToken>
  auto enable_application(std::string_view pool, std::string_view app_name,
			  bool force, CompletionToken&& token) {
    boost::asio::async_completion<CompletionToken, SimpleOpSig> init(token);
    enable_application(pool, app_name, force,
		       SimpleOpComp::create(get_executor(),
					    std::move(init.completion_handler)));
    return init.result.get();
  }

private:

  void execute(const Object& o, const IOContext& ioc, ReadOp&& op,
	       std::unique_ptr<Op::Completion> c, version_t* objver);

  void execute(const Object& o, const IOContext& ioc, WriteOp&& op,
	       std::unique_ptr<Op::Completion> c, version_t* objver);

  void execute(const Object& o, std::int64_t pool, ReadOp&& op,
	       std::unique_ptr<Op::Completion> c,
	       std::optional<std::string_view> ns,
	       std::optional<std::string_view> key,
	       version_t* objver);

  void execute(const Object& o, std::int64_t pool, WriteOp&& op,
	       std::unique_ptr<Op::Completion> c,
	       std::optional<std::string_view> ns,
	       std::optional<std::string_view> key,
	       version_t* objver);

  void lookup_pool(std::string name, std::unique_ptr<LookupPoolComp> c);
  void create_pool_snap(int64_t pool, std::string_view snapName,
			std::unique_ptr<SimpleOpComp> c);
  void allocate_selfmanaged_snap(int64_t pool, std::unique_ptr<SMSnapComp> c);
  void delete_pool_snap(int64_t pool, std::string_view snapName,
			std::unique_ptr<SimpleOpComp> c);
  void delete_selfmanaged_snap(int64_t pool, snapid_t snap,
			       std::unique_ptr<SimpleOpComp> c);
  void create_pool(std::string_view name, std::optional<int> crush_rule,
		   std::unique_ptr<SimpleOpComp> c);
  void delete_pool(std::string_view name,
		   std::unique_ptr<SimpleOpComp> c);
  void delete_pool(int64_t pool,
		   std::unique_ptr<SimpleOpComp> c);
  void stat_pools(const std::vector<std::string>& pools,
		  std::unique_ptr<PoolStatComp> c);
  void stat_fs(std::optional<std::int64_t> pool,
	       std::unique_ptr<StatFSComp> c);

  void watch(const Object& o, const IOContext& ioc,
	     std::optional<std::chrono::seconds> timeout,
	     WatchCB&& cb, std::unique_ptr<WatchComp> c);
  void watch(const Object& o, std::int64_t pool,
	     std::optional<std::chrono::seconds> timeout,
	     WatchCB&& cb, std::unique_ptr<WatchComp> c,
	     std::optional<std::string_view> ns,
	     std::optional<std::string_view> key);
  boost::system::error_code watch_check(uint64_t cookie);
  void notify_ack(const Object& o,
		  const IOContext& _ioc,
		  uint64_t notify_id,
		  uint64_t cookie,
		  bufferlist&& bl,
		  std::unique_ptr<SimpleOpComp>);
  void notify_ack(const Object& o,
		  std::int64_t pool,
		  uint64_t notify_id,
		  uint64_t cookie,
		  bufferlist&& bl,
		  std::unique_ptr<SimpleOpComp>,
		  std::optional<std::string_view> ns,
		  std::optional<std::string_view> key);
  void unwatch(uint64_t cookie, const IOContext& ioc,
	       std::unique_ptr<SimpleOpComp>);
  void unwatch(uint64_t cookie, std::int64_t pool,
	       std::unique_ptr<SimpleOpComp>,
	       std::optional<std::string_view> ns,
	       std::optional<std::string_view> key);
  void notify(const Object& oid, const IOContext& ioctx, bufferlist&& bl,
	      std::optional<std::chrono::milliseconds> timeout,
	      std::unique_ptr<NotifyComp> c);
  void notify(const Object& oid, std::int64_t pool, bufferlist&& bl,
	      std::optional<std::chrono::milliseconds> timeout,
	      std::unique_ptr<NotifyComp> c,
	      std::optional<std::string_view> ns,
	      std::optional<std::string_view> key);
  void flush_watch(std::unique_ptr<VoidOpComp>);

  void enumerate_objects(const IOContext& ioc, const EnumerationCursor& begin,
			 const EnumerationCursor& end, const std::uint32_t max,
			 const bufferlist& filter,
			 std::vector<EnumeratedObject>* ls,
			 EnumerationCursor* cursor,
			 std::unique_ptr<SimpleOpComp> c);
  void enumerate_objects(std::int64_t pool, const EnumerationCursor& begin,
			 const EnumerationCursor& end, const std::uint32_t max,
			 const bufferlist& filter,
			 std::vector<EnumeratedObject>* ls,
			 EnumerationCursor* cursor,
			 std::unique_ptr<SimpleOpComp> c,
			 std::optional<std::string_view> ns,
			 std::optional<std::string_view> key);
  void osd_command(int osd, std::vector<std::string>&& cmd,
		   ceph::bufferlist&& in, std::unique_ptr<CommandComp> c);
  void pg_command(pg_t pg, std::vector<std::string>&& cmd,
		  ceph::bufferlist&& in, std::unique_ptr<CommandComp> c);
  void enable_application(std::string_view pool, std::string_view app_name,
			  bool force, std::unique_ptr<SimpleOpComp> c);


  static constexpr std::size_t impl_size = 512 * 8;
  std::aligned_storage_t<impl_size> impl;
};
}

namespace std {
template<>
struct hash<RADOS::Object> {
  size_t operator ()(const RADOS::Object& r) const;
};
} // namespace std

#endif
