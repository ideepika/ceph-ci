// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/range/adaptor/map.hpp>

#include "common/Formatter.h"
#include "common/PriorityCache.h"
#include "common/admin_socket.h"
#include "common/debug.h"
#include "common/errno.h"
#include "journal/Types.h"
#include "librbd/ImageCtx.h"
#include "perfglue/heap_profiler.h"
#include "Mirror.h"
#include "ServiceDaemon.h"
#include "Threads.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd_mirror

using std::list;
using std::map;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using librados::Rados;
using librados::IoCtx;
using librbd::mirror_peer_t;

namespace rbd {
namespace mirror {

namespace {

class MirrorAdminSocketCommand {
public:
  virtual ~MirrorAdminSocketCommand() {}
  virtual bool call(Formatter *f, stringstream *ss) = 0;
};

class StatusCommand : public MirrorAdminSocketCommand {
public:
  explicit StatusCommand(Mirror *mirror) : mirror(mirror) {}

  bool call(Formatter *f, stringstream *ss) override {
    mirror->print_status(f, ss);
    return true;
  }

private:
  Mirror *mirror;
};

class StartCommand : public MirrorAdminSocketCommand {
public:
  explicit StartCommand(Mirror *mirror) : mirror(mirror) {}

  bool call(Formatter *f, stringstream *ss) override {
    mirror->start();
    return true;
  }

private:
  Mirror *mirror;
};

class StopCommand : public MirrorAdminSocketCommand {
public:
  explicit StopCommand(Mirror *mirror) : mirror(mirror) {}

  bool call(Formatter *f, stringstream *ss) override {
    mirror->stop();
    return true;
  }

private:
  Mirror *mirror;
};

class RestartCommand : public MirrorAdminSocketCommand {
public:
  explicit RestartCommand(Mirror *mirror) : mirror(mirror) {}

  bool call(Formatter *f, stringstream *ss) override {
    mirror->restart();
    return true;
  }

private:
  Mirror *mirror;
};

class FlushCommand : public MirrorAdminSocketCommand {
public:
  explicit FlushCommand(Mirror *mirror) : mirror(mirror) {}

  bool call(Formatter *f, stringstream *ss) override {
    mirror->flush();
    return true;
  }

private:
  Mirror *mirror;
};

class LeaderReleaseCommand : public MirrorAdminSocketCommand {
public:
  explicit LeaderReleaseCommand(Mirror *mirror) : mirror(mirror) {}

  bool call(Formatter *f, stringstream *ss) override {
    mirror->release_leader();
    return true;
  }

private:
  Mirror *mirror;
};

#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::PriCache: " << this << " " \
                           << m_name << " " << __func__ << ": "

struct PriCache : public PriorityCache::PriCache {
  std::string m_name;
  int64_t m_base_cache_max_size;
  int64_t m_extra_cache_max_size;

  PriorityCache::Priority m_base_cache_pri = PriorityCache::Priority::PRI10;
  PriorityCache::Priority m_extra_cache_pri = PriorityCache::Priority::PRI10;
  int64_t m_base_cache_bytes = 0;
  int64_t m_extra_cache_bytes = 0;
  int64_t m_committed_bytes = 0;
  double m_cache_ratio = 0;

  PriCache(const std::string &name, uint64_t min_size, uint64_t max_size)
    : m_name(name), m_base_cache_max_size(min_size),
      m_extra_cache_max_size(max_size - min_size) {
    ceph_assert(max_size >= min_size);
  }

  void prioritize() {
    if (m_base_cache_pri == PriorityCache::Priority::PRI0) {
      return;
    }
    auto pri = static_cast<uint8_t>(m_base_cache_pri);
    m_base_cache_pri = static_cast<PriorityCache::Priority>(--pri);

    dout(30) << m_base_cache_pri << dendl;
  }

