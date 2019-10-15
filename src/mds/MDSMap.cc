// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "common/debug.h"
#include "mon/health_check.h"

#include "MDSMap.h"

#include <sstream>
using std::stringstream;

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_

// features
CompatSet MDSMap::get_compat_set_all() {
  CompatSet::FeatureSet feature_compat;
  CompatSet::FeatureSet feature_ro_compat;
  CompatSet::FeatureSet feature_incompat;
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_BASE);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_CLIENTRANGES);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_FILELAYOUT);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_DIRINODE);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_ENCODING);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_OMAPDIRFRAG);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_INLINE);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_NOANCHOR);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_FILE_LAYOUT_V2);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_SNAPREALM_V2);

  return CompatSet(feature_compat, feature_ro_compat, feature_incompat);
}

CompatSet MDSMap::get_compat_set_default() {
  CompatSet::FeatureSet feature_compat;
  CompatSet::FeatureSet feature_ro_compat;
  CompatSet::FeatureSet feature_incompat;
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_BASE);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_CLIENTRANGES);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_FILELAYOUT);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_DIRINODE);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_ENCODING);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_OMAPDIRFRAG);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_NOANCHOR);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_FILE_LAYOUT_V2);
  feature_incompat.insert(MDS_FEATURE_INCOMPAT_SNAPREALM_V2);

  return CompatSet(feature_compat, feature_ro_compat, feature_incompat);
}

// base (pre v0.20)
CompatSet MDSMap::get_compat_set_base() {
  CompatSet::FeatureSet feature_compat_base;
  CompatSet::FeatureSet feature_incompat_base;
  feature_incompat_base.insert(MDS_FEATURE_INCOMPAT_BASE);
  CompatSet::FeatureSet feature_ro_compat_base;

  return CompatSet(feature_compat_base, feature_ro_compat_base, feature_incompat_base);
}

void MDSMap::mds_info_t::dump(Formatter *f) const
{
  f->dump_unsigned("gid", global_id);
  f->dump_string("name", name);
  f->dump_int("rank", rank);
  f->dump_int("incarnation", inc);
  f->dump_stream("state") << ceph_mds_state_name(state);
  f->dump_int("state_seq", state_seq);
  f->dump_stream("addr") << addrs.get_legacy_str();
  f->dump_object("addrs", addrs);
  if (laggy_since != utime_t())
    f->dump_stream("laggy_since") << laggy_since;
  
  f->open_array_section("export_targets");
  for (set<mds_rank_t>::iterator p = export_targets.begin();
       p != export_targets.end(); ++p) {
    f->dump_int("mds", *p);
  }
  f->close_section();
  f->dump_unsigned("features", mds_features);
  f->dump_unsigned("flags", flags);
}

void MDSMap::mds_info_t::dump(std::ostream& o) const
{
  o << "[mds." << name << "{" <<  rank << ":" << global_id << "}"
       << " state " << ceph_mds_state_name(state)
       << " seq " << state_seq;
  if (laggy()) {
    o << " laggy since " << laggy_since;
  }
  if (!export_targets.empty()) {
    o << " export targets " << export_targets;
  }
  if (is_frozen()) {
    o << " frozen";
  }
  o << " addr " << addrs << "]";
}

void MDSMap::mds_info_t::generate_test_instances(std::list<mds_info_t*>& ls)
{
  mds_info_t *sample = new mds_info_t();
  ls.push_back(sample);
  sample = new mds_info_t();
  sample->global_id = 1;
  sample->name = "test_instance";
  sample->rank = 0;
  ls.push_back(sample);
}

void MDSMap::dump(Formatter *f) const
{
  f->dump_int("epoch", epoch);
  f->dump_unsigned("flags", flags);
  f->dump_unsigned("ever_allowed_features", ever_allowed_features);
  f->dump_unsigned("explicitly_allowed_features", explicitly_allowed_features);
  f->dump_stream("created") << created;
  f->dump_stream("modified") << modified;
  f->dump_int("tableserver", tableserver);
  f->dump_int("root", root);
  f->dump_int("session_timeout", session_timeout);
  f->dump_int("session_autoclose", session_autoclose);
  f->dump_stream("min_compat_client") << ceph::to_integer<int>(min_compat_client) << " ("
				      << min_compat_client << ")";
  f->dump_int("max_file_size", max_file_size);
  f->dump_int("last_failure", last_failure);
  f->dump_int("last_failure_osd_epoch", last_failure_osd_epoch);
  f->open_object_section("compat");
  compat.dump(f);
  f->close_section();
  f->dump_int("max_mds", max_mds);
  f->open_array_section("in");
  for (set<mds_rank_t>::const_iterator p = in.begin(); p != in.end(); ++p)
    f->dump_int("mds", *p);
  f->close_section();
  f->open_object_section("up");
  for (map<mds_rank_t,mds_gid_t>::const_iterator p = up.begin(); p != up.end(); ++p) {
    char s[14];
    sprintf(s, "mds_%d", int(p->first));
    f->dump_int(s, p->second);
  }
  f->close_section();
  f->open_array_section("failed");
  for (set<mds_rank_t>::const_iterator p = failed.begin(); p != failed.end(); ++p)
    f->dump_int("mds", *p);
  f->close_section();
  f->open_array_section("damaged");
  for (set<mds_rank_t>::const_iterator p = damaged.begin(); p != damaged.end(); ++p)
    f->dump_int("mds", *p);
  f->close_section();
  f->open_array_section("stopped");
  for (set<mds_rank_t>::const_iterator p = stopped.begin(); p != stopped.end(); ++p)
    f->dump_int("mds", *p);
  f->close_section();
  f->open_object_section("info");
  for (const auto& [gid, info] : mds_info) {
    char s[25]; // 'gid_' + len(str(ULLONG_MAX)) + '\0'
    sprintf(s, "gid_%llu", (long long unsigned)gid);
    f->open_object_section(s);
    info.dump(f);
    f->close_section();
  }
  f->close_section();
  f->open_array_section("data_pools");
  for (const auto& p: data_pools)
    f->dump_int("pool", p);
  f->close_section();
  f->dump_int("metadata_pool", metadata_pool);
  f->dump_bool("enabled", enabled);
  f->dump_string("fs_name", fs_name);
  f->dump_string("balancer", balancer);
  f->dump_int("standby_count_wanted", std::max(0, standby_count_wanted));
}

