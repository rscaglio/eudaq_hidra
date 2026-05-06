#ifndef HIDRA_FERS2_BOARD_MANAGER_H
#define HIDRA_FERS2_BOARD_MANAGER_H

#include <cstddef>
#include <string>
#include <vector>

#include "FERSBoard.h"
#include "FERSConfiguration.h"
#include "FERSTypes.h"

namespace hidra {
namespace fers2 {

/**
 * @brief Manager for a collection of `FERSBoard` instances.
 *
 * Responsibilities:
 *  - Instantiate boards from a parsed `FERSConfiguration` (Open[n] entries).
 *  - Coordinate connect/configure/start/stop operations across all boards.
 *  - Provide a simple polling interface to collect events from every board.
 *
 * The class favors simplicity over concurrency; callers typically call
 * `ReadAvailableEvents()` from a single-threaded producer loop and then
 * perform any trigger-alignment at the producer level.
 */
 class FERSBoardManager {
 public:
   /**
    * Create board objects for each Open[n] entry in `config`.
    * @param first_board_id Logical starting id assigned to the first board.
    */
   bool BuildBoardsFromConfiguration(const FERSConfiguration& config, int first_board_id = 0);

   /**
    * Add a single board manually.
    * @param board_id Logical id for the board.
    * @param connection_path Open[n]-style connection string.
    * @param error Optional human-readable error output.
    */
   bool AddBoard(int board_id, const std::string& connection_path, std::string* error = nullptr);

   /**
    * Connect all configured boards (open devices and prepare readout).
    */
   bool ConnectAll(int readout_mode = 0, std::string* error = nullptr);

   /**
    * Configure all boards using `config`. When `load_config_file_first` is
    * true the manager will call `config.LoadIntoLibrary()` before applying
    * per-board parameter sets.
    */
   bool ConfigureAll(const FERSConfiguration& config,
                     int configure_mode,
                     bool load_config_file_first = true,
                     std::string* error = nullptr);

   bool StartAll(int start_mode, int run_number, std::string* error = nullptr);
   bool StopAll(int start_mode, int run_number, std::string* error = nullptr);
   bool DisconnectAll(std::string* error = nullptr);

   /**
    * Poll every board and return all events collected. The returned vector
    * aggregates events from all boards; events are not guaranteed to be
    * ordered by trigger id across boards — alignment should be performed
    * by the caller if required.
    */
   std::vector<FERSEvent> ReadAvailableEvents(size_t max_events_per_board = 0, std::string* error = nullptr);

   FERSBoard* FindBoard(int board_id);
   const std::vector<FERSBoard>& boards() const { return boards_; }

 private:
   std::vector<FERSBoard> boards_;
 };

} // namespace fers2
} // namespace hidra

#endif // HIDRA_FERS2_BOARD_MANAGER_H
