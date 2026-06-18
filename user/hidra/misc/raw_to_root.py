#!/usr/bin/env python3
"""raw_to_root.py — convert the XDC/ADC data of a HiDRA merged binary (.raw) to ROOT.

The HiDRA DataCollector writes each run twice: a merged binary (.raw, via
``HidraMergedBinaryWriter`` / ``EventSerializer``) and a ROOT ntuple (via
``HidraRootEventWriter``). This tool reads the .raw back and re-decodes the XDC
(V792/V775) subevent into per-channel ADC/TDC values, writing the same
``ADCs`` / ``ADCFlags`` / ``TDCs`` / ``TDCFlags`` branches as the ROOT writer.

It exists to recover and cross-check the XDC information when the ROOT file is
missing it: in run904 every event from #196 on has an EMPTY ``ADCs`` vector in
ROOT, while the raw still holds the full payload. Running this converter on the
raw produces the 224-channel ADC vectors the ROOT writer dropped, proving the
data is present in the raw (the loss is in the ROOT write path, not the raw).

Self-contained: it parses the v11 merged-binary container itself (event header /
detector subevents / payload extraction, per ``DataFormat.md``) and ports the
word-level decode of ``dc/src/HidraXdcDecoder.cc`` (the channel layout matches
the ROOT writer's, i.e. ``HidraUtils`` ``computeADCchannelFromGeo``). It does not
depend on ``hidra_raw_parser`` (which predates the v11 event-flags header).

Scope: XDC (ADC/TDC) + scalar event metadata. FERS / Tracker branches are not
decoded here (out of scope for the ADC-recovery use case); they can be added the
same way by porting their decoders.

Examples
--------
Convert the whole run::

    raw_to_root.py run904.raw -o run904_from_raw.root

Verify against the original ROOT (no output file) — reports, per event, whether
the raw-decoded ADCs equal the ROOT ones, and flags events the ROOT lost::

    raw_to_root.py run904.raw --compare run904.root --max-events 3000
"""

from __future__ import annotations

import argparse
import struct

# VME module specs, mirroring hidra::utils::VMESpec (HidraUtils.hh):
# name -> (channels, is_qdc). QDC modules feed the ADC branches, the others TDC.
VMESPEC = {
    "V792": (32, True),
    "V792N": (16, True),
    "V862": (32, True),
    "V775": (32, False),
    "V775N": (16, False),
}

# Default XDC crate for run904 (the production VME_CRATE_1).
DEFAULT_VME_CRATE = "2:V792,4:V792,6:V792,8:V792,10:V792,11:V792,12:V862,14:V775N"

# CAEN VME data-word type field (bits 26:24): header / channel / trailer.
WTYPE_HEADER = 0b010
WTYPE_CHANNEL = 0b000
WTYPE_TRAILER = 0b100
WTYPE_INVALID = 0b110  # "no valid datum" (e.g. an empty/zero-suppressed module)


def parse_vme_crate(spec: str) -> dict[int, str]:
    """``"2:V792,4:V792,..."`` -> ``{2: "V792", 4: "V792", ...}`` (geo -> module)."""
    geo_map: dict[int, str] = {}
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        geo_s, _, mod = item.partition(":")
        mod = mod.strip()
        if mod not in VMESPEC:
            raise ValueError(f"unknown VME module type {mod!r} in {item!r}")
        geo_map[int(geo_s)] = mod
    return geo_map


# ── Merged-binary container parser (HiDRA v11, see DataFormat.md) ────────────

EVENT_MARKER = 0xB0BF
EVENT_HEADER_SIZE = 65
EVENT_TRAILER = 0xD04E
DETECTOR_MARKER = 0xDEDE
DETECTOR_HEADER_SIZE = 31
DETECTOR_END_MARKER = 0xDDDD
MAX_DETECTORS = 8


class RawEvent:
    """One merged event: header scalars + ``{detID: payload_bytes}``."""

    __slots__ = ("run", "event", "event_time", "time_span", "spill_number",
                 "trigger_mask", "event_flags", "detector_mask", "payloads")