void MDSMap::generate_test_instances(std::list<MDSMap*>& ls)
{
  MDSMap *m = new MDSMap();
  m->max_mds = 1;
  m->data_pools.push_back(0);
  m->metadata_pool = 1;
  m->cas_pool = 2;
  m->compat = get_compat_set_all();

  // these aren't the defaults, just in case anybody gets confused
  m->session_timeout = 61;
  m->session_autoclose = 301;
  m->max_file_size = 1<<24;
  ls.push_back(m);
}

void MDSMap::print(ostream& out) const
{
  out << "fs_name\t" << fs_name << "\n";
  out << "epoch\t" << epoch << "\n";
  out << "flags\t" << hex << flags << dec << "\n";
  out << "created\t" << created << "\n";
  out << "modified\t" << modified << "\n";
  out << "tableserver\t" << tableserver << "\n";
  out << "root\t" << root << "\n";
  out << "session_timeout\t" << session_timeout << "\n"
      << "session_autoclose\t" << session_autoclose << "\n";
  out << "max_file_size\t" << max_file_size << "\n";
  out << "min_compat_client\t" << ceph::to_integer<int>(min_compat_client) << " ("
			       << min_compat_client << ")\n";
  out << "last_failure\t" << last_failure << "\n"
      << "last_failure_osd_epoch\t" << last_failure_osd_epoch << "\n";
  out << "compat\t" << compat << "\n";
  out << "max_mds\t" << max_mds << "\n";
  out << "in\t" << in << "\n"
      << "up\t" << up << "\n"
      << "failed\t" << failed << "\n"
      << "damaged\t" << damaged << "\n"
      << "stopped\t" << stopped << "\n";
  out << "data_pools\t" << data_pools << "\n";
  out << "metadata_pool\t" << metadata_pool << "\n";
  out << "inline_data\t" << (inline_data_enabled ? "enabled" : "disabled") << "\n";
  out << "balancer\t" << balancer << "\n";
  out << "standby_count_wanted\t" << std::max(0, standby_count_wanted) << "\n";

  multimap< pair<mds_rank_t, unsigned>, mds_gid_t > foo;
  for (const auto &p : mds_info) {
    foo.insert(std::make_pair(
          std::make_pair(p.second.rank, p.second.inc-1), p.first));
  }

  for (const auto &p : foo) {
    out << mds_info.at(p.second) << "\n";
  }
}

void MDSMap::print_summary(Formatter *f, ostream *out) const
{
  map<mds_rank_t,string> by_rank;
  map<string,int> by_state;

  if (f) {
    f->dump_unsigned("epoch", get_epoch());
    f->dump_unsigned("up", up.size());
    f->dump_unsigned("in", in.size());
    f->dump_unsigned("max", max_mds);
  } else {
    *out << "e" << get_epoch() << ": " << up.size() << "/" << in.size() << "/" << max_mds << " up";
  }

  if (f)
    f->open_array_section("by_rank");
  for (const auto &p : mds_info) {
    string s = ceph_mds_state_name(p.second.state);
    if (p.second.laggy())
      s += "(laggy or crashed)";

    if (p.second.rank >= 0 && p.second.state != MDSMap::STATE_STANDBY_REPLAY) {
      if (f) {
	f->open_object_section("mds");
	f->dump_unsigned("rank", p.second.rank);
	f->dump_string("name", p.second.name);
	f->dump_string("status", s);
	f->close_section();
      } else {
	by_rank[p.second.rank] = p.second.name + "=" + s;
      }
    } else {
      by_state[s]++;
    }
  }
  if (f) {
    f->close_section();
  } else {
    if (!by_rank.empty())
      *out << " " << by_rank;
  }

  for (map<string,int>::reverse_iterator p = by_state.rbegin(); p != by_state.rend(); ++p) {
    if (f) {
      f->dump_unsigned(p->first.c_str(), p->second);
    } else {
      *out << ", " << p->second << " " << p->first;
    }
  }

  if (!failed.empty()) {
    if (f) {
      f->dump_unsigned("failed", failed.size());
    } else {
      *out << ", " << failed.size() << " failed";
    }
  }

  if (!damaged.empty()) {
    if (f) {
      f->dump_unsigned("damaged", damaged.size());
    } else {
      *out << ", " << damaged.size() << " damaged";
    }
  }
  //if (stopped.size())
  //out << ", " << stopped.size() << " stopped";
}

