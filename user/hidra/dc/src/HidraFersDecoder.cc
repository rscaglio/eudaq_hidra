#include "HidraUtils.hh"

#include "HidraFersDecoder.hh"

#include <array>
#include <cstring>
#include <limits>

namespace hidra {

#pragma pack(push, 1)
struct FERS_spect_64_packed { // no padding
  uint16_t marker;            // 0xAAAA
  uint16_t block_size;
  uint32_t data_qualifier;
  uint8_t board_id;
  double tstamp_us;
  double rel_tstamp_us;
  uint64_t tstamp_clk;
  uint64_t Tref_tstamp;
  uint64_t trigger_id;
  uint64_t chmask;
  uint64_t qdmask;
  std::array<uint16_t, 64> energyHG;
  std::array<uint16_t, 64> energyLG;
  std::array<uint32_t, 64> tstamp;
  std::array<uint16_t, 64> ToT;
};
#pragma pack(pop)

/* reimplementing below (Nicolo)
void HidraFersPayloadDecoder::Decode(const RootDetectorPayload& detector,
                                     std::vector<RootQuantity>& quantities,
                                     RootBranchValues& branches) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities, branches);

  const auto& payload = detector.payload;

  if (payload.size() >= 56) {
    const auto timestamp_us = ReadLE<double>(payload, 0);
    const auto rel_timestamp_us = ReadLE<double>(payload, 8);
    const auto trigger_id = static_cast<double>(ReadLE<std::uint64_t>(payload, 32));
    const auto chmask = static_cast<double>(ReadLE<std::uint64_t>(payload, 40));
    const auto qdmask = static_cast<double>(ReadLE<std::uint64_t>(payload, 48));

    AddQuantity(quantities, "fers2_tstamp_us", timestamp_us, "us");
    AddQuantity(quantities, "fers2_rel_tstamp_us", rel_timestamp_us, "us");
    AddQuantity(quantities, "fers2_trigger_id", trigger_id);
    AddQuantity(quantities, "fers2_chmask", chmask);
    AddQuantity(quantities, "fers2_qdmask", qdmask);

    AddBranchValue(branches, "fers2_tstamp_us", timestamp_us);
    AddBranchValue(branches, "fers2_rel_tstamp_us", rel_timestamp_us);
    AddBranchValue(branches, "fers2_trigger_id", trigger_id);
    AddBranchValue(branches, "fers2_chmask", chmask);
    AddBranchValue(branches, "fers2_qdmask", qdmask);
  }

  if (payload.size() >= 56 + 64 * 2) {
    std::uint64_t energy_sum = 0;
    std::uint16_t energy_max = 0;
    std::vector<double> energies_hg;
    energies_hg.reserve(64);
    for (std::size_t index = 0; index < 64; ++index) {
      const auto value = ReadLE<std::uint16_t>(payload, 56 + index * sizeof(std::uint16_t));
      energy_sum += value;
      energy_max = std::max(energy_max, value);
      energies_hg.push_back(static_cast<double>(value));
    }
    AddQuantity(quantities, "fers2_energy_hg_sum", static_cast<double>(energy_sum));
    AddQuantity(quantities, "fers2_energy_hg_max", static_cast<double>(energy_max));

    AddBranchValues(branches, "fers2_energy_hg", energies_hg);
    AddBranchValue(branches, "fers2_energy_hg_sum", static_cast<double>(energy_sum));
    AddBranchValue(branches, "fers2_energy_hg_max", static_cast<double>(energy_max));
  }

  if (payload.size() >= 17 && payload.size() < 56) {
    const auto board_id = static_cast<double>(payload[0]);
    const auto timestamp_ns = ReadLE<double>(payload, 1) * 1000.0;
    const auto trigger_id = static_cast<double>(ReadLE<std::uint64_t>(payload, 9));

    AddQuantity(quantities, "fers_first_board_id", board_id);
    AddQuantity(quantities, "fers_first_trigger_timestamp_ns", timestamp_ns, "ns");
    AddQuantity(quantities, "fers_first_trigger_id", trigger_id);

    AddBranchValue(branches, "fers_first_board_id", board_id);
    AddBranchValue(branches, "fers_first_trigger_timestamp_ns", timestamp_ns);
    AddBranchValue(branches, "fers_first_trigger_id", trigger_id);
  }

}
*/

HidraFersDecoder::HidraFersDecoder() = default;

void HidraFersDecoder::decode(const std::vector<std::uint8_t>& payload, HidraFersEvent& event) const {
  static_assert(sizeof(FERS_spect_64_packed) == 705);

  int board_packet_size = (int)sizeof(FERS_spect_64_packed);

  if (payload.size() % board_packet_size != 0) {
    HIDRA_ERROR("Unexpected FERS payload size {}. Should be a multiple of {}. Only first packet will attempt decoding",
                payload.size(),
                board_packet_size);
  }

  const int FERS_N_CHAN_MAX = 64 * 20;

  std::vector<double> FERStsamp_us(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERSrel_tsamp_us(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERStrigger_id(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERSboard_id(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERShg(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERSlg(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERStoa(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERStot(FERS_N_CHAN_MAX, -1);

  int nboards = payload.size() / board_packet_size;
  for (int iboard = 0; iboard < nboards; ++iboard) {

    if (payload.size() % board_packet_size != 0 &&
        iboard > 0) { // to avoid troubles in case of wrong payload size, but still trying to decode first block
      break;
    }

    const auto* block_ptr = payload.data() + iboard * board_packet_size;
    FERS_spect_64_packed boardblock;
    std::memcpy(&boardblock, block_ptr, sizeof(FERS_spect_64_packed));

    if (boardblock.board_id < 0 || boardblock.board_id >= 20) {
      HIDRA_ERROR("FERS block {} has invalid board_id {}. Skipping", iboard, boardblock.board_id);
      continue;
    }
    uint64_t allchan_mask = std::numeric_limits<uint64_t>::max();

    if (boardblock.chmask != allchan_mask) {
      HIDRA_WARN("FERS block {} has chmask {:016X}. Refusing to decode if less then 64 channels are enabled",
                 iboard,
                 boardblock.chmask);
      continue;
    }

    for (int ichan = 0; ichan < 64; ++ichan) {
      int index = boardblock.board_id * 64 + ichan;
      FERStsamp_us[index] = boardblock.tstamp_us;
      FERSrel_tsamp_us[index] = boardblock.rel_tstamp_us;
      FERStrigger_id[index] = static_cast<double>(boardblock.trigger_id);
      FERSboard_id[index] = static_cast<double>(boardblock.board_id);
      FERShg[index] = static_cast<double>(boardblock.energyHG[ichan]);
      FERSlg[index] = static_cast<double>(boardblock.energyLG[ichan]);
      FERStoa[index] = static_cast<double>(boardblock.tstamp[ichan]);
      FERStot[index] = static_cast<double>(boardblock.ToT[ichan]);
    }

  } // loop over board blocks

  event.FERStsamp_us = std::move(FERStsamp_us);
  event.FERSrel_tsamp_us = std::move(FERSrel_tsamp_us);
  event.FERStrigger_id = std::move(FERStrigger_id);
  event.FERSboard_id = std::move(FERSboard_id);
  event.FERShg = std::move(FERShg);
  event.FERSlg = std::move(FERSlg);
  event.FERStoa = std::move(FERStoa);
  event.FERStot = std::move(FERStot);
}

} // namespace hidra