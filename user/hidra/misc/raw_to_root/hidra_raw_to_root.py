#!/usr/bin/env python3
"""
Convert HIDRA merged binary files into the decoded ROOT ntuple layout.

The input format is the binary stream written by user/hidra/common/src/EventSerializer.cc.
The output mimics the active tree produced by HidraRootEventWriter:

  tree name: hidra
  scalar metadata branches: run, event, event_time, time_span, spill_number,
    detector_mask, trigger_mask, event_flags, n_detectors
  vector<double> branches: payload_bytes, ADCs, ADCFlags, TDCs, TDCFlags,
    FERStsamp_us, FERSrel_tsamp_us, FERStrigger_id, FERSboard_id, FERShg,
    FERSlg, FERStoa, FERStot, TrackerX, TrackerY

The decoder classes below intentionally mirror the C++ decoder names and keep
detector-specific logic isolated from the container parser and ROOT writer.
"""

from __future__ import annotations

import argparse
import configparser
import logging
import os
import sys
import struct
import time
from array import array
from dataclasses import dataclass
from typing import Dict, Iterable, List, Mapping, MutableMapping, Optional, Sequence, Tuple


EVENT_MARKER = 0xB0BF
EVENT_HEADER_END_MARKER = 0xBBBB
EVENT_TRAILER = 0xD04E
DETECTOR_EVENT_MARKER = 0xDEDE
DETECTOR_EVENT_END_MARKER = 0xDDDD

EVENT_HEADER_SIZE = 65
EVENT_TRAILER_SIZE = 2
DETECTOR_HEADER_SIZE = 31
MAX_DETECTORS = 8
SUPPORTED_DATA_VERSIONS = {11}

DEFAULT_VME_CRATE = "2:V792,4:V792,6:V792,8:V792,10:V792,12:V862,14:V775N"

VMESPEC = {
    "V792": {"nchannels": 32, "is_qdc": True},
    "V792N": {"nchannels": 16, "is_qdc": True},
    "V862": {"nchannels": 32, "is_qdc": True},
    "V775": {"nchannels": 32, "is_qdc": False},
    "V775N": {"nchannels": 16, "is_qdc": False},
}

ROOT_VECTOR_BRANCHES = [
    "payload_bytes",
    "ADCs",
    "ADCFlags",
    "TDCs",
    "TDCFlags",
    "FERStsamp_us",
    "FERSrel_tsamp_us",
    "FERStrigger_id",
    "FERSboard_id",
    "FERShg",
    "FERSlg",
    "FERStoa",
    "FERStot",
    "TrackerX",
    "TrackerY",
]


class HidraFormatError(RuntimeError):
    """Raised when the HIDRA binary container is malformed."""


@dataclass(frozen=True)
class DetectorPayload:
    det_id: int
    trigger_n: int
    spill_number: int
    event_time_begin: int
    native_event_time_begin: int
    trigger_mask: int
    endianness: int
    payload: bytes


@dataclass(frozen=True)
class HidraEvent:
    file_offset: int
    data_version: int
    event_size: int
    run_number: int
    event_number: int
    spill_number: int
    event_time: int
    time_span: int
    detector_mask: int
    trigger_mask: int
    event_flags: int
    detector_sizes: Tuple[int, ...]
    detectors: Tuple[DetectorPayload, ...]


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def parse_config_map(value: str) -> Dict[int, str]:
    out: Dict[int, str] = {}
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        key, sep, item = token.partition(":")
        if not sep:
            raise ValueError(f"bad map token {token!r}; expected key:value")
        out[int(key.strip())] = item.strip()
    return out