void MDSMap::get_health(list<pair<health_status_t,string> >& summary,
			list<pair<health_status_t,string> > *detail) const
{
  if (!failed.empty()) {
    std::ostringstream oss;
    oss << "mds rank"
	<< ((failed.size() > 1) ? "s ":" ")
	<< failed
	<< ((failed.size() > 1) ? " have":" has")
	<< " failed";
    summary.push_back(make_pair(HEALTH_ERR, oss.str()));
    if (detail) {
      for (set<mds_rank_t>::const_iterator p = failed.begin(); p != failed.end(); ++p) {
	std::ostringstream oss;
	oss << "mds." << *p << " has failed";
	detail->push_back(make_pair(HEALTH_ERR, oss.str()));
      }
    }
  }

  if (!damaged.empty()) {
    std::ostringstream oss;
    oss << "mds rank"
	<< ((damaged.size() > 1) ? "s ":" ")
	<< damaged
	<< ((damaged.size() > 1) ? " are":" is")
	<< " damaged";
    summary.push_back(make_pair(HEALTH_ERR, oss.str()));
    if (detail) {
      for (set<mds_rank_t>::const_iterator p = damaged.begin(); p != damaged.end(); ++p) {
	std::ostringstream oss;
	oss << "mds." << *p << " is damaged";
	detail->push_back(make_pair(HEALTH_ERR, oss.str()));
      }
    }
  }

  if (is_degraded()) {
    summary.push_back(make_pair(HEALTH_WARN, "mds cluster is degraded"));
    if (detail) {
      detail->push_back(make_pair(HEALTH_WARN, "mds cluster is degraded"));
      for (mds_rank_t i = mds_rank_t(0); i< get_max_mds(); i++) {
	if (!is_up(i))
	  continue;
	mds_gid_t gid = up.find(i)->second;
	const auto& info = mds_info.at(gid);
	stringstream ss;
	if (is_resolve(i))
	  ss << "mds." << info.name << " at " << info.addrs
	     << " rank " << i << " is resolving";
	if (is_replay(i))
	  ss << "mds." << info.name << " at " << info.addrs
	     << " rank " << i << " is replaying journal";
	if (is_rejoin(i))
	  ss << "mds." << info.name << " at " << info.addrs
	     << " rank " << i << " is rejoining";
	if (is_reconnect(i))
	  ss << "mds." << info.name << " at " << info.addrs
	     << " rank " << i << " is reconnecting to clients";
	if (ss.str().length())
	  detail->push_back(make_pair(HEALTH_WARN, ss.str()));
      }
    }
  }

  {
  stringstream ss;
  ss << fs_name << " max_mds " << max_mds;
  summary.push_back(make_pair(HEALTH_WARN, ss.str()));
  }

  if ((mds_rank_t)up.size() < max_mds) {
    stringstream ss;
    ss << fs_name << " has " << up.size()
       << " active MDS(s), but has max_mds of " << max_mds;
    summary.push_back(make_pair(HEALTH_WARN, ss.str()));
  }

  set<string> laggy;
  for (const auto &u : up) {
    const auto& info = mds_info.at(u.second);
    if (info.laggy()) {
      laggy.insert(info.name);
      if (detail) {
	std::ostringstream oss;
	oss << "mds." << info.name << " at " << info.addrs
	    << " is laggy/unresponsive";
	detail->push_back(make_pair(HEALTH_WARN, oss.str()));
      }
    }
  }

  if (!laggy.empty()) {
    std::ostringstream oss;
    oss << "mds " << laggy
	<< ((laggy.size() > 1) ? " are":" is")
	<< " laggy";
    summary.push_back(make_pair(HEALTH_WARN, oss.str()));
  }

  if (get_max_mds() > 1 &&
      was_snaps_ever_allowed() && !allows_multimds_snaps()) {
    std::ostringstream oss;
    oss << "multi-active mds while there are snapshots possibly created by pre-mimic MDS";
    summary.push_back(make_pair(HEALTH_WARN, oss.str()));
  }
}