def iter_raw_events(path: str, max_events: int = 0):
    """Yield :class:`RawEvent` for each event in a HiDRA merged binary (.raw).

    The byte offsets follow ``DataFormat.md`` v11 directly (``eventFlags`` @38,
    ``timeSpread`` @42), and each detector subevent's size comes from the header
    ``detectorSize[]`` table @47, so a payload is sliced as
    ``[start+31 : start+size-2]`` (31-byte subheader, 2-byte end marker).
    """
    u16 = lambda b, o: struct.unpack_from("<H", b, o)[0]  # noqa: E731
    u32 = lambda b, o: struct.unpack_from("<I", b, o)[0]  # noqa: E731
    u64 = lambda b, o: struct.unpack_from("<Q", b, o)[0]  # noqa: E731
    with open(path, "rb") as fh:
        idx = 0
        while not max_events or idx < max_events:
            header = fh.read(EVENT_HEADER_SIZE)
            if len(header) < EVENT_HEADER_SIZE:
                return  # clean EOF (or trailing partial header)
            if u16(header, 0) != EVENT_MARKER:
                raise ValueError(f"event {idx}: bad event marker {u16(header, 0):#06x}")
            event_size = u32(header, 11)
            rest = fh.read(event_size - EVENT_HEADER_SIZE)
            if len(rest) < event_size - EVENT_HEADER_SIZE:
                return  # truncated last event
            rec = header + rest

            ev = RawEvent()
            ev.run = u16(rec, 15)
            ev.event = u32(rec, 17)
            ev.spill_number = u32(rec, 21)
            ev.event_time = u64(rec, 25)
            ev.trigger_mask = rec[33]
            ev.event_flags = u32(rec, 38)
            ev.time_span = u32(rec, 42)
            ev.detector_mask = rec[46]
            det_sizes = struct.unpack_from("<8H", rec, 47)

            ev.payloads = {}
            pos = EVENT_HEADER_SIZE
            for det_id in range(MAX_DETECTORS):
                if not (ev.detector_mask >> det_id) & 1:
                    continue
                size = det_sizes[det_id]
                if u16(rec, pos) != DETECTOR_MARKER or rec[pos + 2] != det_id:
                    raise ValueError(f"event {idx}: detector {det_id} framing error at {pos}")
                ev.payloads[det_id] = rec[pos + DETECTOR_HEADER_SIZE : pos + size - 2]
                pos += size
            yield ev
            idx += 1


# ── XDC payload decoder (port of dc/src/HidraXdcDecoder.cc) ───────────────────


