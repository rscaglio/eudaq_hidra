#include "FERSFiller.hh"
#include "HidraUtils.hh"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

FERSFiller::FERSFiller(HistogramRegistry& reg,
                       unsigned int n_boards,
                       unsigned int channels_per_board,
                       int value_max,
                       int channel_nbins,
                       int saturation_threshold)
    : IHistogramFiller("FERSFiller"),
      m_n_boards(n_boards),
      m_channels_per_board(channels_per_board),
      m_n_channels(0),
      m_saturation_threshold(saturation_threshold) {
  if (m_n_boards == 0) {
    HIDRA_WARN("FERSFiller n_boards=0 is invalid, forcing 1.");
    m_n_boards = 1;
  }
  if (m_channels_per_board == 0) {
    HIDRA_WARN("FERSFiller channels_per_board=0 is invalid, forcing 64.");
    m_channels_per_board = 64;
  }
  if (channel_nbins < 1) {
    HIDRA_WARN("FERSFiller channel_nbins={} is invalid, forcing 1024.", channel_nbins);
    channel_nbins = 1024;
  }
  if (value_max < 1) {
    HIDRA_WARN("FERSFiller value_max={} is invalid, forcing 4096.", value_max);
    value_max = 4096;
  }
  m_n_channels = m_n_boards * m_channels_per_board;

  // Per-channel means. The ";channel;<y>" axis titles mark them as channel-indexed
  // for the frontend (channel-numbered hover, used by the detector heatmap).
  m_hg_mean = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_mean", "Mean FERS high gain;channel;mean HG", m_n_channels, 0, m_n_channels));
  m_hg_mean_physics = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_mean_physics", "Mean FERS high gain (physics);channel;mean HG", m_n_channels, 0, m_n_channels));
  m_hg_mean_pedestal = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_mean_pedestal", "Mean FERS high gain (pedestal);channel;mean HG", m_n_channels, 0, m_n_channels));
  m_lg_mean = reg.Add(std::make_unique<TProfile>(
      "FERS_LG_mean", "Mean FERS low gain;channel;mean LG", m_n_channels, 0, m_n_channels));
  m_lg_mean_physics = reg.Add(std::make_unique<TProfile>(
      "FERS_LG_mean_physics", "Mean FERS low gain (physics);channel;mean LG", m_n_channels, 0, m_n_channels));
  m_lg_mean_pedestal = reg.Add(std::make_unique<TProfile>(
      "FERS_LG_mean_pedestal", "Mean FERS low gain (pedestal);channel;mean LG", m_n_channels, 0, m_n_channels));

  // Inclusive distributions over all channels (physics only).
  m_hg_inclusive_physics = reg.Add(std::make_unique<TH1I>(
      "FERS_HG_inclusive_physics", "Inclusive FERS high gain (physics);HG [ADC];entries", channel_nbins, 0, value_max));
  m_lg_inclusive_physics = reg.Add(std::make_unique<TH1I>(
      "FERS_LG_inclusive_physics", "Inclusive FERS low gain (physics);LG [ADC];entries", channel_nbins, 0, value_max));

  // Per-channel distributions: physics and pedestal only (no "total" copy, to
  // save memory), for each gain.
  auto book_channels = [&](const char* gain,
                           std::vector<TH1I*>& physics,
                           std::vector<TH1I*>& pedestal) {
    physics.reserve(m_n_channels);
    pedestal.reserve(m_n_channels);
    for (unsigned int c = 0; c < m_n_channels; ++c) {
      physics.push_back(reg.Add(std::make_unique<TH1I>(
          hidra::utils::format("FERS_{}_channel_{}_physics", gain, c).c_str(),
          hidra::utils::format("FERS {} channel {} (physics);{} [ADC];entries", gain, c, gain).c_str(),
          channel_nbins, 0, value_max)));
      pedestal.push_back(reg.Add(std::make_unique<TH1I>(
          hidra::utils::format("FERS_{}_channel_{}_pedestal", gain, c).c_str(),
          hidra::utils::format("FERS {} channel {} (pedestal);{} [ADC];entries", gain, c, gain).c_str(),
          channel_nbins, 0, value_max)));
    }
  };
  book_channels("HG", m_hg_channels_physics, m_hg_channels_pedestal);
  book_channels("LG", m_lg_channels_physics, m_lg_channels_pedestal);

  // Per-channel saturation fraction (TProfile of a 0/1 indicator). Total and
  // physics only — pedestal events don't saturate. The ";channel;" x-axis title
  // marks them as channel-indexed for the frontend.
  m_hg_saturation = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_saturation", "Saturation fraction per FERS HG channel;channel;saturation fraction", m_n_channels, 0,
      m_n_channels));
  m_hg_saturation_physics = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_saturation_physics", "Saturation fraction per FERS HG channel (physics);channel;saturation fraction",
      m_n_channels, 0, m_n_channels));
  m_lg_saturation = reg.Add(std::make_unique<TProfile>(
      "FERS_LG_saturation", "Saturation fraction per FERS LG channel;channel;saturation fraction", m_n_channels, 0,
      m_n_channels));
  m_lg_saturation_physics = reg.Add(std::make_unique<TProfile>(
      "FERS_LG_saturation_physics", "Saturation fraction per FERS LG channel (physics);channel;saturation fraction",
      m_n_channels, 0, m_n_channels));

  // Per-board aggregates.
  m_hg_board_mean = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_board_mean", "Mean FERS high gain over board channels;board;mean HG", m_n_boards, 0, m_n_boards));
  m_lg_board_mean = reg.Add(std::make_unique<TProfile>(
      "FERS_LG_board_mean", "Mean FERS low gain over board channels;board;mean LG", m_n_boards, 0, m_n_boards));
  m_hg_board_allzero = reg.Add(std::make_unique<TProfile>(
      "FERS_HG_board_allzero", "Fraction of events with all HG channels zero;board;all-zero fraction", m_n_boards, 0,
      m_n_boards));

  // Board-time compatibility: per-board offset from the per-event median.
  m_board_time_offset = reg.Add(std::make_unique<TProfile>(
      "FERS_board_time_offset", "Board time offset vs median;board;rel. time offset [#mus]", m_n_boards, 0, m_n_boards));
}