void MDSMap::get_health_checks(health_check_map_t *checks) const
{
  // MDS_DAMAGE
  if (!damaged.empty()) {
    health_check_t& check = checks->get_or_add("MDS_DAMAGE", HEALTH_ERR,
					       "%num% mds daemon%plurals% damaged",
					       damaged.size());
    for (auto p : damaged) {
      std::ostringstream oss;
      oss << "fs " << fs_name << " mds." << p << " is damaged";
      check.detail.push_back(oss.str());
    }
  }

  // FS_DEGRADED
  if (is_degraded()) {
    health_check_t& fscheck = checks->get_or_add(
      "FS_DEGRADED", HEALTH_WARN,
      "%num% filesystem%plurals% %isorare% degraded", 1);
    ostringstream ss;
    ss << "fs " << fs_name << " is degraded";
    fscheck.detail.push_back(ss.str());

    list<string> detail;
    for (mds_rank_t i = mds_rank_t(0); i< get_max_mds(); i++) {
      if (!is_up(i))
	continue;
      mds_gid_t gid = up.find(i)->second;
      const auto& info = mds_info.at(gid);
      stringstream ss;
      ss << "fs " << fs_name << " mds." << info.name << " at "
	 << info.addrs << " rank " << i;
      if (is_resolve(i))
	ss << " is resolving";
      if (is_replay(i))
	ss << " is replaying journal";
      if (is_rejoin(i))
	ss << " is rejoining";
      if (is_reconnect(i))
	ss << " is reconnecting to clients";
      if (ss.str().length())
	detail.push_back(ss.str());
    }
  }

  // MDS_UP_LESS_THAN_MAX
  if ((mds_rank_t)get_num_in_mds() < get_max_mds()) {
    health_check_t& check = checks->add(
      "MDS_UP_LESS_THAN_MAX", HEALTH_WARN,
      "%num% filesystem%plurals% %isorare% online with fewer MDS than max_mds", 1);
    stringstream ss;
    ss << "fs " << fs_name << " has " << get_num_in_mds()
       << " MDS online, but wants " << get_max_mds();
    check.detail.push_back(ss.str());
  }

  // MDS_ALL_DOWN
  if ((mds_rank_t)get_num_up_mds() == 0 && get_max_mds() > 0) {
    health_check_t &check = checks->add(
      "MDS_ALL_DOWN", HEALTH_ERR,
      "%num% filesystem%plurals% %isorare% offline", 1);
    stringstream ss;
    ss << "fs " << fs_name << " is offline because no MDS is active for it.";
    check.detail.push_back(ss.str());
  }

  if (get_max_mds() > 1 &&
      was_snaps_ever_allowed() && !allows_multimds_snaps()) {
    health_check_t &check = checks->add(
      "MULTIMDS_WITH_OLDSNAPS", HEALTH_ERR,
      "%num% filesystem%plurals% %isorare% multi-active mds with old snapshots", 1);
    stringstream ss;
    ss << "multi-active mds while there are snapshots possibly created by pre-mimic MDS";
    check.detail.push_back(ss.str());
  }

  if (get_inline_data_enabled()) {
    health_check_t &check = checks->add(
      "FS_INLINE_DATA_DEPRECATED", HEALTH_WARN,
      "%num% filesystem%plurals% with deprecated feature inline_data", 1);
    stringstream ss;
    ss << "fs " << fs_name << " has deprecated feature inline_data enabled.";
    check.detail.push_back(ss.str());
  }
}

void MDSMap::add_rank_node_to_consistent_hash_ring(mds_rank_t rank)
{
  uint32_t hash = rjhash32(rank);
  std::vector<mds_rank_node>::iterator iter;
  int index, next_index;
  /* Insertion into a sorted Vector - Best possible way would be to find 
   * position of element greater than the hash via binary search and insert there */
  iter = std::upper_bound( mds_rank_nodes.begin(), mds_rank_nodes.end(), hash,
         [](uint32_t hash_val, const mds_rank_node &rank_hash1) -> bool
         {
           return hash_val < rank_hash1.hash;
         });

  iter = mds_rank_nodes.emplace(iter, mds_rank_node{rank, rjhash32(rank)});
  
  index = iter - mds_rank_nodes.begin();

  /* Now redistribute the inodes hashed in the region between the newly inserted rank
   * and its predecessor. These inodes (before insertion) were mapped to the 
   * successor of the newly inserted rank */
  struct mds_rank_node last_hash = mds_rank_nodes.back();

  /* Check if the newly inserted rank is the last element in the consistent hash ring.
   * If yes, then the successor/next_index wraps around and should be the 
   * first element i.e index 0. */
  if (hash == last_hash.hash)
    next_index = 0;
  else
    next_index = index + 1;
  std::vector<inodeno_t>& inode_nos = mds_rank_nodes[next_index].inode_nos;
  std::vector<inodeno_t>::iterator it = inode_nos.begin();
  while(it != inode_nos.end()) 
  {
    if( rjhash32(*it) < hash ) {
      mds_rank_nodes[index].inode_nos.push_back(*it);
      it = inode_nos.erase(it);
    }
    else {
      ++it;
    }
  }
}

MDSMap::mds_rank_node MDSMap::get_rank_node_from_ino_in_consistent_hash_ring(inodeno_t ino)
{
  uint32_t hash = rjhash32(ino);

  /* Do a binary search to figure out the first hash greater than or equal to the key hash*/
  /* This can be done using std::lower_bound() */
  std::vector<mds_rank_node>::iterator iter;
  int index;
  iter = std::lower_bound(mds_rank_nodes.begin(), mds_rank_nodes.end(), hash,
         [](const mds_rank_node &rank_hash1, uint32_t hash_val) -> bool
         {
           return rank_hash1.hash < hash_val;
         });
  /* If key hash is greater than last rank i.e lower bound returned end, then wrap to the first rank hash */
  if (iter == mds_rank_nodes.end())
    index = 0;
  else
    index = iter - mds_rank_nodes.begin();

  return mds_rank_nodes[index];
}

