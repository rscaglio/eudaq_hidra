#include "HistogramRegistry.hh"

TH1* HistogramRegistry::Get(std::string_view name) const {
    // A copy of the name is needed to construct the std::string key for lookup
    // This can be avoided using a heterogeneous lookup
    // This is not really needed since this function is not expected to be
    // frequently called,
    auto it = m_histograms.find(std::string(name));
    return it == m_histograms.end() ? nullptr : it->second.get();
}

void HistogramRegistry::Reset() {
    for (auto& [_, h] : m_histograms)
        h->Reset();
}

void HistogramRegistry::ForEach(const std::function<void(TH1*)>& fn) const {
    for (auto& [_, h] : m_histograms)
        fn(h.get());
}