void FERSFiller::Fill(const HidraEvent& event) {
  // Split copies are chosen once per event from the trigger mask (physics = bit
  // 0, pedestal = bit 1); a "both" event feeds both. The per-channel
  // distributions exist only for physics/pedestal; the mean profiles also keep
  // an inclusive (total) copy.
  const bool is_physics = event.meta.isPhysics();
  const bool is_pedestal = event.meta.isPedestal();
  const HidraFersEvent& fers = event.fers;

  const std::size_t n_hg = std::min<std::size_t>(fers.FERShg.size(), m_n_channels);
  for (std::size_t i = 0; i < n_hg; ++i) {
    const double hg = fers.FERShg[i];
    // The decoder leaves absent channels at the -1 sentinel; skip them.
    if (hg >= 0) {
      m_hg_mean->Fill(i, hg);
      m_hg_board_mean->Fill(i / m_channels_per_board, hg);
      if (is_physics) {
        m_hg_mean_physics->Fill(i, hg);
        m_hg_inclusive_physics->Fill(hg);
        m_hg_channels_physics[i]->Fill(hg);
      }
      if (is_pedestal) {
        m_hg_mean_pedestal->Fill(i, hg);
        m_hg_channels_pedestal[i]->Fill(hg);
      }
      const int saturated = hg > m_saturation_threshold ? 1 : 0;
      m_hg_saturation->Fill(i, saturated);
      if (is_physics) {
        m_hg_saturation_physics->Fill(i, saturated);
      }
    }
  }

  const std::size_t n_lg = std::min<std::size_t>(fers.FERSlg.size(), m_n_channels);
  for (std::size_t i = 0; i < n_lg; ++i) {
    const double lg = fers.FERSlg[i];
    if (lg >= 0) {
      m_lg_mean->Fill(i, lg);
      m_lg_board_mean->Fill(i / m_channels_per_board, lg);
      if (is_physics) {
        m_lg_mean_physics->Fill(i, lg);
        m_lg_inclusive_physics->Fill(lg);
        m_lg_channels_physics[i]->Fill(lg);
      }
      if (is_pedestal) {
        m_lg_mean_pedestal->Fill(i, lg);
        m_lg_channels_pedestal[i]->Fill(lg);
      }
      const int saturated = lg > m_saturation_threshold ? 1 : 0;
      m_lg_saturation->Fill(i, saturated);
      if (is_physics) {
        m_lg_saturation_physics->Fill(i, saturated);
      }
    }
  }

  FillBoardAllZeroHG(fers);
  FillBoardTime(fers);
}

void FERSFiller::FillBoardAllZeroHG(const HidraFersEvent& fers) {
  // Per board: 1 if every present HG channel reads exactly zero, else 0 — flags a
  // board that is present (has data) but stopped producing signal. Boards with no
  // present channel at all are skipped (absent, not "all zero").
  for (unsigned int b = 0; b < m_n_boards; ++b) {
    bool any_present = false;
    bool any_nonzero = false;
    for (unsigned int ich = 0; ich < m_channels_per_board; ++ich) {
      const std::size_t idx = static_cast<std::size_t>(b) * m_channels_per_board + ich;
      if (idx >= fers.FERShg.size()) {
        break;
      }
      const double v = fers.FERShg[idx];
      if (v >= 0) { // present (−1 is the absent sentinel)
        any_present = true;
        if (v > 0) {
          any_nonzero = true;
          break;
        }
      }
    }
    if (any_present) {
      m_hg_board_allzero->Fill(b, any_nonzero ? 0 : 1);
    }
  }
}

void FERSFiller::FillBoardTime(const HidraFersEvent& fers) {
  // Each board's timestamp is replicated across its channels. Take the time of
  // the first present channel of each board, then offset every present board
  // from the median over the boards present in this event — the median is robust
  // to a single board being far off (which is exactly what we want to flag).
  std::vector<double> board_time(m_n_boards, std::numeric_limits<double>::quiet_NaN());
  for (unsigned int b = 0; b < m_n_boards; ++b) {
    for (unsigned int ich = 0; ich < m_channels_per_board; ++ich) {
      const std::size_t idx = static_cast<std::size_t>(b) * m_channels_per_board + ich;
      if (idx >= fers.FERSrel_tsamp_us.size()) {
        break;
      }
      if (fers.FERSrel_tsamp_us[idx] >= 0) {
        board_time[b] = fers.FERSrel_tsamp_us[idx];
        break;
      }
    }
  }

  std::vector<double> present;
  present.reserve(m_n_boards);
  for (const double t : board_time) {
    if (!std::isnan(t)) {
      present.push_back(t);
    }
  }
  if (present.empty()) {
    return;
  }

  std::sort(present.begin(), present.end());
  const std::size_t mid = present.size() / 2;
  const double median =
      (present.size() % 2 == 0) ? 0.5 * (present[mid - 1] + present[mid]) : present[mid];

  for (unsigned int b = 0; b < m_n_boards; ++b) {
    if (!std::isnan(board_time[b])) {
      m_board_time_offset->Fill(b, board_time[b] - median);
    }
  }
}