mds_rank_t MDSMap::put_ino_in_consistent_hash_ring(inodeno_t ino)
{
  uint32_t hash = rjhash32(ino);
  /* Do a binary search to figure out the first hash greater than or equal to the key hash*/
  /* This can be done using std::lower_bound() */
  std::vector<mds_rank_node>::iterator iter;
  int index;
  iter = std::lower_bound(mds_rank_nodes.begin(), mds_rank_nodes.end(), hash,
         [](const mds_rank_node &rank_hash1, uint32_t hash_val) -> bool 
         {
           return rank_hash1.hash < hash_val;
         });
  /* If key hash is greater than last rank i.e lower bound returned end, then wrap to the first rank hash */
  if (iter == mds_rank_nodes.end()) 
    index = 0; 
  else 
    index = iter - mds_rank_nodes.begin();

  mds_rank_nodes[index].inode_nos.push_back(ino);
  return mds_rank_nodes[index].rank;
}

vector<inodeno_t> MDSMap::get_inos_from_rank_in_consistent_hash_ring(mds_rank_t rank)
{
  std::vector<mds_rank_node>::iterator iter;
  uint32_t hash = rjhash32(rank);

  /* Do a binary search to get the index of the rank that needs to be removed */
  iter = std::lower_bound(mds_rank_nodes.begin(), mds_rank_nodes.end(), hash,
         [](const mds_rank_node &rank_hash1, uint32_t hash_val) -> bool 
         {
           return rank_hash1.hash < hash_val;
         });

  if (iter != mds_rank_nodes.end() && !(hash < (*iter).hash))
    return  mds_rank_nodes[index].inode_nos;
  else
    std::ostringstream oss;
    oss << "rank not hashed in consistent hash ring";
    return;
}

mds_rank_t MDSMap::get_successor_of_rank_in_consistent_hash_ring(mds_rank rank)
{
  uint32_t hash = rjhash32(rank);
  std::vector<mds_rank_node>::iterator iter;
  struct mds_rank_node last_hash = mds_rank_nodes.back();

  if (hash == last_hash.hash)
    return mds_rank_node.begin()
  else
    iter = std::upper_bound( mds_rank_nodes.begin(), mds_rank_nodes.end(), hash,
           [](uint32_t hash_val, const mds_rank_node &rank_hash1) -> bool
           {
             return hash_val < rank_hash1.hash;
           });

    return *iter.rank;
}

void MDSMap::remove_rank_node_from_consistent_hash_ring(mds_rank_t rank)
{
  std::vector<mds_rank_node>::iterator iter;
  uint32_t hash = rjhash32(rank);

  /* Do a binary search to get the index of the rank that needs to be removed */
  iter = std::lower_bound(mds_rank_nodes.begin(), mds_rank_nodes.end(), hash,
         [](const mds_rank_node &rank_hash1, uint32_t hash_val) -> bool
         {
           return rank_hash1.hash < hash_val;
         });

  if (iter != mds_rank_nodes.end() && !(hash < (*iter).hash)) {
    int index = iter - mds_rank_nodes.begin(), next_index;
    struct mds_rank_node last_hash = mds_rank_nodes.back();
    if(hash == last_hash.hash)
      next_index = 0;
    else
      next_index = index + 1;
    
    std::vector<inodeno_t>& inode_nos = mds_rank_nodes[index].inode_nos;
    std::vector<inodeno_t>::iterator it = inode_nos.begin();
    while(it != inode_nos.end())
    {
        mds_rank_nodes[next_index].inode_nos.push_back(*it);
	++it;
    }

    mds_rank_nodes.erase(iter);
  }
  else {
    std::ostringstream oss;
    oss << "rank not hashed in consistent hash ring";
    return;
  }
}
void MDSMap::mds_info_t::encode_versioned(bufferlist& bl, uint64_t features) const
{
  __u8 v = 9;
  if (!HAVE_FEATURE(features, SERVER_NAUTILUS)) {
    v = 7;
  }
  ENCODE_START(v, 4, bl);
  encode(global_id, bl);
  encode(name, bl);
  encode(rank, bl);
  encode(inc, bl);
  encode((int32_t)state, bl);
  encode(state_seq, bl);
  if (v < 8) {
    encode(addrs.legacy_addr(), bl, features);
  } else {
    encode(addrs, bl, features);
  }
  encode(laggy_since, bl);
  encode(MDS_RANK_NONE, bl); /* standby_for_rank */
  encode(std::string(), bl); /* standby_for_name */
  encode(export_targets, bl);
  encode(mds_features, bl);
  encode(FS_CLUSTER_ID_NONE, bl); /* standby_for_fscid */
  encode(false, bl);
  if (v >= 9) {
    encode(flags, bl);
  }
  ENCODE_FINISH(bl);
}

