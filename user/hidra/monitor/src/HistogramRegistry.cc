#include "HistogramRegistry.hh"

#include <TDirectory.h>
#include <TFile.h>

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

bool HistogramRegistry::SaveToFile(const std::string& filepath) const {
    // Preserve the caller's current ROOT directory: TFile's constructor makes
    // the new file the current directory, and we must not leak that side effect.
    TDirectory* prev = gDirectory;

    TFile file(filepath.c_str(), "RECREATE");
    if (file.IsZombie()) {
        if (prev)
            prev->cd();
        return false;
    }

    // TH1::Write() serialises into the current directory (the file opened above)
    // without attaching the histogram to it, so the registry keeps ownership.
    for (auto& [_, h] : m_histograms)
        h->Write();

    file.Close();
    if (prev)
        prev->cd();
    return true;
}