def read_conf_value(path: str, key: str) -> Optional[str]:
    """Read an EUDAQ-style key from a config file, ignoring section syntax issues."""

    if not path:
        return None
    with open(path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            lhs, rhs = line.split("=", 1)
            if lhs.strip() == key:
                return os.path.expandvars(rhs.strip())

    # Fall back to configparser for unusual files that still parse cleanly.
    parser = configparser.ConfigParser()
    parser.optionxform = str
    parser.read(path)
    for section in parser.sections():
        if parser.has_option(section, key):
            return os.path.expandvars(parser.get(section, key))
    return None


def active_detector_ids(mask: int) -> List[int]:
    return [det_id for det_id in range(MAX_DETECTORS) if mask & (1 << det_id)]


def find_detector_payload_end(record: bytes, payload_start: int, event_trailer_offset: int) -> int:
    marker = struct.pack("<H", DETECTOR_EVENT_END_MARKER)
    pos = payload_start
    while True:
        candidate = record.find(marker, pos, event_trailer_offset)
        if candidate < 0:
            raise HidraFormatError("detector end marker 0xDDDD not found")

        next_offset = candidate + 2
        if next_offset == event_trailer_offset:
            return candidate
        if next_offset + 2 <= event_trailer_offset and u16(record, next_offset) == DETECTOR_EVENT_MARKER:
            return candidate
        pos = candidate + 1


def parse_event_record(record: bytes, file_offset: int, strict: bool) -> HidraEvent:
    if len(record) < EVENT_HEADER_SIZE + EVENT_TRAILER_SIZE:
        raise HidraFormatError(f"short event at file offset {file_offset}")
    if u16(record, 0) != EVENT_MARKER:
        raise HidraFormatError(f"bad event marker at file offset {file_offset}")

    data_version = record[2]
    header_size = u32(record, 3)
    trailer_size = u32(record, 7)
    event_size = u32(record, 11)
    run_number = u16(record, 15)
    event_number = u32(record, 17)
    spill_number = u32(record, 21)
    event_time = u64(record, 25)
    trigger_mask = record[33]
    reserved32 = u32(record, 34)
    event_flags = u32(record, 38)
    time_span = u32(record, 42)
    detector_mask = record[46]
    detector_sizes = tuple(u16(record, 47 + 2 * det_id) for det_id in range(MAX_DETECTORS))
    header_end_marker = u16(record, 63)

    warnings: List[str] = []
    if data_version not in SUPPORTED_DATA_VERSIONS:
        warnings.append(f"data version {data_version} is not in {sorted(SUPPORTED_DATA_VERSIONS)}")
    if header_size != EVENT_HEADER_SIZE:
        warnings.append(f"header size {header_size} != {EVENT_HEADER_SIZE}")
    if trailer_size != EVENT_TRAILER_SIZE:
        warnings.append(f"trailer size {trailer_size} != {EVENT_TRAILER_SIZE}")
    if event_size != len(record):
        warnings.append(f"event size {event_size} != record length {len(record)}")
    if reserved32 != 0:
        warnings.append(f"reserved32 is non-zero: 0x{reserved32:08x}")
    if header_end_marker != EVENT_HEADER_END_MARKER:
        warnings.append(f"bad header end marker 0x{header_end_marker:04x}")
    if warnings:
        message = f"event {event_number} at offset {file_offset}: " + "; ".join(warnings)
        if strict:
            raise HidraFormatError(message)
        logging.warning(message)

    event_trailer_offset = event_size - trailer_size
    if event_trailer_offset < header_size or u16(record, event_trailer_offset) != EVENT_TRAILER:
        raise HidraFormatError(f"event {event_number}: bad event trailer")

    detectors: List[DetectorPayload] = []
    pos = header_size
    while pos < event_trailer_offset:
        if pos + DETECTOR_HEADER_SIZE > event_trailer_offset:
            raise HidraFormatError(f"event {event_number}: truncated detector header")
        if u16(record, pos) != DETECTOR_EVENT_MARKER:
            raise HidraFormatError(f"event {event_number}: bad detector marker at record offset {pos}")

        det_id = record[pos + 2]
        det_trigger_n = u32(record, pos + 3)
        det_spill_number = u32(record, pos + 7)
        det_event_time = u64(record, pos + 11)
        native_event_time = u64(record, pos + 19)
        det_trigger_mask = record[pos + 29]
        endianness = record[pos + 30]
        payload_start = pos + DETECTOR_HEADER_SIZE

        expected_size = detector_sizes[det_id] if det_id < len(detector_sizes) else 0
        if expected_size:
            payload_end = pos + expected_size - 2
            if payload_end < payload_start or payload_end + 2 > event_trailer_offset:
                raise HidraFormatError(f"event {event_number}: invalid size table entry for detector {det_id}")
            if u16(record, payload_end) != DETECTOR_EVENT_END_MARKER:
                payload_end = find_detector_payload_end(record, payload_start, event_trailer_offset)
        else:
            payload_end = find_detector_payload_end(record, payload_start, event_trailer_offset)

        detectors.append(
            DetectorPayload(
                det_id=det_id,
                trigger_n=det_trigger_n,
                spill_number=det_spill_number,
                event_time_begin=det_event_time,
                native_event_time_begin=native_event_time,
                trigger_mask=det_trigger_mask,
                endianness=endianness,
                payload=record[payload_start:payload_end],
            )
        )
        pos = payload_end + 2

    parsed_ids = {det.det_id for det in detectors}
    mask_ids = set(active_detector_ids(detector_mask))
    if parsed_ids != mask_ids:
        message = f"event {event_number}: detector mask {sorted(mask_ids)} != parsed detectors {sorted(parsed_ids)}"
        if strict:
            raise HidraFormatError(message)
        logging.warning(message)

    return HidraEvent(
        file_offset=file_offset,
        data_version=data_version,
        event_size=event_size,
        run_number=run_number,
        event_number=event_number,
        spill_number=spill_number,
        event_time=event_time,
        time_span=time_span,
        detector_mask=detector_mask,
        trigger_mask=trigger_mask,
        event_flags=event_flags,
        detector_sizes=detector_sizes,
        detectors=tuple(detectors),
    )


def iter_events(filename: str, strict: bool = False, max_events: int = 0) -> Iterable[HidraEvent]:
    with open(filename, "rb") as handle:
        count = 0
        while True:
            if max_events and count >= max_events:
                return
            file_offset = handle.tell()
            prefix = handle.read(15)
            if not prefix:
                return
            if len(prefix) != 15:
                raise HidraFormatError(f"truncated event prefix at file offset {file_offset}")
            if u16(prefix, 0) != EVENT_MARKER:
                raise HidraFormatError(f"bad event marker 0x{u16(prefix, 0):04x} at file offset {file_offset}")

            event_size = u32(prefix, 11)
            if event_size < EVENT_HEADER_SIZE + EVENT_TRAILER_SIZE:
                raise HidraFormatError(f"invalid event size {event_size} at file offset {file_offset}")
            rest = handle.read(event_size - len(prefix))
            if len(rest) != event_size - len(prefix):
                raise HidraFormatError(f"truncated event at file offset {file_offset}")

            yield parse_event_record(prefix + rest, file_offset, strict)
            count += 1


class RootPayloadDecoder:
    def matches(self, detector: DetectorPayload) -> bool:
        raise NotImplementedError

    def decode(self, detector: DetectorPayload, branches: MutableMapping[str, List[float]]) -> None:
        raise NotImplementedError

    @staticmethod
    def add(branches: MutableMapping[str, List[float]], name: str, values: Sequence[float]) -> None:
        branches.setdefault(name, []).extend(float(value) for value in values)


class HidraGenericPayloadDecoder(RootPayloadDecoder):
    def matches(self, detector: DetectorPayload) -> bool:
        return True

    def decode(self, detector: DetectorPayload, branches: MutableMapping[str, List[float]]) -> None:
        branches.setdefault("payload_bytes", []).append(float(len(detector.payload)))


def compute_channel_from_geo(vme_geo_map: Mapping[int, str], geo: int, channel: int, want_qdc: bool) -> int:
    channel_index = channel
    for module_geo, module_type in sorted(vme_geo_map.items()):
        if module_geo == geo:
            return channel_index
        spec = VMESPEC.get(module_type)
        if spec is None:
            logging.error("unknown VME module type %s at geo %s", module_type, module_geo)
            return channel
        if bool(spec["is_qdc"]) == want_qdc:
            channel_index += int(spec["nchannels"])
    logging.error("geo %s not found in VME map", geo)
    return channel


def compute_max_channels(vme_geo_map: Mapping[int, str], want_qdc: bool) -> int:
    if not vme_geo_map:
        logging.error("empty VME map; using C++ fallback size 1500")
        return 1500
    max_channel = 0
    for module_type in vme_geo_map.values():
        spec = VMESPEC.get(module_type)
        if spec is None:
            logging.error("unknown VME module type %s; using C++ fallback size 1500", module_type)
            return 1500
        if bool(spec["is_qdc"]) == want_qdc:
            max_channel += int(spec["nchannels"])
    return max_channel


class HidraXdcPayloadDecoder(RootPayloadDecoder):
    def __init__(self, vme_geo_map: Mapping[int, str], det_ids: Sequence[int]):
        self.vme_geo_map = dict(sorted(vme_geo_map.items()))
        self.det_ids = set(det_ids)
        self.n_adc_channels = compute_max_channels(self.vme_geo_map, want_qdc=True)
        self.n_tdc_channels = compute_max_channels(self.vme_geo_map, want_qdc=False)

    def matches(self, detector: DetectorPayload) -> bool:
        return detector.det_id in self.det_ids

    def decode(self, detector: DetectorPayload, branches: MutableMapping[str, List[float]]) -> None:
        HidraGenericPayloadDecoder().decode(detector, branches)
        payload = detector.payload
        if not payload:
            logging.error("event %s: XDC payload is empty", detector.trigger_n)
            return
        if len(payload) % 4 != 0:
            logging.error("event %s: XDC payload size %s is not a multiple of 4", detector.trigger_n, len(payload))
            return

        adc_values = [-1.0] * self.n_adc_channels
        adc_flags = [-1.0] * self.n_adc_channels
        tdc_values = [-1.0] * self.n_tdc_channels
        tdc_flags = [-1.0] * self.n_tdc_channels
        words = struct.unpack("<" + "I" * (len(payload) // 4), payload)

        iword = 0
        while iword < len(words):
            word = words[iword]
            if (word & 0xFE000000) == 0xFE000000:
                iword += 1
                continue

            header_type = (word >> 24) & 0x7
            geo = (word >> 27) & 0x1F
            crate = (word >> 16) & 0xFF
            nchan = (word >> 8) & 0x3F
            if header_type != 0b010:
                if header_type == 0b110 and self.vme_geo_map.get(geo) == "V775N":
                    iword += 1
                    continue
                logging.error(
                    "event %s: geo %s unexpected XDC header word 0x%08x type %s",
                    detector.trigger_n,
                    geo,
                    word,
                    header_type,
                )
                return

            module_type = self.vme_geo_map.get(geo)
            if module_type is None:
                logging.error("event %s: no XDC module configured for crate %s geo %s", detector.trigger_n, crate, geo)
                return

            for _ in range(nchan):
                iword += 1
                if iword >= len(words):
                    logging.error("event %s: truncated XDC channel words", detector.trigger_n)
                    return
                channel_word = words[iword]
                word_type = (channel_word >> 24) & 0x7
                channel_geo = (channel_word >> 27) & 0x1F
                if word_type != 0:
                    logging.error("event %s: geo %s unexpected XDC channel word 0x%08x", detector.trigger_n, geo, channel_word)
                    return
                if channel_geo != geo:
                    logging.error("event %s: XDC geo mismatch header %s channel %s", detector.trigger_n, geo, channel_geo)
                    return

                value = channel_word & 0xFFF
                ov = (channel_word >> 12) & 0x1
                un = (channel_word >> 13) & 0x1
                if module_type in {"V792", "V792N", "V862"}:
                    module_channel = ((channel_word >> 17) & 0xF) if module_type == "V792N" else ((channel_word >> 16) & 0x1F)
                    index = compute_channel_from_geo(self.vme_geo_map, geo, module_channel, want_qdc=True)
                    if 0 <= index < len(adc_values):
                        adc_values[index] = float(value)
                        adc_flags[index] = float((ov << 1) | un)
                elif module_type in {"V775", "V775N"}:
                    vd = (channel_word >> 14) & 0x1
                    module_channel = ((channel_word >> 17) & 0xF) if module_type == "V775N" else ((channel_word >> 16) & 0x1F)
                    index = compute_channel_from_geo(self.vme_geo_map, geo, module_channel, want_qdc=False)
                    if 0 <= index < len(tdc_values):
                        tdc_values[index] = float(value)
                        tdc_flags[index] = float((ov << 2) | (un << 1) | vd)
                else:
                    logging.error("event %s: unknown XDC module type %s", detector.trigger_n, module_type)
                    return

            iword += 1
            if iword >= len(words):
                logging.error("event %s: missing XDC trailer", detector.trigger_n)
                return
            trailer_word = words[iword]
            trailer_type = (trailer_word >> 24) & 0x7
            trailer_geo = (trailer_word >> 27) & 0x1F
            trailer_event = trailer_word & 0xFFFFFF
            if trailer_type != 0b100:
                logging.error("event %s: geo %s unexpected XDC trailer word 0x%08x", detector.trigger_n, geo, trailer_word)
                return
            if trailer_event != detector.trigger_n:
                logging.warning(
                    "event %s: geo %s XDC trailer event count %s does not match trigger",
                    detector.trigger_n,
                    trailer_geo,
                    trailer_event,
                )
            iword += 1

        self.add(branches, "ADCs", adc_values)
        self.add(branches, "ADCFlags", adc_flags)
        self.add(branches, "TDCs", tdc_values)
        self.add(branches, "TDCFlags", tdc_flags)


class HidraFersPayloadDecoder(RootPayloadDecoder):
    PACKET = struct.Struct("<HHIBddQQQQQ64H64H64I64H")
    N_CHANNELS_MAX = 64 * 20
    ALL_CHANNELS_MASK = (1 << 64) - 1

    def __init__(self, det_ids: Sequence[int]):
        self.det_ids = set(det_ids)

    def matches(self, detector: DetectorPayload) -> bool:
        return detector.det_id in self.det_ids

    def decode(self, detector: DetectorPayload, branches: MutableMapping[str, List[float]]) -> None:
        HidraGenericPayloadDecoder().decode(detector, branches)
        payload = detector.payload
        packet_size = self.PACKET.size
        if len(payload) % packet_size != 0:
            logging.error(
                "event %s: unexpected FERS payload size %s, expected a multiple of %s",
                detector.trigger_n,
                len(payload),
                packet_size,
            )

        tsamp = [-1.0] * self.N_CHANNELS_MAX
        rel_tsamp = [-1.0] * self.N_CHANNELS_MAX
        trigger_id = [-1.0] * self.N_CHANNELS_MAX
        board_id_values = [-1.0] * self.N_CHANNELS_MAX
        hg = [-1.0] * self.N_CHANNELS_MAX
        lg = [-1.0] * self.N_CHANNELS_MAX
        toa = [-1.0] * self.N_CHANNELS_MAX
        tot = [-1.0] * self.N_CHANNELS_MAX

        nboards = len(payload) // packet_size
        for iboard in range(nboards):
            block = self.PACKET.unpack_from(payload, iboard * packet_size)
            marker = block[0]
            board_id = block[3]
            tstamp_us = block[4]
            rel_tstamp_us = block[5]
            trig_id = block[8]
            chmask = block[9]
            energy_hg = block[11:75]
            energy_lg = block[75:139]
            tstamp = block[139:203]
            time_over_threshold = block[203:267]

            if marker != 0xAAAA:
                logging.warning("event %s: FERS block %s marker is 0x%04x, expected 0xAAAA", detector.trigger_n, iboard, marker)
            if board_id >= 20:
                logging.error("event %s: FERS block %s has invalid board_id %s", detector.trigger_n, iboard, board_id)
                continue
            if chmask != self.ALL_CHANNELS_MASK:
                logging.warning("event %s: FERS block %s chmask is 0x%016x; skipping", detector.trigger_n, iboard, chmask)
                continue

            base = board_id * 64
            for ichan in range(64):
                index = base + ichan
                tsamp[index] = float(tstamp_us)
                rel_tsamp[index] = float(rel_tstamp_us)
                trigger_id[index] = float(trig_id)
                board_id_values[index] = float(board_id)
                hg[index] = float(energy_hg[ichan])
                lg[index] = float(energy_lg[ichan])
                toa[index] = float(tstamp[ichan])
                tot[index] = float(time_over_threshold[ichan])

        self.add(branches, "FERStsamp_us", tsamp)
        self.add(branches, "FERSrel_tsamp_us", rel_tsamp)
        self.add(branches, "FERStrigger_id", trigger_id)
        self.add(branches, "FERSboard_id", board_id_values)
        self.add(branches, "FERShg", hg)
        self.add(branches, "FERSlg", lg)
        self.add(branches, "FERStoa", toa)
        self.add(branches, "FERStot", tot)


class HidraTrackerPayloadDecoder(RootPayloadDecoder):
    def __init__(self, det_ids: Sequence[int]):
        self.det_ids = set(det_ids)

    def matches(self, detector: DetectorPayload) -> bool:
        return detector.det_id in self.det_ids

    def decode(self, detector: DetectorPayload, branches: MutableMapping[str, List[float]]) -> None:
        HidraGenericPayloadDecoder().decode(detector, branches)
        payload = detector.payload
        if not payload:
            logging.error("event %s: tracker payload is empty", detector.trigger_n)
            return
        if len(payload) % 8 != 0:
            logging.error("event %s: tracker payload size %s is not a multiple of 8", detector.trigger_n, len(payload))
            return
        values = struct.unpack("<" + "d" * (len(payload) // 8), payload)
        if len(values) % 2 != 0:
            logging.error("event %s: tracker payload has an odd number of values", detector.trigger_n)
            return
        self.add(branches, "TrackerX", values[0::2])
        self.add(branches, "TrackerY", values[1::2])


def decode_event(event: HidraEvent, decoders: Sequence[RootPayloadDecoder]) -> Dict[str, List[float]]:
    branches: Dict[str, List[float]] = {name: [] for name in ROOT_VECTOR_BRANCHES}
    for detector in event.detectors:
        for decoder in decoders:
            if decoder.matches(detector):
                decoder.decode(detector, branches)
                break
    return branches


def jagged_float64(values: Sequence[Sequence[float]]) -> ak.Array:
    import awkward as ak

    # Plain nested lists let uproot choose a conventional jagged double layout
    # that reads back as vector-like data without object-array ambiguity.
    return ak.Array([[float(value) for value in item] for item in values])


def root_branch_types() -> Dict[str, str]:
    branch_types = {
        "run": "int32",
        "event": "uint32",
        "event_time": "uint64",
        "time_span": "uint32",
        "spill_number": "uint32",
        "detector_mask": "uint8",
        "trigger_mask": "uint8",
        "event_flags": "uint32",
        "n_detectors": "int32",
    }
    branch_types.update({branch: "var * float64" for branch in ROOT_VECTOR_BRANCHES})
    return branch_types


def make_root_arrays(events: Sequence[HidraEvent], decoded: Sequence[Mapping[str, Sequence[float]]]) -> Dict[str, object]:
    import numpy as np

    arrays = {
        "run": np.asarray([event.run_number for event in events], dtype=np.int32),
        "event": np.asarray([event.event_number for event in events], dtype=np.uint32),
        "event_time": np.asarray([event.event_time for event in events], dtype=np.uint64),
        "time_span": np.asarray([event.time_span for event in events], dtype=np.uint32),
        "spill_number": np.asarray([event.spill_number for event in events], dtype=np.uint32),
        "detector_mask": np.asarray([event.detector_mask for event in events], dtype=np.uint8),
        "trigger_mask": np.asarray([event.trigger_mask for event in events], dtype=np.uint8),
        "event_flags": np.asarray([event.event_flags for event in events], dtype=np.uint32),
        "n_detectors": np.asarray([len(event.detectors) for event in events], dtype=np.int32),
    }
    for branch in ROOT_VECTOR_BRANCHES:
        arrays[branch] = jagged_float64([entry.get(branch, []) for entry in decoded])
    return arrays


class UprootBatchWriter:
    """Fallback writer: stores vector-like branches as double[] plus counters."""

    def __init__(self, output_file: str):
        self.output_file = output_file
        self.root_file = None
        self.tree = None
        self.written_events = 0

    def __enter__(self) -> "UprootBatchWriter":
        import uproot

        self.root_file = uproot.recreate(self.output_file)
        self.root_file.mktree("hidra", root_branch_types(), title="HIDRA live decoded quantities")
        self.tree = self.root_file["hidra"]
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if self.root_file is not None:
            self.root_file.close()

    def extend(self, events: Sequence[HidraEvent], decoded: Sequence[Mapping[str, Sequence[float]]]) -> None:
        if not events:
            return
        assert self.tree is not None
        self.tree.extend(make_root_arrays(events, decoded))
        self.written_events += len(events)


class PyRootBatchWriter:
    """Writer matching HidraRootEventWriter's scalar and std::vector<double> branches."""

    def __init__(self, output_file: str):
        self.output_file = output_file
        self.root_file = None
        self.tree = None
        self.written_events = 0
        self.scalars = {
            "run": array("i", [0]),
            "event": array("I", [0]),
            "event_time": array("Q", [0]),
            "time_span": array("I", [0]),
            "spill_number": array("I", [0]),
            "detector_mask": array("B", [0]),
            "trigger_mask": array("B", [0]),
            "event_flags": array("I", [0]),
            "n_detectors": array("i", [0]),
        }
        self.vectors = {}

    def __enter__(self) -> "PyRootBatchWriter":
        try:
            import ROOT
        except ImportError as exc:
            raise RuntimeError(
                "PyROOT is required for --writer root because it is the only supported backend here "
                "that writes real std::vector<double> branches. Source/install ROOT, or use "
                "--writer uproot for the non-identical double[] fallback."
            ) from exc

        self.root_file = ROOT.TFile(self.output_file, "RECREATE")
        if not self.root_file or self.root_file.IsZombie():
            raise RuntimeError(f"cannot open ROOT output file: {self.output_file}")
        self.tree = ROOT.TTree("hidra", "HIDRA live decoded quantities")

        self.tree.Branch("run", self.scalars["run"], "run/I")
        self.tree.Branch("event", self.scalars["event"], "event/i")
        self.tree.Branch("event_time", self.scalars["event_time"], "event_time/l")
        self.tree.Branch("time_span", self.scalars["time_span"], "time_span/i")
        self.tree.Branch("spill_number", self.scalars["spill_number"], "spill_number/i")
        self.tree.Branch("detector_mask", self.scalars["detector_mask"], "detector_mask/b")
        self.tree.Branch("trigger_mask", self.scalars["trigger_mask"], "trigger_mask/b")
        self.tree.Branch("event_flags", self.scalars["event_flags"], "event_flags/i")
        self.tree.Branch("n_detectors", self.scalars["n_detectors"], "n_detectors/I")

        vector_type = ROOT.std.vector("double")
        for name in ROOT_VECTOR_BRANCHES:
            self.vectors[name] = vector_type()
            self.tree.Branch(name, self.vectors[name])
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if self.root_file is None:
            return
        if exc_type is None and self.tree is not None:
            self.root_file.cd()
            self.tree.Write("", 2)  # TObject::kOverwrite
        self.root_file.Close()

    def extend(self, events: Sequence[HidraEvent], decoded: Sequence[Mapping[str, Sequence[float]]]) -> None:
        if not events:
            return
        assert self.tree is not None
        for event, branches in zip(events, decoded):
            self.scalars["run"][0] = int(event.run_number)
            self.scalars["event"][0] = int(event.event_number)
            self.scalars["event_time"][0] = int(event.event_time)
            self.scalars["time_span"][0] = int(event.time_span)
            self.scalars["spill_number"][0] = int(event.spill_number)
            self.scalars["detector_mask"][0] = int(event.detector_mask)
            self.scalars["trigger_mask"][0] = int(event.trigger_mask)
            self.scalars["event_flags"][0] = int(event.event_flags)
            self.scalars["n_detectors"][0] = int(len(event.detectors))

            for name, vector in self.vectors.items():
                vector.clear()
                for value in branches.get(name, []):
                    vector.push_back(float(value))
            self.tree.Fill()
            self.written_events += 1


def make_batch_writer(output_file: str, writer: str):
    if writer == "root":
        return PyRootBatchWriter(output_file)
    if writer == "uproot":
        return UprootBatchWriter(output_file)
    raise ValueError(f"unknown writer backend: {writer}")


class ProgressReporter:
    def __init__(self, input_file: str, max_events: int, interval: int, enabled: bool):
        self.input_file = input_file
        self.max_events = max_events
        self.interval = max(1, interval)
        self.enabled = enabled
        self.file_size = os.path.getsize(input_file) if os.path.exists(input_file) else 0
        self.start_time = time.monotonic()
        self.last_report_events = 0
        self.last_report_time = self.start_time

    def maybe_report(self, event: HidraEvent, written_events: int, force: bool = False) -> None:
        if not self.enabled:
            return
        now = time.monotonic()
        due_by_events = written_events - self.last_report_events >= self.interval
        due_by_time = now - self.last_report_time >= 10.0
        if not force and not due_by_events and not due_by_time:
            return

        elapsed = max(now - self.start_time, 1e-9)
        rate = written_events / elapsed
        read_bytes = min(event.file_offset + event.event_size, self.file_size) if self.file_size else 0
        parts = [
            f"events={written_events}",
            f"rate={rate:.1f} ev/s",
            f"elapsed={format_duration(elapsed)}",
        ]

        if self.file_size:
            fraction = min(read_bytes / self.file_size, 1.0)
            parts.append(f"input={format_bytes(read_bytes)}/{format_bytes(self.file_size)} ({100.0 * fraction:.1f}%)")
            if rate > 0 and fraction > 0:
                eta_seconds = elapsed * (1.0 / fraction - 1.0)
                parts.append(f"eta={format_duration(eta_seconds)}")
        elif self.max_events:
            fraction = min(written_events / self.max_events, 1.0)
            parts.append(f"target={written_events}/{self.max_events} ({100.0 * fraction:.1f}%)")
            if rate > 0:
                parts.append(f"eta={format_duration((self.max_events - written_events) / rate)}")

        print("[hidra_raw_to_root] " + " | ".join(parts), file=sys.stderr, flush=True)
        self.last_report_events = written_events
        self.last_report_time = now


def format_duration(seconds: float) -> str:
    seconds = max(0, int(seconds))
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    if hours:
        return f"{hours:d}h{minutes:02d}m{seconds:02d}s"
    if minutes:
        return f"{minutes:d}m{seconds:02d}s"
    return f"{seconds:d}s"


def format_bytes(nbytes: int) -> str:
    if nbytes < 1_000_000:
        return f"{nbytes / 1_000:.1f} kB"
    return f"{nbytes / 1_000_000:.1f} MB"


def convert_file(input_file: str, output_file: str, decoders: Sequence[RootPayloadDecoder],
                 strict: bool, max_events: int, chunk_size: int,
                 progress_interval: int, progress: bool, writer_backend: str) -> int:
    chunk_size = max(1, chunk_size)
    events: List[HidraEvent] = []
    decoded: List[Mapping[str, Sequence[float]]] = []
    reporter = ProgressReporter(input_file, max_events=max_events, interval=progress_interval, enabled=progress)
    last_event: Optional[HidraEvent] = None

    with make_batch_writer(output_file, writer_backend) as writer:
        for event in iter_events(input_file, strict=strict, max_events=max_events):
            last_event = event
            events.append(event)
            decoded.append(decode_event(event, decoders))
            if len(events) >= chunk_size:
                writer.extend(events, decoded)
                reporter.maybe_report(event, writer.written_events)
                events.clear()
                decoded.clear()

        writer.extend(events, decoded)
        if last_event is not None and reporter.last_report_events != writer.written_events:
            reporter.maybe_report(last_event, writer.written_events, force=True)
        return writer.written_events


def parse_id_list(value: str) -> Tuple[int, ...]:
    if not value.strip():
        return tuple()
    return tuple(int(item.strip()) for item in value.split(",") if item.strip())


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="HIDRA merged binary input file")
    parser.add_argument("output", help="decoded ROOT output file")
    parser.add_argument("--config", help="EUDAQ/HIDRA .conf file; used for VME_CRATE_1 when --vme-crate is omitted")
    parser.add_argument("--vme-crate", help=f"VME geo map, e.g. {DEFAULT_VME_CRATE!r}")
    parser.add_argument("--xdc-detids", default="1,6", help="comma-separated detector IDs decoded as XDC")
    parser.add_argument("--fers-detids", default="2", help="comma-separated detector IDs decoded as FERS")
    parser.add_argument("--tracker-detids", default="3", help="comma-separated detector IDs decoded as tracker")
    parser.add_argument("--max-events", type=int, default=0, help="stop after N events, useful for checks")
    parser.add_argument("--chunk-size", type=int, default=100, help="events decoded and written per ROOT extend call")
    parser.add_argument(
        "--writer",
        choices=("root", "uproot"),
        default="root",
        help="ROOT output backend: root writes real std::vector<double>; uproot writes double[] fallback",
    )
    parser.add_argument("--progress-interval", type=int, default=1000, help="print progress every N written events")
    parser.add_argument("--quiet", action="store_true", help="disable progress messages")
    parser.add_argument("--strict", action="store_true", help="turn container warnings into errors")
    parser.add_argument("-v", "--verbose", action="store_true", help="enable info-level logging")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING, format="%(levelname)s: %(message)s")

    vme_crate = args.vme_crate
    if vme_crate is None and args.config:
        vme_crate = read_conf_value(args.config, "VME_CRATE_1")
    if vme_crate is None:
        vme_crate = DEFAULT_VME_CRATE

    vme_geo_map = parse_config_map(vme_crate)
    decoders: List[RootPayloadDecoder] = [
        HidraXdcPayloadDecoder(vme_geo_map, parse_id_list(args.xdc_detids)),
        HidraFersPayloadDecoder(parse_id_list(args.fers_detids)),
        HidraTrackerPayloadDecoder(parse_id_list(args.tracker_detids)),
        HidraGenericPayloadDecoder(),
    ]

    written_events = convert_file(
        args.input,
        args.output,
        decoders,
        strict=args.strict,
        max_events=args.max_events,
        chunk_size=args.chunk_size,
        progress_interval=args.progress_interval,
        progress=not args.quiet,
        writer_backend=args.writer,
    )
    if written_events == 0:
        raise HidraFormatError(f"no events found in {args.input}")

    logging.info("wrote %s events to %s", written_events, args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, HidraFormatError) as exc:
        logging.error("%s", exc)
        raise SystemExit(1)