void MDSMap::mds_info_t::encode_unversioned(bufferlist& bl) const
{
  __u8 struct_v = 3;
  using ceph::encode;
  encode(struct_v, bl);
  encode(global_id, bl);
  encode(name, bl);
  encode(rank, bl);
  encode(inc, bl);
  encode((int32_t)state, bl);
  encode(state_seq, bl);
  encode(addrs.legacy_addr(), bl, 0);
  encode(laggy_since, bl);
  encode(MDS_RANK_NONE, bl);
  encode(std::string(), bl);
  encode(export_targets, bl);
}

void MDSMap::mds_info_t::decode(bufferlist::const_iterator& bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(9, 4, 4, bl);
  decode(global_id, bl);
  decode(name, bl);
  decode(rank, bl);
  decode(inc, bl);
  decode((int32_t&)(state), bl);
  decode(state_seq, bl);
  decode(addrs, bl);
  decode(laggy_since, bl);
  {
    mds_rank_t standby_for_rank;
    decode(standby_for_rank, bl);
  }
  {
    std::string standby_for_name;
    decode(standby_for_name, bl);
  }
  if (struct_v >= 2)
    decode(export_targets, bl);
  if (struct_v >= 5)
    decode(mds_features, bl);
  if (struct_v >= 6) {
    fs_cluster_id_t standby_for_fscid;
    decode(standby_for_fscid, bl);
  }
  if (struct_v >= 7) {
    bool standby_replay;
    decode(standby_replay, bl);
  }
  if (struct_v >= 9) {
    decode(flags, bl);
  }
  DECODE_FINISH(bl);
}

std::string MDSMap::mds_info_t::human_name() const
{
  // Like "daemon mds.myhost restarted", "Activating daemon mds.myhost"
  std::ostringstream out;
  out << "daemon mds." << name;
  return out.str();
}

void MDSMap::encode(bufferlist& bl, uint64_t features) const
{
  std::map<mds_rank_t,int32_t> inc;  // Legacy field, fake it so that
                                     // old-mon peers have something sane
                                     // during upgrade
  for (const auto rank : in) {
    inc.insert(std::make_pair(rank, epoch));
  }

  using ceph::encode;
  if ((features & CEPH_FEATURE_PGID64) == 0) {
    __u16 v = 2;
    encode(v, bl);
    encode(epoch, bl);
    encode(flags, bl);
    encode(last_failure, bl);
    encode(root, bl);
    encode(session_timeout, bl);
    encode(session_autoclose, bl);
    encode(max_file_size, bl);
    encode(max_mds, bl);
    __u32 n = mds_info.size();
    encode(n, bl);
    for (map<mds_gid_t, mds_info_t>::const_iterator i = mds_info.begin();
	i != mds_info.end(); ++i) {
      encode(i->first, bl);
      encode(i->second, bl, features);
    }
    n = data_pools.size();
    encode(n, bl);
    for (const auto p: data_pools) {
      n = p;
      encode(n, bl);
    }

    int32_t m = cas_pool;
    encode(m, bl);
    return;
  } else if ((features & CEPH_FEATURE_MDSENC) == 0) {
    __u16 v = 3;
    encode(v, bl);
    encode(epoch, bl);
    encode(flags, bl);
    encode(last_failure, bl);
    encode(root, bl);
    encode(session_timeout, bl);
    encode(session_autoclose, bl);
    encode(max_file_size, bl);
    encode(max_mds, bl);
    __u32 n = mds_info.size();
    encode(n, bl);
    for (map<mds_gid_t, mds_info_t>::const_iterator i = mds_info.begin();
	i != mds_info.end(); ++i) {
      encode(i->first, bl);
      encode(i->second, bl, features);
    }
    encode(data_pools, bl);
    encode(cas_pool, bl);

    __u16 ev = 5;
    encode(ev, bl);
    encode(compat, bl);
    encode(metadata_pool, bl);
    encode(created, bl);
    encode(modified, bl);
    encode(tableserver, bl);
    encode(in, bl);
    encode(inc, bl);
    encode(up, bl);
    encode(failed, bl);
    encode(stopped, bl);
    encode(last_failure_osd_epoch, bl);
    return;
  }

  ENCODE_START(5, 4, bl);
  encode(epoch, bl);
  encode(flags, bl);
  encode(last_failure, bl);
  encode(root, bl);
  encode(session_timeout, bl);
  encode(session_autoclose, bl);
  encode(max_file_size, bl);
  encode(max_mds, bl);
  encode(mds_info, bl, features);
  encode(data_pools, bl);
  encode(cas_pool, bl);

  __u16 ev = 15;
  encode(ev, bl);
  encode(compat, bl);
  encode(metadata_pool, bl);
  encode(created, bl);
  encode(modified, bl);
  encode(tableserver, bl);
  encode(in, bl);
  encode(inc, bl);
  encode(up, bl);
  encode(failed, bl);
  encode(stopped, bl);
  encode(last_failure_osd_epoch, bl);
  encode(ever_allowed_features, bl);
  encode(explicitly_allowed_features, bl);
  encode(inline_data_enabled, bl);
  encode(enabled, bl);
  encode(fs_name, bl);
  encode(damaged, bl);
  encode(balancer, bl);
  encode(standby_count_wanted, bl);
  encode(old_max_mds, bl);
  encode(min_compat_client, bl);
  ENCODE_FINISH(bl);
}