  int64_t request_cache_bytes(PriorityCache::Priority pri,
                              uint64_t total_cache) const override {
    int64_t cache_bytes = 0;

    if (pri == m_base_cache_pri) {
      cache_bytes += m_base_cache_max_size;
    }
    if (pri == m_extra_cache_pri) {
      cache_bytes += m_extra_cache_max_size;
    }

    dout(30) << cache_bytes << dendl;

    return cache_bytes;
  }

  int64_t get_cache_bytes(PriorityCache::Priority pri) const override {
    int64_t cache_bytes = 0;

    if (pri == m_base_cache_pri) {
      cache_bytes += m_base_cache_bytes;
    }
    if (pri == m_extra_cache_pri) {
      cache_bytes += m_extra_cache_bytes;
    }

    dout(30) << "pri=" << pri << " " << cache_bytes << dendl;

    return cache_bytes;
  }

  int64_t get_cache_bytes() const override {
    auto cache_bytes = m_base_cache_bytes + m_extra_cache_bytes;

    dout(30) << m_base_cache_bytes << "+" << m_extra_cache_bytes << "="
             << cache_bytes << dendl;

    return cache_bytes;
  }

  void set_cache_bytes(PriorityCache::Priority pri, int64_t bytes) override {
    ceph_assert(bytes >= 0);
    ceph_assert(pri == m_base_cache_pri || pri == m_extra_cache_pri ||
                bytes == 0);

    dout(30) << "pri=" << pri << " " << bytes << dendl;

    if (pri == m_base_cache_pri) {
      m_base_cache_bytes = std::min(m_base_cache_max_size, bytes);
      bytes -= std::min(m_base_cache_bytes, bytes);
    }

    if (pri == m_extra_cache_pri) {
      m_extra_cache_bytes = bytes;
    }
  }

  void add_cache_bytes(PriorityCache::Priority pri, int64_t bytes) override {
    ceph_assert(bytes >= 0);
    ceph_assert(pri == m_base_cache_pri || pri == m_extra_cache_pri);

    dout(30) << "pri=" << pri << " " << bytes << dendl;

    if (pri == m_base_cache_pri) {
      ceph_assert(m_base_cache_bytes <= m_base_cache_max_size);

      auto chunk = std::min(m_base_cache_max_size - m_base_cache_bytes, bytes);
      m_base_cache_bytes += chunk;
      bytes -= chunk;
    }

    if (pri == m_extra_cache_pri) {
      m_extra_cache_bytes += bytes;
    }
  }

  int64_t commit_cache_size(uint64_t total_cache) override {
    m_committed_bytes = p2roundup<int64_t>(get_cache_bytes(), 4096);

    dout(30) << m_committed_bytes << dendl;

    return m_committed_bytes;
  }

  int64_t get_committed_size() const override {
    dout(30) << m_committed_bytes << dendl;

    return m_committed_bytes;
  }

  double get_cache_ratio() const override {
    dout(30) << m_cache_ratio << dendl;

    return m_cache_ratio;
  }

  void set_cache_ratio(double ratio) override {
    dout(30) << m_cache_ratio << dendl;

    m_cache_ratio = ratio;
  }

  std::string get_cache_name() const override {
    return m_name;
  }
};

} // anonymous namespace

#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::Mirror: " << this << " " \
                           << __func__ << ": "

class MirrorAdminSocketHook : public AdminSocketHook {
public:
  MirrorAdminSocketHook(CephContext *cct, Mirror *mirror) :
    admin_socket(cct->get_admin_socket()) {
    std::string command;
    int r;

    command = "rbd mirror status";
    r = admin_socket->register_command(command, this,
				       "get status for rbd mirror");
    if (r == 0) {
      commands[command] = new StatusCommand(mirror);
    }

    command = "rbd mirror start";
    r = admin_socket->register_command(command, this,
				       "start rbd mirror");
    if (r == 0) {
      commands[command] = new StartCommand(mirror);
    }

    command = "rbd mirror stop";
    r = admin_socket->register_command(command, this,
				       "stop rbd mirror");
    if (r == 0) {
      commands[command] = new StopCommand(mirror);
    }

    command = "rbd mirror restart";
    r = admin_socket->register_command(command, this,
				       "restart rbd mirror");
    if (r == 0) {
      commands[command] = new RestartCommand(mirror);
    }

    command = "rbd mirror flush";
    r = admin_socket->register_command(command, this,
				       "flush rbd mirror");
    if (r == 0) {
      commands[command] = new FlushCommand(mirror);
    }

    command = "rbd mirror leader release";
    r = admin_socket->register_command(command, this,
				       "release rbd mirror leader");
    if (r == 0) {
      commands[command] = new LeaderReleaseCommand(mirror);
    }
  }