class XdcDecoder:
    """Port of ``hidra::HidraXdcDecoder`` for the V792/V775 payload.

    The geo map fixes the flattened channel layout: ADC channels are numbered by
    iterating the geo map in ascending geo order and accumulating the channel
    count of every preceding QDC module (TDC channels likewise over the TDC
    modules) — identical to ``computeADC/TDCchannelFromGeo`` in ``HidraUtils``.
    """

    def __init__(self, geo_map: dict[int, str]):
        self.geo_map = geo_map
        self.geos = sorted(geo_map)
        self.n_adc = sum(VMESPEC[geo_map[g]][0] for g in self.geos if VMESPEC[geo_map[g]][1])
        self.n_tdc = sum(VMESPEC[geo_map[g]][0] for g in self.geos if not VMESPEC[geo_map[g]][1])
        # Precompute geo -> channel-offset for ADC and TDC.
        self._adc_off: dict[int, int] = {}
        self._tdc_off: dict[int, int] = {}
        a = t = 0
        for g in self.geos:
            nch, is_qdc = VMESPEC[geo_map[g]]
            if is_qdc:
                self._adc_off[g] = a
                a += nch
            else:
                self._tdc_off[g] = t
                t += nch

    def decode(self, payload: bytes, *, robust: bool = False):
        """Decode one XDC payload.

        Returns ``(adc, adc_flags, tdc, tdc_flags)`` lists (``-1.0`` = no data),
        or ``None`` on a fatal parse error — matching the production decoder,
        which leaves the ADC vector empty so the ROOT writer skips it.

        With ``robust=True`` a malformed module does not abort the whole event:
        the decoder resynchronises to the next header word and keeps the channels
        it could read (used to maximise recovery from corrupted payloads).
        """
        if not payload or len(payload) % 4 != 0:
            return None
        # The payload is a stream of native 32-bit words (the producer stores the
        # CAEN BLT buffer verbatim; on the x86 collector that is little-endian, so
        # we read "<I" — the subevent endianness byte is informational only, the
        # C++ decoder likewise just memcpy's the bytes).
        words = struct.unpack(f"<{len(payload) // 4}I", payload)
        # Output vectors are pre-sized to the channel count and pre-filled with the
        # -1 "no data" sentinel; only hit channels get overwritten (matches the C++).
        adc = [-1.0] * self.n_adc
        adc_flags = [-1.0] * self.n_adc
        tdc = [-1.0] * self.n_tdc
        tdc_flags = [-1.0] * self.n_tdc

        # CAEN V79x/V77x word layout (32-bit): bits 31:27 = VME geo address,
        # 26:24 = type. Each module emits one block: a header, `cnt` channel words,
        # then a trailer.
        #   header  (type 0b010): bits 13:8 = channel count, 23:16 = crate
        #   channel (type 0b000): bits 11:0 = 12-bit value, 12 = overflow,
        #                         13 = underflow, 14 = valid-data (TDC only),
        #                         20:16 = channel (the 16-ch V792N/V775N put a
        #                         4-bit channel at bits 20:17 instead)
        #   trailer (type 0b100): bits 23:0 = event counter
        # A word with bits 31:25 all set (0xFE......) is a CAEN filler / EOB marker.
        i, n = 0, len(words)
        while i < n:
            w = words[i]
            if (w & 0xFE000000) == 0xFE000000:  # CAEN filler / end-of-buffer
                i += 1
                continue
            wtype = (w >> 24) & 0x7
            if wtype != WTYPE_HEADER:
                if wtype == WTYPE_INVALID:  # module with no datum: skip the word
                    i += 1
                    continue
                if robust:
                    i += 1
                    continue
                return None  # unexpected word where a header was expected -> abort
            geo = (w >> 27) & 0x1F
            mod = self.geo_map.get(geo)
            if mod is None:
                if robust:
                    i += 1
                    continue
                return None
            nchan = (w >> 8) & 0x3F
            i += 1
            bad = False
            for _ in range(nchan):
                if i >= n:
                    return None if not robust else (adc, adc_flags, tdc, tdc_flags)
                cw = words[i]
                i += 1
                if ((cw >> 24) & 0x7) != WTYPE_CHANNEL or ((cw >> 27) & 0x1F) != geo:
                    if robust:
                        bad = True
                        break
                    return None
                value = cw & 0xFFF
                ov = (cw >> 12) & 0x1
                un = (cw >> 13) & 0x1
                # `enc` is the flattened channel index (module offset + in-module
                # channel); flags are packed exactly as HidraXdcDecoder does.
                if VMESPEC[mod][1]:  # QDC -> ADC
                    ch = ((cw >> 17) & 0xF) if mod == "V792N" else ((cw >> 16) & 0x1F)
                    enc = self._adc_off[geo] + ch
                    if 0 <= enc < self.n_adc:
                        adc[enc] = float(value)
                        adc_flags[enc] = float((ov << 1) | un)  # overflow<<1 | underflow
                else:  # TDC
                    vd = (cw >> 14) & 0x1
                    ch = ((cw >> 17) & 0xF) if mod == "V775N" else ((cw >> 16) & 0x1F)
                    enc = self._tdc_off[geo] + ch
                    if 0 <= enc < self.n_tdc:
                        tdc[enc] = float(value)
                        tdc_flags[enc] = float((ov << 2) | (un << 1) | vd)  # ov<<2 | un<<1 | valid
            if bad:  # robust resync: scan to the next header word
                while i < n and ((words[i] >> 24) & 0x7) != WTYPE_HEADER:
                    i += 1
                continue
            # trailer
            if i >= n or ((words[i] >> 24) & 0x7) != WTYPE_TRAILER:
                if robust:
                    continue
                return None
            i += 1
        return adc, adc_flags, tdc, tdc_flags


def iter_xdc(raw_path: str, decoder: XdcDecoder, xdc_detid: int, max_events: int, robust: bool):
    """Yield ``(RawEvent, decoded_or_None)`` for each merged event in the raw.

    ``decoded`` is ``None`` when the event carries no XDC subevent (e.g. a
    tracker-only event) or when its payload fails to decode in strict mode.
    """
    for ev in iter_raw_events(raw_path, max_events):
        payload = ev.payloads.get(xdc_detid)
        if payload is None:
            yield ev, None
            continue
        yield ev, decoder.decode(payload, robust=robust)


