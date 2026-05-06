#ifndef HIDRA_FERS2_CONFIGURATION_H
#define HIDRA_FERS2_CONFIGURATION_H

#include <map>
#include <string>
#include <vector>

namespace hidra {
namespace fers2 {

/**
 * @brief Simple representation of a FERS configuration file.
 *
 * This class models the minimal information extracted from a CAEN
 * FERSlib configuration file that the rest of the `fers2` module needs:
 * - the original source filename
 * - the list of device connection "Open[n] = ..." paths
 * - a set of default parameters (key/value) from the file
 * - optional per-board overrides stored by board id
 *
 * Parsing and loading is implemented in the corresponding .cc file.
 */
class FERSConfiguration {
public:
  /**
   * @brief Parse a configuration file into a FERSConfiguration object.
   *
   * @param path Path to the FERSlib-style configuration file.
   * @param out Pointer to an existing FERSConfiguration to populate.
   * @param error Optional output string which receives a human-friendly
   *              error message on failure.
   * @return true on success, false on parse or I/O error.
   */
  static bool FromFile(const std::string& path, FERSConfiguration* out, std::string* error = nullptr);

  /**
   * @brief Load the parsed configuration into the FERS library runtime.
   *
   * This typically calls into the CAEN-provided FERS library to register
   * parameters. The library may keep its own internal copy of parameters;
   * this method merely mirrors the parsed values into the library.
   *
   * @param error Optional output string which receives a human-friendly
   *              error message on failure.
   * @return true on success, false on failure.
   */
  bool LoadIntoLibrary(std::string* error = nullptr) const;

  /// Path of the source configuration file as parsed.
  const std::string& source_file() const { return source_file_; }

  /// List of connection strings extracted from the file (Open[0], Open[1], ...)
  const std::vector<std::string>& open_paths() const { return open_paths_; }

  /// Map of default parameter names to their string values.
  const std::map<std::string, std::string>& default_params() const { return default_params_; }

  /**
   * @brief Set a per-board override parameter.
   *
   * The override map is small and intended for test-time tweaks or for
   * programmatic changes to the configuration after parsing.
   */
  void SetBoardOverride(int board_id, const std::string& param_name, const std::string& value);

  /**
   * @brief Build the effective parameter set for a given board id.
   *
   * Returns the union of `default_params()` and any board-specific overrides
   * (overrides take precedence).
   */
  std::map<std::string, std::string> EffectiveParamsForBoard(int board_id) const;

private:
  std::string source_file_;
  std::vector<std::string> open_paths_;
  std::map<std::string, std::string> default_params_;
  std::map<int, std::map<std::string, std::string>> board_overrides_;
};

} // namespace fers2
} // namespace hidra

#endif // HIDRA_FERS2_CONFIGURATION_H