  ~MirrorAdminSocketHook() override {
    for (Commands::const_iterator i = commands.begin(); i != commands.end();
	 ++i) {
      (void)admin_socket->unregister_command(i->first);
      delete i->second;
    }
  }

  bool call(std::string_view command, const cmdmap_t& cmdmap,
	    std::string_view format, bufferlist& out) override {
    Commands::const_iterator i = commands.find(command);
    ceph_assert(i != commands.end());
    Formatter *f = Formatter::create(format);
    stringstream ss;
    bool r = i->second->call(f, &ss);
    delete f;
    out.append(ss);
    return r;
  }

private:
  typedef std::map<std::string, MirrorAdminSocketCommand*, std::less<>> Commands;

  AdminSocket *admin_socket;
  Commands commands;
};

class CacheManagerHandler : public journal::CacheManagerHandler {
public:
  CacheManagerHandler(CephContext *cct)
    : m_cct(cct) {

    if (!m_cct->_conf.get_val<bool>("rbd_mirror_memory_autotune")) {
      return;
    }

    uint64_t base = m_cct->_conf.get_val<Option::size_t>(
        "rbd_mirror_memory_base");
    double fragmentation = m_cct->_conf.get_val<double>(
        "rbd_mirror_memory_expected_fragmentation");
    uint64_t target = m_cct->_conf.get_val<Option::size_t>(
        "rbd_mirror_memory_target");
    uint64_t min = m_cct->_conf.get_val<Option::size_t>(
        "rbd_mirror_memory_cache_min");
    uint64_t max = min;

    // When setting the maximum amount of memory to use for cache, first
    // assume some base amount of memory for the daemon and then fudge in
    // some overhead for fragmentation that scales with cache usage.
    uint64_t ltarget = (1.0 - fragmentation) * target;
    if (ltarget > base + min) {
      max = ltarget - base;
    }

    m_next_balance = ceph_clock_now();
    m_next_resize = ceph_clock_now();

    m_cache_manager = std::make_unique<PriorityCache::Manager>(
      m_cct, min, max, target, false);
  }

  ~CacheManagerHandler() {
    std::lock_guard locker{m_lock};

    ceph_assert(m_caches.empty());
  }

  void register_cache(const std::string &cache_name,
                      uint64_t min_size, uint64_t max_size,
                      journal::CacheRebalanceHandler* handler) override {
    if (!m_cache_manager) {
      handler->handle_cache_rebalanced(max_size);
      return;
    }

    dout(20) << cache_name << " min_size=" << min_size << " max_size="
             << max_size << " handler=" << handler << dendl;

    std::lock_guard locker{m_lock};

    auto p = m_caches.insert(
        {cache_name, {cache_name, min_size, max_size, handler}});
    ceph_assert(p.second == true);

    m_cache_manager->insert(cache_name, p.first->second.pri_cache, false);
    m_next_balance = ceph_clock_now();
  }

  void unregister_cache(const std::string &cache_name) override {
    if (!m_cache_manager) {
      return;
    }

    dout(20) << cache_name << dendl;

    std::lock_guard locker{m_lock};

    auto it = m_caches.find(cache_name);
    ceph_assert(it != m_caches.end());

    m_cache_manager->erase(cache_name);
    m_caches.erase(it);
    m_next_balance = ceph_clock_now();
  }

