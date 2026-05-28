"""ChannelSelectorPanel — dropdown of channel names, one histogram shown.

STUB. The default config.yaml does not enable this panel because the
backend currently exposes only ADC_mean / TDC_mean as TProfiles, not
per-channel TH1Ds.

TODO(per_channel_hists): when XDCFiller starts publishing
xdc_ch_<N> histograms (TH1D, 4096 bins each), implement the body of
this class:

  * `layout()` should add a dcc.Dropdown (id including self.panel_id
    so the controls callback file can pick it up) plus a single
    panel-graph slot.
  * `histogram_names()` should return only the currently-selected
    name (read from client_state, populated by a callback registered
    in `register_callbacks()`).
  * `render()` returns one figure.

Until that lands, enabling this panel in config.yaml will raise
NotImplementedError at startup so you don't ship a half-working UI.
"""

from __future__ import annotations

from dash import html

from .base import Panel


class ChannelSelectorPanel(Panel):
    def __init__(self, panel_id, params):
        super().__init__(panel_id, params)
        raise NotImplementedError(
            "channel_selector panel is not implemented yet — waiting for "
            "per-channel TH1D histograms on the backend"
        )

    def histogram_names(self):
        return []

    def layout(self) -> html.Div:
        return html.Div()

    def render(self, figs, payloads, client_state):
        return []