def convert(args, decoder: XdcDecoder) -> None:
    import awkward as ak
    import numpy as np
    import uproot

    batch = 5000
    keys = ("run", "event", "event_time", "time_span", "spill_number", "detector_mask",
            "trigger_mask", "event_flags", "ADCs", "ADCFlags", "TDCs", "TDCFlags")
    # Scalar branch dtypes mirror HidraRootEventWriter; the XDC vectors are jagged
    # doubles. mktree forces a classic TTree (uproot 5.x writes an RNTuple for a
    # plain dict assignment), so the output matches the production ROOT format.
    scalar_dtype = {
        "run": np.int32, "event": np.uint32, "event_time": np.uint64, "time_span": np.uint32,
        "spill_number": np.uint32, "detector_mask": np.uint8, "trigger_mask": np.uint8,
        "event_flags": np.uint32,
    }
    branch_types = {**scalar_dtype, **{v: "var * float64" for v in ("ADCs", "ADCFlags", "TDCs", "TDCFlags")}}
    cols: dict[str, list] = {k: [] for k in keys}
    n_written = 0

    def flush(fout):
        data = {k: np.array(cols[k], dtype=scalar_dtype[k]) for k in scalar_dtype}
        for k in ("ADCs", "ADCFlags", "TDCs", "TDCFlags"):
            data[k] = ak.Array(cols[k])
        fout["hidra"].extend(data)
        for v in cols.values():
            v.clear()

    with uproot.recreate(args.output) as fout:
        fout.mktree("hidra", branch_types)
        for ev, dec in iter_xdc(args.input, decoder, args.xdc_detid, args.max_events, args.robust):
            adc, adcf, tdc, tdcf = dec if dec is not None else ([], [], [], [])
            cols["run"].append(ev.run)
            cols["event"].append(ev.event)
            cols["event_time"].append(ev.event_time)
            cols["time_span"].append(ev.time_span)
            cols["spill_number"].append(ev.spill_number)
            cols["detector_mask"].append(ev.detector_mask)
            cols["trigger_mask"].append(ev.trigger_mask)
            cols["event_flags"].append(ev.event_flags)
            cols["ADCs"].append(list(adc))
            cols["ADCFlags"].append(list(adcf))
            cols["TDCs"].append(list(tdc))
            cols["TDCFlags"].append(list(tdcf))
            n_written += 1
            if len(cols["event"]) >= batch:
                flush(fout)
        if cols["event"]:
            flush(fout)
    print(f"wrote {n_written} events to {args.output} (n_adc={decoder.n_adc}, n_tdc={decoder.n_tdc})")


def compare(args, decoder: XdcDecoder) -> None:
    import numpy as np
    import uproot

    t = uproot.open(args.compare)["hidra"]
    stop = args.max_events or t.num_entries
    ev_root = t["event"].array(entry_stop=stop, library="np")
    adc_root = t["ADCs"].array(entry_stop=stop, library="np")
    root_by_event: dict[int, np.ndarray] = {}
    for e, a in zip(ev_root, adc_root):
        root_by_event.setdefault(int(e), np.asarray(a, dtype=float))

    n = equal = differ = root_empty_raw_full = both = raw_abort = 0
    first_loss = None
    examples = []
    for ev, dec in iter_xdc(args.input, decoder, args.xdc_detid, args.max_events, args.robust):
        rt = root_by_event.get(ev.event)
        if rt is None:
            continue
        n += 1
        if dec is None:
            raw_abort += 1
            continue
        raw = np.asarray(dec[0], dtype=float)
        if len(rt) == 0:
            if np.any(raw >= 0):
                root_empty_raw_full += 1
                if first_loss is None:
                    first_loss = ev.event
                if len(examples) < 3:
                    examples.append((ev.event, [int(x) for x in raw[:12]]))
            else:
                both += 1
        elif len(rt) == len(raw) and np.allclose(rt, raw):
            equal += 1
        else:
            differ += 1

    print(f"compared {n} events present in both (n_adc={decoder.n_adc})")
    print(f"  ROOT==raw (validates decoder) : {equal}")
    print(f"  ROOT!=raw (mismatch)          : {differ}")
    print(f"  ROOT empty, raw has ADC data  : {root_empty_raw_full}   <-- data recovered from raw")
    print(f"  ROOT empty, raw also empty    : {both}")
    print(f"  raw decode aborted            : {raw_abort}")
    if first_loss is not None:
        print(f"  first event ROOT lost but raw has: {first_loss}")
    for en, vals in examples:
        print(f"    event {en}: raw ADC[0:12] = {vals}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", help="HiDRA merged binary (.raw)")
    p.add_argument("-o", "--output", help="output ROOT file (convert mode)")
    p.add_argument("--compare", help="existing ROOT file to verify against (compare mode, no output)")
    p.add_argument("--vme-crate", default=DEFAULT_VME_CRATE,
                   help="VME_CRATE_1 geo map (default: run904 production)")
    p.add_argument("--xdc-detid", type=int, default=1, help="detector ID of the XDC subevent (default 1)")
    p.add_argument("--max-events", type=int, default=0, help="stop after N events (0 = all)")
    p.add_argument("--robust", action="store_true",
                   help="recover from malformed payloads instead of aborting the event")
    args = p.parse_args(argv)

    decoder = XdcDecoder(parse_vme_crate(args.vme_crate))
    if args.compare:
        compare(args, decoder)
    elif args.output:
        convert(args, decoder)
    else:
        p.error("specify -o/--output (convert) or --compare (verify)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
