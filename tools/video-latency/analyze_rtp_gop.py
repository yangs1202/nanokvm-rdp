#!/usr/bin/env python3
"""Summarize raw RTP/H.264 GOP evidence from a classic pcap capture.

The agent currently increments its RTP timestamp by a fixed 9000 for every
encoded access unit. This tool therefore reports RTP timestamp deltas and wall
arrival cadence separately; it never converts the RTP timestamp increment into
FPS. It also excludes a capture-boundary AU without an RTP marker from the
complete-AU GOP interval calculation.
"""

import argparse
import json
import statistics
import struct
from collections import Counter
from pathlib import Path


def pcap_endian(data):
    if data[:4] == bytes.fromhex('d4c3b2a1'):
        return '<'
    if data[:4] == bytes.fromhex('a1b2c3d4'):
        return '>'
    raise ValueError('unsupported pcap magic')


def iter_rtp_packets(data):
    endian = pcap_endian(data)
    _, _, _, _, snaplen, network = struct.unpack_from(endian + 'HHiiii', data, 4)
    if network != 1:
        raise ValueError(f'only Ethernet pcap is supported, linktype={network}')
    offset = 24
    while offset + 16 <= len(data):
        seconds, micros, captured, _ = struct.unpack_from(endian + 'IIII', data, offset)
        offset += 16
        frame = data[offset:offset + captured]
        offset += captured
        if len(frame) < 34:
            continue
        ethertype = struct.unpack_from('>H', frame, 12)[0]
        l3 = 14
        if ethertype == 0x8100 and len(frame) >= 38:
            ethertype = struct.unpack_from('>H', frame, 16)[0]
            l3 = 18
        if ethertype != 0x0800:
            continue
        ihl = (frame[l3] & 0x0F) * 4
        if frame[l3 + 9] != 17:
            continue
        udp = l3 + ihl
        if len(frame) < udp + 8:
            continue
        _, destination, length, _ = struct.unpack_from('>HHHH', frame, udp)
        if destination != 5005:
            continue
        payload = frame[udp + 8:udp + length]
        if len(payload) < 12 or payload[0] >> 6 != 2:
            continue
        csrc_count = payload[0] & 0x0F
        has_extension = bool(payload[0] & 0x10)
        has_padding = bool(payload[0] & 0x20)
        marker = bool(payload[1] & 0x80)
        sequence, timestamp, ssrc = struct.unpack_from('>HII', payload, 2)
        payload_offset = 12 + 4 * csrc_count
        if has_extension:
            if len(payload) < payload_offset + 4:
                continue
            _, extension_words = struct.unpack_from('>HH', payload, payload_offset)
            payload_offset += 4 + 4 * extension_words
        payload_end = len(payload)
        if has_padding and payload_end:
            payload_end -= payload[-1]
        rtp_payload = payload[payload_offset:payload_end]
        if not rtp_payload:
            continue
        yield {
            'wall': seconds + micros / 1_000_000,
            'sequence': sequence,
            'timestamp': timestamp,
            'ssrc': ssrc,
            'marker': marker,
            'payload': rtp_payload,
        }


def inspect_nal_payload(payload):
    """Return NAL types and FU boundary events for one RTP payload.

    ``marker=1`` is only an RTP packet boundary hint.  A fragmented NAL can
    start before the pcap and still end in a marker packet, so formal GOP
    acceptance also needs explicit FU start/end validation.
    """
    indicator = payload[0] & 0x1F
    if 1 <= indicator <= 23:
        return {'types': [indicator], 'fu_events': [], 'malformed': False}
    if indicator == 24:  # STAP-A
        types = []
        offset = 1
        malformed = False
        while offset + 2 <= len(payload):
            size = struct.unpack_from('>H', payload, offset)[0]
            offset += 2
            if size == 0 or offset + size > len(payload):
                malformed = True
                break
            types.append(payload[offset] & 0x1F)
            offset += size
        if offset != len(payload):
            malformed = True
        return {'types': types, 'fu_events': [], 'malformed': malformed or not types}
    if indicator in (28, 29) and len(payload) >= 2:  # FU-A/FU-B
        fu_header = payload[1]
        return {
            'types': [fu_header & 0x1F],
            'fu_events': [{'start': bool(fu_header & 0x80),
                           'end': bool(fu_header & 0x40),
                           'nal_type': fu_header & 0x1F}],
            'malformed': indicator == 29 and len(payload) < 4,
        }
    return {'types': [indicator], 'fu_events': [], 'malformed': True}


