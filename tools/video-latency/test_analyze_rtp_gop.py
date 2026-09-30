#!/usr/bin/env python3
"""Regression tests for RTP GOP completeness gates."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import analyze_rtp_gop


def packet(seconds, micros, sequence, timestamp, payload, marker=True, ssrc=691):
    rtp = struct.pack('>BBHII', 0x80, (0x80 if marker else 0) | 96,
                      sequence, timestamp, ssrc) + payload
    udp = struct.pack('>HHHH', 40000, 5005, 8 + len(rtp), 0) + rtp
    ip = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0, 64,
                     17, 0, bytes((10, 0, 0, 1)), bytes((10, 0, 0, 2))) + udp
    frame = bytes(6) + bytes(6) + struct.pack('>H', 0x0800) + ip
    header = struct.pack('<IIII', seconds, micros, len(frame), len(frame))
    return header + frame


def write_pcap(packets):
    data = struct.pack('<IHHIIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
    data += b''.join(packets)
    handle = tempfile.NamedTemporaryFile(suffix='.pcap', delete=False)
    handle.write(data)
    handle.close()
    return Path(handle.name)


class RtpGopCompletenessTests(unittest.TestCase):
    def test_fu_end_without_start_is_not_a_valid_picture_au(self):
        path = write_pcap([
            packet(1, 0, 10, 100, bytes((0x7C, 0x05, 1)), marker=False),
            packet(1, 1000, 11, 100, bytes((0x7C, 0x45, 1)), marker=True),
        ])
        try:
            result = analyze_rtp_gop.summarize(path)
        finally:
            path.unlink()
        self.assertEqual(result['complete_access_units_marker_1'], 1)
        self.assertEqual(result['valid_picture_access_units'], 0)
        self.assertFalse(result['valid_for_gop'])

    def test_internal_sequence_gap_rejects_formal_gop(self):
        path = write_pcap([
            packet(1, 0, 10, 100, bytes((0x41,))),
            packet(1, 1000, 12, 200, bytes((0x41,))),
            packet(1, 2000, 13, 300, bytes((0x41,))),
            packet(1, 3000, 14, 400, bytes((0x41,))),
        ])
        try:
            result = analyze_rtp_gop.summarize(path)
        finally:
            path.unlink()
        self.assertEqual(result['sequence_missing_estimate'], 1)
        self.assertFalse(result['gop_validation']['sequence_contiguous'])
        self.assertIn('sequence_not_contiguous', result['gop_validation']['rejection_reasons'])
        self.assertFalse(result['valid_for_gop'])

    def test_p_only_stream_is_transport_valid_but_gop_not_measurable(self):
        path = write_pcap([
            packet(1, 0, 10, 100, bytes((0x41,))),
            packet(1, 1000, 11, 200, bytes((0x41,))),
            packet(1, 2000, 12, 300, bytes((0x41,))),
            packet(1, 3000, 13, 400, bytes((0x41,))),
        ])
        try:
            result = analyze_rtp_gop.summarize(path)
        finally:
            path.unlink()
        self.assertTrue(result['transport_integrity_valid'])
        self.assertFalse(result['gop_measurable'])
        self.assertFalse(result['valid_for_gop'])
        self.assertEqual(result['idr_interval_access_units'], [])
        self.assertIn('insufficient_idr_samples_for_gop_measurement',
                      result['gop_validation']['rejection_reasons'])


if __name__ == '__main__':
    unittest.main()
