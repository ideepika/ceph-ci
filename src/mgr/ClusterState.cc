// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 John Spray <john.spray@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "messages/MMgrDigest.h"
#include "messages/MMonMgrReport.h"
#include "messages/MPGStats.h"

#include "mgr/ClusterState.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mgr
#undef dout_prefix
#define dout_prefix *_dout << "mgr " << __func__ << " "

ClusterState::ClusterState(
  MonClient *monc_,
  Objecter *objecter_,
  const MgrMap& mgrmap)
  : monc(monc_),
    objecter(objecter_),
    lock("ClusterState"),
    mgr_map(mgrmap),
    pgservice(pg_map)
{}

void ClusterState::set_objecter(Objecter *objecter_)
{
  Mutex::Locker l(lock);

  objecter = objecter_;
}

void ClusterState::set_fsmap(FSMap const &new_fsmap)
{
  Mutex::Locker l(lock);

  fsmap = new_fsmap;
}

void ClusterState::set_mgr_map(MgrMap const &new_mgrmap)
{
  Mutex::Locker l(lock);
  mgr_map = new_mgrmap;
}

void ClusterState::set_service_map(ServiceMap const &new_service_map)
{
  Mutex::Locker l(lock);
  servicemap = new_service_map;
}

void ClusterState::load_digest(MMgrDigest *m)
{
  health_json = std::move(m->health_json);
  mon_status_json = std::move(m->mon_status_json);
}

void ClusterState::ingest_pgstats(MPGStats *stats)
{
  Mutex::Locker l(lock);

  const int from = stats->get_orig_source().num();

  pending_inc.update_stat(from, stats->epoch, std::move(stats->osd_stat));

  for (auto p : stats->pg_stat) {
    pg_t pgid = p.first;
    const auto &pg_stats = p.second;

    // In case we're hearing about a PG that according to last
    // OSDMap update should not exist
    if (existing_pools.count(pgid.pool()) == 0) {
      dout(15) << " got " << pgid
	       << " reported at " << pg_stats.reported_epoch << ":"
               << pg_stats.reported_seq
               << " state " << pg_state_string(pg_stats.state)
               << " but pool not in " << existing_pools
               << dendl;
      continue;
    }
    // In case we already heard about more recent stats from this PG
    // from another OSD
    const auto q = pg_map.pg_stat.find(pgid);
    if (q != pg_map.pg_stat.end() &&
	q->second.get_version_pair() > pg_stats.get_version_pair()) {
      dout(15) << " had " << pgid << " from "
	       << q->second.reported_epoch << ":"
               << q->second.reported_seq << dendl;
      continue;
    }

    pending_inc.pg_stat_updates[pgid] = pg_stats;

    // try to drop any pending stale state since we are hearing again
    pending_stale.erase(pgid);
  }

  for (const auto &p : stats->op_stat) {
    auto &op_stat = p.second;
    pg_map.pool_op_stat[p.first.pool()].add(op_stat);
  }

  utime_t now = ceph_clock_now();
  if (now - pg_map.last_sampled >= g_conf->get_val<int64_t>("mgr_op_latency_sample_interval")) {
     pg_map.calc_pool_op_latency();
     pg_map.last_sampled = now;
  }
}

void ClusterState::try_mark_pg_stale() {
  utime_t now = ceph_clock_now();
  double delay = g_conf->get_val<double>("mgr_mark_pg_stale_delay");
  for (auto it = pending_stale.begin();
       it != pending_stale.end(); /* no inc */) {
    auto cur = pg_map.pg_stat.find(it->first);
    if (cur == pg_map.pg_stat.end()) {
      pending_stale.erase(it++);
      continue;
    }
    if (cur->second.state & PG_STATE_STALE) {
      // already stale
      pending_stale.erase(it++);
      continue;
    }

    if (now - it->second >= delay) {
      pg_stat_t *newstat;
      auto pi = pending_inc.pg_stat_updates.find(it->first);
      if (pi != pending_inc.pg_stat_updates.end()) {
        if (pi->second.state & PG_STATE_STALE) {
          it++; // pending to mark
          continue;
        } else {
          newstat = &pi->second;
        }
      } else {
        newstat = &pending_inc.pg_stat_updates[it->first];
        *newstat = cur->second;
      }
      newstat->state |= PG_STATE_STALE;
      dout(10) << " mark pg (" << *it
	       << ") to stale at " << now << dendl;
    }
    it++;
  }
}

void ClusterState::update_delta_stats()
{
  pending_inc.stamp = ceph_clock_now();
  pending_inc.version = pg_map.version + 1; // to make apply_incremental happy
  dout(10) << " v" << pending_inc.version << dendl;

  dout(30) << " pg_map before:\n";
  JSONFormatter jf(true);
  jf.dump_object("pg_map", pg_map);
  jf.flush(*_dout);
  *_dout << dendl;
  dout(30) << " incremental:\n";
  JSONFormatter jf(true);
  jf.dump_object("pending_inc", pending_inc);
  jf.flush(*_dout);
  *_dout << dendl;

  pg_map.apply_incremental(g_ceph_context, pending_inc);
  pending_inc = PGMap::Incremental();
}

void ClusterState::notify_osdmap(const OSDMap &osd_map)
{
  assert(lock.is_locked_by_me());

  pending_inc.stamp = ceph_clock_now();
  pending_inc.version = pg_map.version + 1; // to make apply_incremental happy
  dout(10) << " v" << pending_inc.version << dendl;

  PGMapUpdater::check_osd_map(g_ceph_context, osd_map, pg_map, &pending_inc);

  // update our list of pools that exist, so that we can filter pg_map updates
  // in synchrony with this OSDMap.
  existing_pools.clear();
  for (auto& p : osd_map.get_pools()) {
    existing_pools.insert(p.first);
  }

  // brute force this for now (don't bother being clever by only
  // checking osds that went up/down)
  set<int> need_check_down_pg_osds;
  PGMapUpdater::check_down_pgs(osd_map, pg_map, true,
			       need_check_down_pg_osds, pending_stale);

  dout(30) << " pg_map before:\n";
  JSONFormatter jf(true);
  jf.dump_object("pg_map", pg_map);
  jf.flush(*_dout);
  *_dout << dendl;
  dout(30) << " incremental:\n";
  JSONFormatter jf(true);
  jf.dump_object("pending_inc", pending_inc);
  jf.flush(*_dout);
  *_dout << dendl;

  pg_map.apply_incremental(g_ceph_context, pending_inc);
  pending_inc = PGMap::Incremental();
  // TODO: Complete the separation of PG state handling so
  // that a cut-down set of functionality remains in PGMonitor
  // while the full-blown PGMap lives only here.
}

void ClusterState::dump(Formatter *f) {
  f->open_object_section("pg pending stale");
  for (auto &ps: pending_stale) {
    std::stringstream oss;
    oss << ps.first;
    f->dump_stream(oss.str().c_str()) << ps.second;
  }
  f->close_section();
}

