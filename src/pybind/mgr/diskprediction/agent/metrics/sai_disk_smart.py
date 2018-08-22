from __future__ import absolute_import

import datetime
import json
import _strptime
import socket
import time

from . import AGENT_VERSION, MetricsAgent, MetricsField
from ...common.clusterdata import ClusterAPI


class SAIDiskSmartFields(MetricsField):
    """ SAI DiskSmart structure """
    measurement = 'sai_disk_smart'

    def __init__(self):
        super(SAIDiskSmartFields, self).__init__()
        self.fields['agenthost'] = None
        self.tags['agenthost_domain_id'] = None
        self.tags['disk_domain_id'] = None
        self.tags['disk_name'] = None
        self.tags['disk_wwn'] = None
        self.tags['primary_key'] = None
        self.fields['cluster_domain_id'] = None
        self.fields['host_domain_id'] = None
        self.fields['agent_version'] = str(AGENT_VERSION)


class SAIDiskSmartAgent(MetricsAgent):
    measurement = 'sai_disk_smart'

    def _collect_data(self):
        # process data and save to 'self.data'
        obj_api = ClusterAPI(self._module_inst)
        cluster_id = obj_api.get_cluster_id()
        osds = obj_api.get_osds()
        for osd in osds:
            if osd.get('osd') is None:
                continue
            if not osd.get('in'):
                continue
            osds_meta = obj_api.get_osd_metadata(osd.get('osd'))
            if not osds_meta:
                continue
            devs_info = obj_api.get_osd_device_id(osd.get('osd'))
            if devs_info:
                for dev_name, dev_info in devs_info.iteritems():
                    osds_smart = obj_api.get_device_health(dev_info['dev_id'])
                    if not osds_smart:
                        continue
                    # Always pass through last smart data record
                    o_key = sorted(osds_smart.iterkeys(), reverse=True)[0]
                    if o_key:
                        s_date = o_key
                        s_val = osds_smart[s_date]
                        smart_data = SAIDiskSmartFields()
                        smart_data.tags['disk_name'] = str(dev_name)
                        smart_data.fields['cluster_domain_id'] = str(cluster_id)
                        smart_data.tags['host_domain_id'] = \
                            str('%s_%s'
                                % (cluster_id, osds_meta.get('hostname', 'None')))
                        smart_data.fields['agenthost'] = str(socket.gethostname())
                        smart_data.tags['agenthost_domain_id'] = \
                            str('%s_%s' % (cluster_id, smart_data.fields['agenthost']))
                        # parse attributes
                        ata_smart = s_val.get('ata_smart_attributes', {})
                        for attr in ata_smart.get('table', []):
                            if attr.get('raw', {}).get('string'):
                                if str(attr.get('raw', {}).get('string', '0')).isdigit():
                                    smart_data.fields['%s_raw' % attr.get('id')] = \
                                        int(attr.get('raw', {}).get('string', '0'))
                                else:
                                    if str(attr.get('raw', {}).get('string', '0')).split(' ')[0].isdigit():
                                        smart_data.fields['%s_raw' % attr.get('id')] = \
                                            int(attr.get('raw', {}).get('string', '0').split(' ')[0])
                                    else:
                                        smart_data.fields['%s_raw' % attr.get('id')] = \
                                            attr.get('raw', {}).get('value', 0)
                        smart_data.fields['raw_data'] = str(json.dumps(osds_smart[s_date]).replace("\"", "\'"))
                        if s_val.get('temperature', {}).get('current') is not None:
                            smart_data.fields['CurrentDriveTemperature_raw'] = \
                                int(s_val['temperature']['current'])
                        if s_val.get('temperature', {}).get('drive_trip') is not None:
                            smart_data.fields['DriveTripTemperature_raw'] = \
                                int(s_val['temperature']['drive_trip'])
                        if s_val.get('elements_grown_list') is not None:
                            smart_data.fields['ElementsInGrownDefectList_raw'] = int(s_val['elements_grown_list'])
                        if s_val.get('hours_powered_up') is not None:
                            smart_data.fields['9_raw'] = int(round(float(s_val['hours_powered_up'])))
                        if s_val.get('percentage_used_endurance_indicator') is not None:
                            smart_data.fields['PercentageUsedEnduranceIndicator_raw'] = \
                                int(s_val['percentage_used_endurance_indicator'])
                        if s_val.get('error_counter_log') is not None:
                            s_err_counter = s_val['error_counter_log']
                            for s_key in s_err_counter.keys():
                                if s_key.lower() in ['read', 'write']:
                                    for s1_key in s_err_counter[s_key].keys():
                                        if s1_key.lower() == 'errors_corrected_by_eccfast':
                                            smart_data.fields['ErrorsCorrectedbyECCFast%s_raw' % s_key.capitalize()] = \
                                                int(s_err_counter[s_key]['errors_corrected_by_eccfast'])
                                        elif s1_key.lower() == 'errors_corrected_by_eccdelayed':
                                            smart_data.fields['ErrorsCorrectedbyECCDelayed%s_raw' % s_key.capitalize()] = \
                                                int(s_err_counter[s_key]['errors_corrected_by_eccdelayed'])
                                        elif s1_key.lower() == 'errors_corrected_by_rereads_rewrites':
                                            smart_data.fields['ErrorCorrectedByRereadsRewrites%s_raw' % s_key.capitalize()] = \
                                                int(s_err_counter[s_key]['errors_corrected_by_rereads_rewrites'])
                                        elif s1_key.lower() == 'total_errors_corrected':
                                            smart_data.fields['TotalErrorsCorrected%s_raw' % s_key.capitalize()] = \
                                                int(s_err_counter[s_key]['total_errors_corrected'])
                                        elif s1_key.lower() == 'correction_algorithm_invocations':
                                            smart_data.fields['CorrectionAlgorithmInvocations%s_raw' % s_key.capitalize()] = \
                                                int(s_err_counter[s_key]['correction_algorithm_invocations'])
                                        elif s1_key.lower() == 'gigabytes_processed':
                                            smart_data.fields['GigaBytesProcessed%s_raw' % s_key.capitalize()] = \
                                                float(s_err_counter[s_key]['gigabytes_processed'])
                                        elif s1_key.lower() == 'total_uncorrected_errors':
                                            smart_data.fields['TotalUncorrectedErrors%s_raw' % s_key.capitalize()] = \
                                                int(s_err_counter[s_key]['total_uncorrected_errors'])

                        serial_number = s_val.get('serial_number')
                        wwn = s_val.get('wwn', {})
                        wwpn = ''
                        if wwn:
                            wwpn = '%06X%X' % (wwn.get('oui', 0), wwn.get('id', 0))
                            for k in wwn.keys():
                                if k in ['naa', 't10', 'eui', 'iqn']:
                                    wwpn = ('%X%s' % (wwn[k], wwpn)).lower()
                                    break
                        if wwpn:
                            smart_data.tags['disk_domain_id'] = str(dev_info['dev_id'])
                            smart_data.tags['disk_wwn'] = str(wwpn)
                            if serial_number:
                                smart_data.fields['serial_number'] = str(serial_number)
                            else:
                                smart_data.fields['serial_number'] = str(wwpn)
                        elif serial_number:
                            smart_data.tags['disk_domain_id'] = str(dev_info['dev_id'])
                            smart_data.fields['serial_number'] = str(serial_number)
                            if wwpn:
                                smart_data.tags['disk_wwn'] = str(wwpn)
                            else:
                                smart_data.tags['disk_wwn'] = str(serial_number)
                        else:
                            smart_data.tags['disk_domain_id'] = str(dev_info['dev_id'])
                            smart_data.tags['disk_wwn'] = str(dev_name)
                            smart_data.fields['serial_number'] = str(dev_name)
                        smart_data.tags['primary_key'] = \
                            str('%s%s%s'
                                % (cluster_id,
                                   smart_data.tags['host_domain_id'],
                                   smart_data.tags['disk_domain_id']))
                        smart_data.timestamp = \
                            time.mktime(datetime.datetime.strptime(
                                s_date, '%Y%m%d-%H%M%S').timetuple())
                        self.data.append(smart_data)
