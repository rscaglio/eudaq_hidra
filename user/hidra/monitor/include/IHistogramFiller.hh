#pragma once

#include "HidraEvent.hh"

/**
 * @brief Interface for histogram fillers.
 * 
 * Implementations of this interface are responsible for filling histograms based on unpacked EUDAQ events.
 * Each filler typically focuses on a specific subsystem or "view" of the data (e.g., summary histograms, special channels, correlations).
 * The filler:
 *  • Registers its histograms in the constructor (via HistogramRegistry::Add) and keeps pointers to them as members for direct use in Fill().
 * • Does NOT handle any locking. Synchronization is the responsibility of FillerChain, which acquires the shared mutex before calling Fill().
 * • Does NOT own the event: it reads from it and returns.
 * 
 */
class IHistogramFiller {
public:
    virtual void Fill(const HidraEvent& ev) = 0;

    // Called by FillerChain::Reset() when the histogram registry is cleared (e.g. DoReset).
    // Override to reset any filler-internal state that is relative to run start (e.g. time references).
    // The caller (DoReset) already holds the histogram mutex.
    virtual void Reset() {}

    virtual ~IHistogramFiller() = default;
};
