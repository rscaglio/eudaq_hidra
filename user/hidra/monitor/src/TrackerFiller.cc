#include "TrackerFiller.hh"
#include "HidraUtils.hh"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

TrackerFiller::TrackerFiller(HistogramRegistry& reg, const std::vector<TrackerStationConfig>& stations)
    : IHistogramFiller("TrackerFiller") {
  m_station_hist.reserve(stations.size());
  for (std::size_t i = 0; i < stations.size(); ++i) {
    TrackerStationConfig cfg = stations[i];
    if (cfg.x_nbins < 1) {
      HIDRA_WARN("TrackerFiller station {} x_nbins={} is invalid, forcing 100.", i, cfg.x_nbins);
      cfg.x_nbins = 100;
    }
    if (cfg.y_nbins < 1) {
      HIDRA_WARN("TrackerFiller station {} y_nbins={} is invalid, forcing 100.", i, cfg.y_nbins);
      cfg.y_nbins = 100;
    }
    if (cfg.x_max <= cfg.x_min) {
      HIDRA_WARN("TrackerFiller station {} x range [{}, {}] is empty/inverted, forcing [0, 1000].", i, cfg.x_min,
                 cfg.x_max);
      cfg.x_min = 0.0;
      cfg.x_max = 1000.0;
    }
    if (cfg.y_max <= cfg.y_min) {
      HIDRA_WARN("TrackerFiller station {} y range [{}, {}] is empty/inverted, forcing [0, 1000].", i, cfg.y_min,
                 cfg.y_max);
      cfg.y_min = 0.0;
      cfg.y_max = 1000.0;
    }

    const std::string name = hidra::utils::format("Tracker_station{}", i);
    const std::string title = hidra::utils::format("Tracker station {} hit map;X;Y", i);
    m_station_hist.push_back(reg.Add(std::make_unique<TH2I>(
        name.c_str(), title.c_str(), cfg.x_nbins, cfg.x_min, cfg.x_max, cfg.y_nbins, cfg.y_min, cfg.y_max)));
  }
}

void TrackerFiller::Fill(const HidraEvent& event) {
  const HidraTrackerEvent& tracker = event.tracker;

  // No tracker sub-event this event (or a decode that produced nothing): the
  // decoder leaves X/Y empty, so there is nothing to fill.
  const std::size_t n = std::min(m_station_hist.size(), std::min(tracker.X.size(), tracker.Y.size()));
  for (std::size_t i = 0; i < n; ++i) {
    const double x = tracker.X[i];
    const double y = tracker.Y[i];
    // Skip no-hit coordinates so they don't appear as a hit: the producer marks
    // a missing measurement with a negative "no hit" sentinel (e.g. -5000/-6000)
    // and real cm coordinates are >= 0. (The raw values, sentinels included, are
    // still kept by the decoder and written to the ROOT output.)
    if (x < 0.0 || y < 0.0) {
      continue;
    }
    m_station_hist[i]->Fill(x, y);
  }
}