def nal_types(payload):
    """Compatibility helper used by ad-hoc callers."""
    return inspect_nal_payload(payload)['types']


def update_access_unit(access_unit, packet):
    details = inspect_nal_payload(packet['payload'])
    access_unit['packets'] += 1
    access_unit['markers'] += int(packet['marker'])
    access_unit['last_sequence'] = packet['sequence']
    access_unit['nal_types'].extend(details['types'])
    access_unit['malformed_payload'] |= details['malformed']
    for event in details['fu_events']:
        if event['start']:
            if access_unit['fu_active']:
                access_unit['fu_boundary_errors'].append('nested_start')
            access_unit['fu_active'] = True
        elif not access_unit['fu_active']:
            access_unit['fu_boundary_errors'].append('missing_start')
        if event['end']:
            if not access_unit['fu_active']:
                access_unit['fu_boundary_errors'].append('end_without_start')
            access_unit['fu_active'] = False
    access_unit['wall_last'] = packet['wall']


def finalize_access_unit(access_unit, boundary):
    access_unit['capture_boundary'] = boundary
    if access_unit['fu_active']:
        access_unit['fu_boundary_errors'].append('missing_end')
    access_unit['fu_balanced'] = not access_unit['fu_boundary_errors']
    access_unit['has_vcl'] = any(nal_type in (1, 5) for nal_type in access_unit['nal_types'])
    access_unit['marker_complete'] = access_unit['markers'] == 1
    access_unit['valid_picture_access_unit'] = (
        not boundary and access_unit['marker_complete'] and
        access_unit['fu_balanced'] and not access_unit['malformed_payload'] and
        access_unit['has_vcl']
    )


