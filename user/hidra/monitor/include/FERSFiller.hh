#pragma once

#include "HistogramRegistry.hh"
#include "IHistogramFiller.hh"

#include <TH1.h>
#include <TProfile.h>

#include <vector>

/**
 * @brief Fills the FERS monitoring histograms from HidraEvent::fers.
 *
 * Twin of XDCFiller, for the FERS detector (n_boards × 64 channels, flat
 * per-channel vectors with a -1 "no hit" sentinel). It publishes:
 *   - per-channel mean HG and LG (TProfile, x = channel), split into
 *     physics/pedestal via the trigger mask, plus an inclusive (physics)
 *     distribution over all channels;
 *   - per-channel HG and LG distributions (TH1I, one per channel, for the
 *     frontend channel dropdown), split physics/pedestal only (no standalone
 *     "total" copy, to save memory — physics vs pedestal is the useful overlay);
 *   - per-channel saturation fraction HG and LG (TProfile of a 0/1 indicator,
 *     x = channel), total and physics only (pedestal events don't saturate): a
 *     channel is saturated when its value exceeds a configurable threshold;
 *   - per-board aggregates (TProfile, x = board): the mean over all channels of
 *     the board for each gain (`FERS_HG_board_mean` / `FERS_LG_board_mean`) and
 *     the fraction of events in which every HG channel of the board reads zero
 *     (`FERS_HG_board_allzero`, e.g. a board that stopped responding);
 *   - board-time compatibility: per board, the offset of its timestamp from the
 *     median over all boards present in the event (TProfile, x = board).
 *
 * The data source is identical regardless of how `fers` was produced (the real
 * HidraFersDecoder or the random test decoder).
 */
class FERSFiller : public IHistogramFiller {
public:
  explicit FERSFiller(HistogramRegistry& reg,
                      unsigned int n_boards = 20,
                      unsigned int channels_per_board = 64,
                      int value_max = 4096,
                      int channel_nbins = 1024,
                      int saturation_threshold = 3800);
  void Fill(const HidraEvent&) override;

private:
  void FillBoardTime(const HidraFersEvent& fers);
  void FillBoardAllZeroHG(const HidraFersEvent& fers);

  unsigned int m_n_boards;
  unsigned int m_channels_per_board;
  unsigned int m_n_channels;
  double m_saturation_threshold;

  // Per-channel mean (x = channel). The ";channel;" x-axis title marks these as
  // channel-indexed so the frontend labels the hover with the channel number.
  TProfile* m_hg_mean;
  TProfile* m_hg_mean_physics;
  TProfile* m_hg_mean_pedestal;
  TProfile* m_lg_mean;
  TProfile* m_lg_mean_physics;
  TProfile* m_lg_mean_pedestal;

  // Inclusive distribution over all channels (physics only).
  TH1I* m_hg_inclusive_physics;
  TH1I* m_lg_inclusive_physics;

  // Per-channel saturation fraction (TProfile of a 0/1 indicator, x = channel).
  // Total and physics only — pedestal events don't saturate.
  TProfile* m_hg_saturation;
  TProfile* m_hg_saturation_physics;
  TProfile* m_lg_saturation;
  TProfile* m_lg_saturation_physics;

  // Per-channel distributions, physics and pedestal only (no "total" copy, to
  // save memory). TH1I (Int_t, 4 B per bin) since the contents are integer
  // counts: half the memory of TH1D.
  std::vector<TH1I*> m_hg_channels_physics;
  std::vector<TH1I*> m_hg_channels_pedestal;
  std::vector<TH1I*> m_lg_channels_physics;
  std::vector<TH1I*> m_lg_channels_pedestal;

  // Per-board aggregates (x = board).
  TProfile* m_hg_board_mean;    // mean HG over all channels of the board
  TProfile* m_lg_board_mean;    // mean LG over all channels of the board
  TProfile* m_hg_board_allzero; // fraction of events with every HG channel == 0

  // Board time offset vs the per-event median (x = board).
  TProfile* m_board_time_offset;
};
