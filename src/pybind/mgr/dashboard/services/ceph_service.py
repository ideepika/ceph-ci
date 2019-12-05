# -*- coding: utf-8 -*-
from __future__ import absolute_import
import json
import logging
from six.moves import reduce

import rados

from mgr_module import CommandResult

try:
    from more_itertools import pairwise
except ImportError:
    def pairwise(iterable):
        from itertools import tee
        a, b = tee(iterable)
        next(b, None)
        return zip(a, b)

from .. import mgr

try:
    from typing import Dict, Any  # pylint: disable=unused-import
except ImportError:
    pass  # For typing only


logger = logging.getLogger('ceph_service')


class SendCommandError(rados.Error):
    def __init__(self, err, prefix, argdict, errno):
        self.prefix = prefix
        self.argdict = argdict
        super(SendCommandError, self).__init__(err, errno)


class CephService(object):

    OSD_FLAG_NO_SCRUB = 'noscrub'
    OSD_FLAG_NO_DEEP_SCRUB = 'nodeep-scrub'

    PG_STATUS_SCRUBBING = 'scrubbing'
    PG_STATUS_DEEP_SCRUBBING = 'deep'

    SCRUB_STATUS_DISABLED = 'Disabled'
    SCRUB_STATUS_ACTIVE = 'Active'
    SCRUB_STATUS_INACTIVE = 'Inactive'

    @classmethod
    def get_service_map(cls, service_name):
        service_map = {}  # type: Dict[str, Dict[str, Any]]
        for server in mgr.list_servers():
            for service in server['services']:
                if service['type'] == service_name:
                    if server['hostname'] not in service_map:
                        service_map[server['hostname']] = {
                            'server': server,
                            'services': []
                        }
                    inst_id = service['id']
                    metadata = mgr.get_metadata(service_name, inst_id)
                    status = mgr.get_daemon_status(service_name, inst_id)
                    service_map[server['hostname']]['services'].append({
                        'id': inst_id,
                        'type': service_name,
                        'hostname': server['hostname'],
                        'metadata': metadata,
                        'status': status
                    })
        return service_map

    @classmethod
    def get_service_list(cls, service_name):
        service_map = cls.get_service_map(service_name)
        return [svc for _, svcs in service_map.items() for svc in svcs['services']]

    @classmethod
    def get_service(cls, service_name, service_id):
        for server in mgr.list_servers():
            for service in server['services']:
                if service['type'] == service_name:
                    inst_id = service['id']
                    if inst_id == service_id:
                        metadata = mgr.get_metadata(service_name, inst_id)
                        status = mgr.get_daemon_status(service_name, inst_id)
                        return {
                            'id': inst_id,
                            'type': service_name,
                            'hostname': server['hostname'],
                            'metadata': metadata,
                            'status': status
                        }
        return None

    @classmethod
    def get_pool_list(cls, application=None):
        osd_map = mgr.get('osd_map')
        if not application:
            return osd_map['pools']
        return [pool for pool in osd_map['pools']
                if application in pool.get('application_metadata', {})]

    @classmethod
    def get_pool_list_with_stats(cls, application=None):
        # pylint: disable=too-many-locals
        pools = cls.get_pool_list(application)

        pools_w_stats = []

        pg_summary = mgr.get("pg_summary")
        pool_stats = mgr.get_updated_pool_stats()

        for pool in pools:
            pool['pg_status'] = pg_summary['by_pool'][pool['pool'].__str__()]
            stats = pool_stats[pool['pool']]
            s = {}

            def get_rate(series):
                if len(series) >= 2:
                    return differentiate(*list(series)[-2:])
                return 0

            for stat_name, stat_series in stats.items():
                s[stat_name] = {
                    'latest': stat_series[0][1],
                    'rate': get_rate(stat_series),
                    'rates': get_rates_from_data(stat_series)
                }
            pool['stats'] = s
            pools_w_stats.append(pool)
        return pools_w_stats

    @classmethod
    def get_pool_name_from_id(cls, pool_id):
        pool_list = cls.get_pool_list()
        for pool in pool_list:
            if pool['pool'] == pool_id:
                return pool['pool_name']
        return None

    @classmethod
    def send_command(cls, srv_type, prefix, srv_spec='', **kwargs):
        """
        :type prefix: str
        :param srv_type: mon |
        :param kwargs: will be added to argdict
        :param srv_spec: typically empty. or something like "<fs_id>:0"

        :raises PermissionError: See rados.make_ex
        :raises ObjectNotFound: See rados.make_ex
        :raises IOError: See rados.make_ex
        :raises NoSpace: See rados.make_ex
        :raises ObjectExists: See rados.make_ex
        :raises ObjectBusy: See rados.make_ex
        :raises NoData: See rados.make_ex
        :raises InterruptedOrTimeoutError: See rados.make_ex
        :raises TimedOut: See rados.make_ex
        :raises ValueError: return code != 0
        """
        argdict = {
            "prefix": prefix,
            "format": "json",
        }
        argdict.update({k: v for k, v in kwargs.items() if v is not None})
        result = CommandResult("")
        mgr.send_command(result, srv_type, srv_spec, json.dumps(argdict), "")
        r, outb, outs = result.wait()
        if r != 0:
            logger.error("send_command '%s' failed. (r=%s, outs=\"%s\", kwargs=%s)", prefix, r,
                         outs, kwargs)

            raise SendCommandError(outs, prefix, argdict, r)

        try:
            return json.loads(outb)
        except Exception:  # pylint: disable=broad-except
            return outb

    @staticmethod
    def get_smart_data_by_device(device):
        dev_id = device['devid']
        if 'daemons' in device and device['daemons']:
            daemons = [daemon for daemon in device['daemons'] if daemon.startswith('osd')]
            if daemons:
                svc_type, svc_id = daemons[0].split('.')
                dev_smart_data = CephService.send_command(svc_type, 'smart', svc_id, devid=dev_id)
                for dev_id, dev_data in dev_smart_data.items():
                    if 'error' in dev_data:
                        logger.warning(
                            '[SMART] error retrieving smartctl data for device ID "%s": %s', dev_id,
                            dev_data)
                return dev_smart_data
            logger.warning('[SMART] no OSD service found for device ID "%s"', dev_id)
            return {}
        logger.warning('[SMART] key "daemon" not found for device ID "%s"', dev_id)
        return {}

    @staticmethod
    def get_devices_by_host(hostname):
        # (str) -> dict
        return CephService.send_command('mon', 'device ls-by-host', host=hostname)

    @staticmethod
    def get_smart_data_by_host(hostname):
        # type: (str) -> dict
        return reduce(lambda a, b: a.update(b) or a, [
            CephService.get_smart_data_by_device(device)
            for device in CephService.get_devices_by_host(hostname)
        ], {})

    @staticmethod
    def get_smart_data_by_daemon(daemon_type, daemon_id):
        # type: (str, str) -> dict
        smart_data = CephService.send_command(daemon_type, 'smart', daemon_id)
        for _, dev_data in smart_data.items():
            if 'error' in dev_data:
                logger.warning('[SMART] Error retrieving smartctl data for daemon "%s.%s"',
                               daemon_type, daemon_id)
        return smart_data

    @classmethod
    def get_rates(cls, svc_type, svc_name, path):
        """
        :return: the derivative of mgr.get_counter()
        :rtype: list[tuple[int, float]]"""
        data = mgr.get_counter(svc_type, svc_name, path)[path]
        return get_rates_from_data(data)

    @classmethod
    def get_rate(cls, svc_type, svc_name, path):
        """returns most recent rate"""
        data = mgr.get_counter(svc_type, svc_name, path)[path]

        if data and len(data) > 1:
            return differentiate(*data[-2:])
        return 0.0

    @classmethod
    def get_client_perf(cls):
        pools_stats = mgr.get('osd_pool_stats')['pool_stats']

        io_stats = {
            'read_bytes_sec': 0,
            'read_op_per_sec': 0,
            'write_bytes_sec': 0,
            'write_op_per_sec': 0,
        }
        recovery_stats = {'recovering_bytes_per_sec': 0}

        for pool_stats in pools_stats:
            client_io = pool_stats['client_io_rate']
            for stat in list(io_stats.keys()):
                if stat in client_io:
                    io_stats[stat] += client_io[stat]

            client_recovery = pool_stats['recovery_rate']
            for stat in list(recovery_stats.keys()):
                if stat in client_recovery:
                    recovery_stats[stat] += client_recovery[stat]

        client_perf = io_stats.copy()
        client_perf.update(recovery_stats)

        return client_perf

    @classmethod
    def get_scrub_status(cls):
        enabled_flags = mgr.get('osd_map')['flags_set']
        if cls.OSD_FLAG_NO_SCRUB in enabled_flags or cls.OSD_FLAG_NO_DEEP_SCRUB in enabled_flags:
            return cls.SCRUB_STATUS_DISABLED

        grouped_pg_statuses = mgr.get('pg_summary')['all']
        for grouped_pg_status in grouped_pg_statuses.keys():
            if len(grouped_pg_status.split(cls.PG_STATUS_SCRUBBING)) > 1 \
                    or len(grouped_pg_status.split(cls.PG_STATUS_DEEP_SCRUBBING)) > 1:
                return cls.SCRUB_STATUS_ACTIVE

        return cls.SCRUB_STATUS_INACTIVE

    @classmethod
    def get_pg_info(cls):
        pg_summary = mgr.get('pg_summary')
        object_stats = {stat: pg_summary['pg_stats_sum']['stat_sum'][stat] for stat in [
            'num_objects', 'num_object_copies', 'num_objects_degraded',
            'num_objects_misplaced', 'num_objects_unfound']}

        pgs_per_osd = 0.0
        total_osds = len(pg_summary['by_osd'])
        if total_osds > 0:
            total_pgs = 0.0
            for _, osd_pg_statuses in pg_summary['by_osd'].items():
                for _, pg_amount in osd_pg_statuses.items():
                    total_pgs += pg_amount

            pgs_per_osd = total_pgs / total_osds

        return {
            'object_stats': object_stats,
            'statuses': pg_summary['all'],
            'pgs_per_osd': pgs_per_osd,
        }


def get_rates_from_data(data):
    """
    >>> get_rates_from_data([])
    [(0, 0.0)]
    >>> get_rates_from_data([[1, 42]])
    [(1, 0.0)]
    >>> get_rates_from_data([[0, 100], [2, 101], [3, 100], [4, 100]])
    [(2, 0.5), (3, 1.0), (4, 0.0)]
    """
    if not data:
        return [(0, 0.0)]
    if len(data) == 1:
        return [(data[0][0], 0.0)]
    return [(data2[0], differentiate(data1, data2)) for data1, data2 in pairwise(data)]


def differentiate(data1, data2):
    """
    >>> times = [0, 2]
    >>> values = [100, 101]
    >>> differentiate(*zip(times, values))
    0.5
    >>> times = [0, 2]
    >>> values = [100, 99]
    >>> differentiate(*zip(times, values))
    0.5
    """
    return abs((data2[1] - data1[1]) / float(data2[0] - data1[0]))