void MDSMap::sanitize(const std::function<bool(int64_t pool)>& pool_exists)
{
  /* Before we did stricter checking, it was possible to remove a data pool
   * without also deleting it from the MDSMap. Check for that here after
   * decoding the data pools.
   */

  for (auto it = data_pools.begin(); it != data_pools.end();) {
    if (!pool_exists(*it)) {
      dout(0) << "removed non-existant data pool " << *it << " from MDSMap" << dendl;
      it = data_pools.erase(it);
    } else {
      it++;
    }
  }
}

void MDSMap::decode(bufferlist::const_iterator& p)
{
  std::map<mds_rank_t,int32_t> inc;  // Legacy field, parse and drop

  cached_up_features = 0;
  DECODE_START_LEGACY_COMPAT_LEN_16(5, 4, 4, p);
  decode(epoch, p);
  decode(flags, p);
  decode(last_failure, p);
  decode(root, p);
  decode(session_timeout, p);
  decode(session_autoclose, p);
  decode(max_file_size, p);
  decode(max_mds, p);
  decode(mds_info, p);
  if (struct_v < 3) {
    __u32 n;
    decode(n, p);
    while (n--) {
      __u32 m;
      decode(m, p);
      data_pools.push_back(m);
    }
    __s32 s;
    decode(s, p);
    cas_pool = s;
  } else {
    decode(data_pools, p);
    decode(cas_pool, p);
  }

  // kclient ignores everything from here
  __u16 ev = 1;
  if (struct_v >= 2)
    decode(ev, p);
  if (ev >= 3)
    decode(compat, p);
  else
    compat = get_compat_set_base();
  if (ev < 5) {
    __u32 n;
    decode(n, p);
    metadata_pool = n;
  } else {
    decode(metadata_pool, p);
  }
  decode(created, p);
  decode(modified, p);
  decode(tableserver, p);
  decode(in, p);
  decode(inc, p);
  decode(up, p);
  decode(failed, p);
  decode(stopped, p);
  if (ev >= 4)
    decode(last_failure_osd_epoch, p);
  if (ev >= 6) {
    if (ev < 10) {
      // previously this was a bool about snaps, not a flag map
      bool flag;
      decode(flag, p);
      ever_allowed_features = flag ? CEPH_MDSMAP_ALLOW_SNAPS : 0;
      decode(flag, p);
      explicitly_allowed_features = flag ? CEPH_MDSMAP_ALLOW_SNAPS : 0;
    } else {
      decode(ever_allowed_features, p);
      decode(explicitly_allowed_features, p);
    }
  } else {
    ever_allowed_features = 0;
    explicitly_allowed_features = 0;
  }
  if (ev >= 7)
    decode(inline_data_enabled, p);

  if (ev >= 8) {
    ceph_assert(struct_v >= 5);
    decode(enabled, p);
    decode(fs_name, p);
  } else {
    if (epoch > 1) {
      // If an MDS has ever been started, epoch will be greater than 1,
      // assume filesystem is enabled.
      enabled = true;
    } else {
      // Upgrading from a cluster that never used an MDS, switch off
      // filesystem until it's explicitly enabled.
      enabled = false;
    }
  }

  if (ev >= 9) {
    decode(damaged, p);
  }

  if (ev >= 11) {
    decode(balancer, p);
  }

  if (ev >= 12) {
    decode(standby_count_wanted, p);
  }

  if (ev >= 13) {
    decode(old_max_mds, p);
  }

  if (ev == 14) {
    int8_t r;
    decode(r, p);
    if (r < 0) {
      min_compat_client = ceph_release_t::unknown;
    } else {
      min_compat_client = ceph_release_t{static_cast<uint8_t>(r)};
    }
  } else if (ev > 14) {
    decode(min_compat_client, p);
  }

  DECODE_FINISH(p);
}

MDSMap::availability_t MDSMap::is_cluster_available() const
{
  if (epoch == 0) {
    // If I'm a client, this means I'm looking at an MDSMap instance
    // that was never actually initialized from the mons.  Client should
    // wait.
    return TRANSIENT_UNAVAILABLE;
  }

  // If a rank is marked damage (unavailable until operator intervenes)
  if (damaged.size()) {
    return STUCK_UNAVAILABLE;
  }

  // If no ranks are created (filesystem not initialized)
  if (in.empty()) {
    return STUCK_UNAVAILABLE;
  }

  for (const auto rank : in) {
    if (up.count(rank) && mds_info.at(up.at(rank)).laggy()) {
      // This might only be transient, but because we can't see
      // standbys, we have no way of knowing whether there is a
      // standby available to replace the laggy guy.
      return STUCK_UNAVAILABLE;
    }
  }

  if (get_num_mds(CEPH_MDS_STATE_ACTIVE) > 0) {
    // Nobody looks stuck, so indicate to client they should go ahead
    // and try mounting if anybody is active.  This may include e.g.
    // one MDS failing over and another active: the client should
    // proceed to start talking to the active one and let the
    // transiently-unavailable guy catch up later.
    return AVAILABLE;
  } else {
    // Nothing indicating we were stuck, but nobody active (yet)
    //return TRANSIENT_UNAVAILABLE;

    // Because we don't have standbys in the MDSMap any more, we can't
    // reliably indicate transient vs. stuck, so always say stuck so
    // that the client doesn't block.
    return STUCK_UNAVAILABLE;
  }
}