def summarize(path, start_unix=None, end_unix=None):
    data = Path(path).read_bytes()
    packets = list(iter_rtp_packets(data))
    if not packets:
        raise ValueError('no RTP packets for UDP destination port 5005')
    if start_unix is not None or end_unix is not None:
        packets = [
            packet for packet in packets
            if (start_unix is None or packet['wall'] >= start_unix) and
               (end_unix is None or packet['wall'] <= end_unix)
        ]
        if not packets:
            raise ValueError('no RTP packets within requested wall-time interval')

    sequence_deltas = [
        (right['sequence'] - left['sequence']) % 65536
        for left, right in zip(packets, packets[1:])
    ]
    missing = sum(max(0, delta - 1) for delta in sequence_deltas if delta <= 32768)

    access_units = []
    for packet in packets:
        if not access_units or access_units[-1]['timestamp'] != packet['timestamp']:
            access_units.append({
                'timestamp': packet['timestamp'],
                'packets': 0,
                'markers': 0,
                'first_sequence': packet['sequence'],
                'last_sequence': packet['sequence'],
                'nal_types': [],
                'wall_first': packet['wall'],
                'wall_last': packet['wall'],
                'malformed_payload': False,
                'fu_active': False,
                'fu_boundary_errors': [],
            })
        update_access_unit(access_units[-1], packet)
    for index, access_unit in enumerate(access_units):
        finalize_access_unit(
            access_unit,
            boundary='first' if index == 0 else 'last' if index == len(access_units) - 1 else None,
        )

    marker_complete = [item for item in access_units if item['marker_complete']]
    valid_picture = [item for item in access_units if item['valid_picture_access_unit']]
    idr_indices = [
        index for index, item in enumerate(access_units)
        if item['valid_picture_access_unit'] and 5 in item['nal_types']
    ]
    timestamp_deltas = [
        (right['timestamp'] - left['timestamp']) % (2 ** 32)
        for left, right in zip(access_units, access_units[1:])
    ]
    arrival_intervals = [
        right['wall_last'] - left['wall_last']
        for left, right in zip(valid_picture, valid_picture[1:])
    ]
    sequence_contiguous = len(packets) > 1 and all(delta == 1 for delta in sequence_deltas)
    ssrcs = sorted({packet['ssrc'] for packet in packets})
    timestamp_reappears = len(access_units) - len({item['timestamp'] for item in access_units})
    interior = access_units[1:-1]
    rejected_interior = [item for item in interior if not item['valid_picture_access_unit']]
    rejection_reasons = []
    if len(ssrcs) != 1:
        rejection_reasons.append('multiple_ssrc')
    if not sequence_contiguous:
        rejection_reasons.append('sequence_not_contiguous')
    if timestamp_reappears:
        rejection_reasons.append('timestamp_reappears')
    if len(access_units) < 3:
        rejection_reasons.append('capture_has_no_interior_access_units')
    if rejected_interior:
        rejection_reasons.append('interior_access_unit_boundary_or_payload_invalid')
    if not valid_picture:
        rejection_reasons.append('no_valid_picture_access_units')
    transport_integrity_valid = not any(reason in rejection_reasons for reason in (
        'multiple_ssrc', 'sequence_not_contiguous', 'timestamp_reappears'))
    gop_structurally_valid = not any(reason in rejection_reasons for reason in (
        'capture_has_no_interior_access_units',
        'interior_access_unit_boundary_or_payload_invalid',
        'no_valid_picture_access_units'))
    gop_measurable = len(idr_indices) >= 2 and all(
        interval >= 1 for interval in (
            right - left for left, right in zip(idr_indices, idr_indices[1:]))
    )
    if not gop_measurable:
        rejection_reasons.append('insufficient_idr_samples_for_gop_measurement')
    valid_for_gop = transport_integrity_valid and gop_structurally_valid and gop_measurable
    return {
        'pcap': str(path),
        'pcap_bytes': len(data),
        'wall_filter_unix': {'start': start_unix, 'end': end_unix},
        'udp_packets': len(packets),
        'ssrcs': ssrcs,
        'wall_start_unix': packets[0]['wall'],
        'wall_end_unix': packets[-1]['wall'],
        'wall_span_seconds': packets[-1]['wall'] - packets[0]['wall'],
        'sequence_first': packets[0]['sequence'],
        'sequence_last': packets[-1]['sequence'],
        'sequence_delta_counts': dict(Counter(sequence_deltas)),
        'sequence_missing_estimate': missing,
        'sequence_duplicate_edges': sum(delta == 0 for delta in sequence_deltas),
        'sequence_reorder_edges': sum(delta > 32768 for delta in sequence_deltas),
        'access_unit_runs': len(access_units),
        'complete_access_units_marker_1': len(marker_complete),
        'valid_picture_access_units': len(valid_picture),
        'boundary_access_units_excluded': sum(item['capture_boundary'] is not None for item in access_units),
        'boundary_incomplete_access_units': sum(not item['marker_complete'] for item in access_units),
        'invalid_interior_access_units': len(rejected_interior),
        'marker_count_distribution': dict(Counter(item['markers'] for item in access_units)),
        'timestamp_reappears_after_new_timestamp': timestamp_reappears,
        'rtp_timestamp_delta_counts': dict(Counter(timestamp_deltas)),
        'arrival_au_rate_hz': (len(valid_picture) / (valid_picture[-1]['wall_last'] - valid_picture[0]['wall_first'])
                               if len(valid_picture) > 1 else None),
        'arrival_interval_ms': {
            'min': min(arrival_intervals) * 1000 if arrival_intervals else None,
            'median': statistics.median(arrival_intervals) * 1000 if arrival_intervals else None,
            'max': max(arrival_intervals) * 1000 if arrival_intervals else None,
        },
        'idr_complete_access_unit_count': len(idr_indices),
        'idr_access_unit_indices': idr_indices,
        'idr_interval_access_units': [
            right - left for left, right in zip(idr_indices, idr_indices[1:])
        ],
        'nal_or_fu_original_type_counts': dict(Counter(
            nal_type for item in access_units for nal_type in item['nal_types'])),
        'fps_interpretation': 'RTP timestamp deltas are not converted to FPS; report wall arrival cadence separately because the agent uses fixed +9000 timestamp increments.',
        'gop_validation': {
            'valid_for_gop': valid_for_gop,
            'rejection_reasons': rejection_reasons,
            'transport_integrity_valid': transport_integrity_valid,
            'gop_structurally_valid': gop_structurally_valid,
            'gop_measurable': gop_measurable,
            'minimum_idr_samples_required': 2,
            'sequence_contiguous': sequence_contiguous,
            'single_ssrc': len(ssrcs) == 1,
            'all_interior_access_units_valid_picture': not rejected_interior,
            'boundary_access_units_excluded': True,
            'fu_start_end_balanced_for_interior': all(item['fu_balanced'] for item in interior),
            'vcl_present_for_interior': all(item['has_vcl'] for item in interior),
            'sps_pps_only_excluded_from_idr': True,
        },
        'transport_integrity_valid': transport_integrity_valid,
        'gop_measurable': gop_measurable,
        'valid_for_gop': valid_for_gop,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('pcap')
    parser.add_argument('--start-unix', type=float)
    parser.add_argument('--end-unix', type=float)
    args = parser.parse_args()
    print(json.dumps(summarize(args.pcap, args.start_unix, args.end_unix),
                     indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
