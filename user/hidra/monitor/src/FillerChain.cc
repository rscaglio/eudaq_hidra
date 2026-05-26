#include "FillerChain.hh"

void FillerChain::Fill(const HidraEvent& ev) {
    // A single lock for the whole chain. From the pump thread's perspective,
    // either all Fill() of this event have happened, or none.
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& filler : m_fillers)
        filler->Fill(ev);
}