bool MDSMap::state_transition_valid(DaemonState prev, DaemonState next)
{
  bool state_valid = true;
  if (next != prev) {
    if (prev == MDSMap::STATE_REPLAY) {
      if (next != MDSMap::STATE_RESOLVE && next != MDSMap::STATE_RECONNECT) {
        state_valid = false;
      }
    } else if (prev == MDSMap::STATE_REJOIN) {
      if (next != MDSMap::STATE_ACTIVE &&
	  next != MDSMap::STATE_CLIENTREPLAY &&
	  next != MDSMap::STATE_STOPPED) {
        state_valid = false;
      }
    } else if (prev >= MDSMap::STATE_RESOLVE && prev < MDSMap::STATE_ACTIVE) {
      // Once I have entered replay, the only allowable transitions are to
      // the next next along in the sequence.
      if (next != prev + 1) {
        state_valid = false;
      }
    }
  }

  return state_valid;
}

bool MDSMap::check_health(mds_rank_t standby_daemon_count)
{
  std::set<mds_rank_t> standbys;
  get_standby_replay_mds_set(standbys);
  std::set<mds_rank_t> actives;
  get_active_mds_set(actives);
  mds_rank_t standbys_avail = (mds_rank_t)standbys.size()+standby_daemon_count;

  /* If there are standby daemons available/replaying and
   * standby_count_wanted is unset (default), then we set it to 1. This will
   * happen during health checks by the mons. Also, during initial creation
   * of the FS we will have no actives so we don't want to change the default
   * yet.
   */
  if (standby_count_wanted == -1 && actives.size() > 0 && standbys_avail > 0) {
    set_standby_count_wanted(1);
    return true;
  }
  return false;
}

mds_gid_t MDSMap::find_mds_gid_by_name(std::string_view s) const {
  for (const auto& [gid, info] : mds_info) {
    if (info.name == s) {
      return gid;
    }
  }
  return MDS_GID_NONE;
}

unsigned MDSMap::get_num_mds(int state) const {
  unsigned n = 0;
  for (std::map<mds_gid_t,mds_info_t>::const_iterator p = mds_info.begin();
       p != mds_info.end();
       ++p)
    if (p->second.state == state) ++n;
  return n;
}

void MDSMap::get_up_mds_set(std::set<mds_rank_t>& s) const {
  for (std::map<mds_rank_t, mds_gid_t>::const_iterator p = up.begin();
       p != up.end();
       ++p)
    s.insert(p->first);
}

uint64_t MDSMap::get_up_features() {
  if (!cached_up_features) {
    bool first = true;
    for (std::map<mds_rank_t, mds_gid_t>::const_iterator p = up.begin();
         p != up.end();
         ++p) {
      std::map<mds_gid_t, mds_info_t>::const_iterator q =
        mds_info.find(p->second);
      ceph_assert(q != mds_info.end());
      if (first) {
        cached_up_features = q->second.mds_features;
        first = false;
      } else {
        cached_up_features &= q->second.mds_features;
      }
    }
  }
  return cached_up_features;
}

void MDSMap::get_recovery_mds_set(std::set<mds_rank_t>& s) const {
  s = failed;
  for (const auto& p : damaged)
    s.insert(p);
  for (const auto& p : mds_info)
    if (p.second.state >= STATE_REPLAY && p.second.state <= STATE_STOPPING)
      s.insert(p.second.rank);
}

void MDSMap::get_mds_set_lower_bound(std::set<mds_rank_t>& s, DaemonState first) const {
  for (std::map<mds_gid_t, mds_info_t>::const_iterator p = mds_info.begin();
       p != mds_info.end();
       ++p)
    if (p->second.state >= first && p->second.state <= STATE_STOPPING)
      s.insert(p->second.rank);
}

void MDSMap::get_mds_set(std::set<mds_rank_t>& s, DaemonState state) const {
  for (std::map<mds_gid_t, mds_info_t>::const_iterator p = mds_info.begin();
       p != mds_info.end();
       ++p)
    if (p->second.state == state)
      s.insert(p->second.rank);
}

mds_gid_t MDSMap::get_standby_replay(mds_rank_t r) const {
  for (auto& [gid,info] : mds_info) {
    if (info.rank == r && info.state == STATE_STANDBY_REPLAY) {
      return gid;
    }
  }
  return MDS_GID_NONE;
}

bool MDSMap::is_degraded() const {
  if (!failed.empty() || !damaged.empty())
    return true;
  for (const auto& p : mds_info) {
    if (p.second.is_degraded())
      return true;
  }
  return false;
}
