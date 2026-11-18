"""Run with: python -m unittest discover -s tests -v (from frontend/)."""

import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from dash import Dash, no_update
import requests

from hidra_frontend.backend_client import BackendClient
from hidra_frontend.callbacks import poll
from hidra_frontend.config import load_config
from hidra_frontend.mapping import get_pmt_channel_info, get_sipm_channel_info
from hidra_frontend.panels.event_display import EventDisplayPanel, front_face_figure


def event(event_id="one", number=100):
    return dict(id=event_id, run_number=42, event_number=number, trigger_number=number,
                trigger_mask=1, spill_number=None, timestamp_begin_ns="1795000000123456789",
                adc=[0, None, 123], fers_hg=[12, 0, None], fers_lg=[3, 4])


class EventDisplayTests(unittest.TestCase):
    def setUp(self):
        self.client = Mock()
        self.panel = EventDisplayPanel("event_display__p0", {"_client": self.client})
        self.app = Dash(__name__, suppress_callback_exceptions=True)
        self.app.layout = self.panel.layout()
        self.panel.register_callbacks(self.app)
        self.callbacks = {
            value["callback"].__wrapped__.__name__: value["callback"].__wrapped__
            for value in self.app.callback_map.values()
        }

    def test_every_fibre_is_inside_its_mapped_module_and_keeps_orientation(self):
        original = get_sipm_channel_info()
        cells = self.panel._sipm_geom["cells"]
        self.assertEqual({ch for _, _, ch, _ in cells}, set(original))
        for x, y, ch, cell in cells:
            col, row = divmod(int(cell["module"]), 100)
            self.assertTrue(col - .5 < x < col + .5)
            self.assertTrue(row - .5 < y < row + .5)
            self.assertEqual(cell, original[ch])
        positions = {(c["module"], c["column"], c["row"]): (x, y) for x, y, _, c in cells}
        for x, y, _, cell in cells:
            above = positions.get((cell["module"], cell["column"], cell["row"] + 1))
            if above:
                self.assertGreater(above[1], y)
        # The same module convention is used by every existing ADC placement.
        for cell in get_pmt_channel_info().values():
            self.assertEqual(divmod(int(cell["module"][1:]), 100), (cell["column"], cell["row"]))

    def test_figures_contain_exact_event_values_and_selected_gain(self):
        info = get_sipm_channel_info()
        channel = next(ch for ch, cell in info.items() if cell["type"] == "S")
        snapshot = event()
        snapshot["fers_hg"] = [None] * (max(info) + 1)
        snapshot["fers_lg"] = [None] * (max(info) + 1)
        snapshot["fers_hg"][channel] = 987
        snapshot["fers_lg"][channel] = 0
        for gain, expected in (("HG", 987), ("LG", 0)):
            fig = front_face_figure("S", snapshot, gain, "full", self.panel._pmt_geom, self.panel._sipm_geom)
            values = [value for row in fig.data[4].z for value in row if value is not None]
            self.assertEqual(values, [expected])
            self.assertIn(f"FERS {gain}", fig.layout.title.text)
            self.assertEqual(fig.data[4].zmax, 4095)
        empty = front_face_figure("C", {}, "HG", "full", self.panel._pmt_geom, self.panel._sipm_geom)
        self.assertTrue(all(value is None for row in empty.data[4].z for value in row))
        self.assertEqual(len(empty.layout.shapes), 10)

    def test_catalogue_pins_a_captured_event_after_eviction(self):
        self.client.event_index.return_value = dict(capacity=2, event_prescale=3, events=[event("two", 101)])
        catalog, options, status = self.callbacks["refresh_catalog"](0, {"tab": "event_display"}, 0, event())
        self.assertEqual([entry["value"] for entry in options], ["two", "one"])
        self.assertIn("captured", options[-1]["label"])
        self.assertIn("prescale 3", status)
        with patch("hidra_frontend.panels.event_display.ctx", SimpleNamespace(triggered_id=None)):
            result = self.callbacks["navigate"](catalog, 0, 0, 0, "one", event())
        self.assertIs(result, no_update)

    def test_event_scale_ignores_unmapped_adc_channels(self):
        mapped = get_pmt_channel_info()
        unmapped = next(ch for ch in range(224) if ch not in mapped)
        snapshot = event()
        snapshot["adc"] = [None] * 224
        snapshot["adc"][next(iter(mapped))] = 17
        snapshot["adc"][unmapped] = 4000
        for typ in "SC":
            fig = front_face_figure(typ, snapshot, "HG", "event", self.panel._pmt_geom, self.panel._sipm_geom)
            self.assertEqual(fig.data[1].zmax, 17)

    def test_previous_next_latest_use_buffer_order(self):
        catalog = [event("one", 100), event("two", 110), event("three", 200)]
        for action, selected, expected in (("previous", "two", "one"), ("next", "one", "two"),
                                           ("latest", "one", "three"), ("previous", "one", "one")):
            with patch("hidra_frontend.panels.event_display.ctx",
                       SimpleNamespace(triggered_id=self.panel._id(action))):
                self.assertEqual(self.callbacks["navigate"](catalog, 1, 1, 1, selected, None), expected)

    def test_capture_is_per_viewer_and_does_not_refetch_a_pinned_event(self):
        capture = self.callbacks["capture"]
        first = event()
        self.client.event_snapshot.return_value = first
        captured, metadata = capture("one", None)
        self.assertEqual(captured, first)
        self.assertIn("event 100", metadata)
        self.client.event_snapshot.reset_mock()
        self.assertIs(capture("one", first)[0], no_update)
        self.client.event_snapshot.assert_not_called()
        second = event("two", 101)
        self.client.event_snapshot.return_value = second
        self.assertEqual(capture("two", None)[0], second)
        self.assertEqual(first["id"], "one")
        # An expired selection clears the picture rather than mislabelling old data.
        self.client.event_snapshot.return_value = None
        cleared, message = capture("evicted", first)
        self.assertEqual(cleared, {})
        self.assertIn("no longer available", message)

    def test_unavailable_feed_preserves_the_last_catalogue(self):
        self.client.event_index.return_value = None
        catalog, options, message = self.callbacks["refresh_catalog"](0, {"tab": "event_display"}, 0, event())
        self.assertIs(catalog, no_update)
        self.assertIs(options, no_update)
        self.assertIn("unavailable", message)

    def test_backend_client_handles_old_backend_and_expired_events(self):
        backend = BackendClient("http://example.invalid")
        with patch("hidra_frontend.backend_client.requests.get", side_effect=requests.ConnectionError):
            self.assertIsNone(backend.event_index())
        with patch("hidra_frontend.backend_client.requests.get") as get:
            get.return_value.json.return_value = event()
            self.assertEqual(backend.event_snapshot("one")["id"], "one")
            self.assertEqual(get.call_args.args[0], "http://example.invalid/EventDisplay/one.json")

    def test_histogram_poll_keeps_the_header_live_without_touching_event_figures(self):
        config = load_config(Path(__file__).parents[1] / "config.yaml")
        self.client.fetch_multi.return_value = {
            "event_count": {"_typename": "TH1I", "fXaxis": {"fNbins": 1}, "fArray": [0, 8, 0]}
        }
        poll.register(self.app, config, {"event_display": [self.panel]}, self.client, Mock(), Mock())
        callback = next(value["callback"].__wrapped__ for value in self.app.callback_map.values()
                        if value["callback"].__wrapped__.__name__ == "poll")
        with patch("hidra_frontend.callbacks.poll.ctx", SimpleNamespace(outputs_list=[[], None, None])):
            figures, status, count = callback(1, {"tab": "event_display"}, {}, [], {})
        self.assertEqual(figures, [])
        self.assertEqual(count, "8")
        self.assertIn("backend connected", status[1])
        self.client.fetch_multi.assert_called_once_with(["event_count"])


if __name__ == "__main__":
    unittest.main()