  void run_cache_manager() {
    if (!m_cache_manager) {
      return;
    }

    std::lock_guard locker{m_lock};

    // Before we trim, check and see if it's time to rebalance/resize.
    auto autotune_interval = m_cct->_conf.get_val<double>(
        "rbd_mirror_memory_cache_autotune_interval");
    auto resize_interval = m_cct->_conf.get_val<double>(
        "rbd_mirror_memory_cache_resize_interval");

    utime_t now = ceph_clock_now();

    if (autotune_interval > 0 && m_next_balance <= now) {
      dout(20) << "balance" << dendl;
      m_cache_manager->balance();

      for (auto &it : m_caches) {
        auto pri_cache = static_cast<PriCache *>(it.second.pri_cache.get());
        auto new_cache_bytes = pri_cache->get_cache_bytes();
        it.second.handler->handle_cache_rebalanced(new_cache_bytes);
        pri_cache->prioritize();
      }

      m_next_balance = ceph_clock_now();
      m_next_balance += autotune_interval;
    }

    if (resize_interval > 0 && m_next_resize < now) {
      if (ceph_using_tcmalloc()) {
        dout(20) << "tune memory" << dendl;
        m_cache_manager->tune_memory();
      }

      m_next_resize = ceph_clock_now();
      m_next_resize += resize_interval;
    }
  }

private:
  struct Cache {
    std::shared_ptr<PriorityCache::PriCache> pri_cache;
    journal::CacheRebalanceHandler *handler;

    Cache(const std::string name, uint64_t min_size, uint64_t max_size,
          journal::CacheRebalanceHandler *handler)
      : pri_cache(new PriCache(name, min_size, max_size)), handler(handler) {
    }
  };

  CephContext *m_cct;

  mutable ceph::mutex m_lock =
    ceph::make_mutex("rbd::mirror::CacheManagerHandler");
  std::unique_ptr<PriorityCache::Manager> m_cache_manager;
  std::map<std::string, Cache> m_caches;

  utime_t m_next_balance;
  utime_t m_next_resize;
};

Mirror::Mirror(CephContext *cct, const std::vector<const char*> &args) :
  m_cct(cct),
  m_args(args),
  m_local(new librados::Rados()),
  m_cache_manager_handler(new CacheManagerHandler(cct)),
  m_asok_hook(new MirrorAdminSocketHook(cct, this))
{
  m_threads =
    &(cct->lookup_or_create_singleton_object<Threads<librbd::ImageCtx>>(
	"rbd_mirror::threads", false, cct));
  m_service_daemon.reset(new ServiceDaemon<>(m_cct, m_local, m_threads));
}

Mirror::~Mirror()
{
  delete m_asok_hook;
}

void Mirror::handle_signal(int signum)
{
  m_stopping = true;
  {
    std::lock_guard l{m_lock};
    m_cond.notify_all();
  }
}

int Mirror::init()
{
  int r = m_local->init_with_context(m_cct);
  if (r < 0) {
    derr << "could not initialize rados handle" << dendl;
    return r;
  }

  r = m_local->connect();
  if (r < 0) {
    derr << "error connecting to local cluster" << dendl;
    return r;
  }

  r = m_service_daemon->init();
  if (r < 0) {
    derr << "error registering service daemon: " << cpp_strerror(r) << dendl;
    return r;
  }

  m_local_cluster_watcher.reset(new ClusterWatcher(m_local, m_lock,
                                                   m_service_daemon.get()));
  return r;
}

void Mirror::run()
{
  dout(20) << "enter" << dendl;

  utime_t next_refresh_pools = ceph_clock_now();

  while (!m_stopping) {
    utime_t now = ceph_clock_now();
    bool refresh_pools = next_refresh_pools <= now;
    if (refresh_pools) {
      m_local_cluster_watcher->refresh_pools();
      next_refresh_pools = ceph_clock_now();
      next_refresh_pools += m_cct->_conf.get_val<uint64_t>(
          "rbd_mirror_pool_replayers_refresh_interval");
    }
    std::unique_lock l{m_lock};
    if (!m_manual_stop) {
      if (refresh_pools) {
        update_pool_replayers(m_local_cluster_watcher->get_pool_peers());
      }
      m_cache_manager_handler->run_cache_manager();
    }
    m_cond.wait_for(l, 1s);
  }

  // stop all pool replayers in parallel
  std::lock_guard locker{m_lock};
  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->stop(false);
  }
  dout(20) << "return" << dendl;
}

void Mirror::print_status(Formatter *f, stringstream *ss)
{
  dout(20) << "enter" << dendl;

  std::lock_guard l{m_lock};

  if (m_stopping) {
    return;
  }

  if (f) {
    f->open_object_section("mirror_status");
    f->open_array_section("pool_replayers");
  };

  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->print_status(f, ss);
  }

  if (f) {
    f->close_section();
  }
}

void Mirror::start()
{
  dout(20) << "enter" << dendl;
  std::lock_guard l{m_lock};

  if (m_stopping) {
    return;
  }

  m_manual_stop = false;

  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->start();
  }
}

void Mirror::stop()
{
  dout(20) << "enter" << dendl;
  std::lock_guard l{m_lock};

  if (m_stopping) {
    return;
  }

  m_manual_stop = true;

  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->stop(true);
  }
}

void Mirror::restart()
{
  dout(20) << "enter" << dendl;
  std::lock_guard l{m_lock};

  if (m_stopping) {
    return;
  }

  m_manual_stop = false;

  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->restart();
  }
}

void Mirror::flush()
{
  dout(20) << "enter" << dendl;
  std::lock_guard l{m_lock};

  if (m_stopping || m_manual_stop) {
    return;
  }

  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->flush();
  }
}

void Mirror::release_leader()
{
  dout(20) << "enter" << dendl;
  std::lock_guard l{m_lock};

  if (m_stopping) {
    return;
  }

  for (auto &pool_replayer : m_pool_replayers) {
    pool_replayer.second->release_leader();
  }
}

void Mirror::update_pool_replayers(const PoolPeers &pool_peers)
{
  dout(20) << "enter" << dendl;
  ceph_assert(ceph_mutex_is_locked(m_lock));

  // remove stale pool replayers before creating new pool replayers
  for (auto it = m_pool_replayers.begin(); it != m_pool_replayers.end();) {
    auto &peer = it->first.second;
    auto pool_peer_it = pool_peers.find(it->first.first);
    if (pool_peer_it == pool_peers.end() ||
        pool_peer_it->second.find(peer) == pool_peer_it->second.end()) {
      dout(20) << "removing pool replayer for " << peer << dendl;
      // TODO: make async
      it->second->shut_down();
      it = m_pool_replayers.erase(it);
    } else {
      ++it;
    }
  }

  for (auto &kv : pool_peers) {
    for (auto &peer : kv.second) {
      PoolPeer pool_peer(kv.first, peer);

      auto pool_replayers_it = m_pool_replayers.find(pool_peer);
      if (pool_replayers_it != m_pool_replayers.end()) {
        auto& pool_replayer = pool_replayers_it->second;
        if (pool_replayer->is_blacklisted()) {
          derr << "restarting blacklisted pool replayer for " << peer << dendl;
          // TODO: make async
          pool_replayer->shut_down();
          pool_replayer->init();
        } else if (!pool_replayer->is_running()) {
          derr << "restarting failed pool replayer for " << peer << dendl;
          // TODO: make async
          pool_replayer->shut_down();
          pool_replayer->init();
        }
      } else {
        dout(20) << "starting pool replayer for " << peer << dendl;
        unique_ptr<PoolReplayer<>> pool_replayer(
            new PoolReplayer<>(m_threads, m_service_daemon.get(),
                               m_cache_manager_handler.get(), kv.first, peer,
                               m_args));

        // TODO: make async
        pool_replayer->init();
        m_pool_replayers.emplace(pool_peer, std::move(pool_replayer));
      }
    }

    // TODO currently only support a single peer
  }
}

} // namespace mirror
} // namespace rbd
