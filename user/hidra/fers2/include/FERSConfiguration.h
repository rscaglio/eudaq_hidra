#ifndef HIDRA_FERS2_CONFIGURATION_H
#define HIDRA_FERS2_CONFIGURATION_H

#include <map>
#include <string>
#include <vector>

namespace hidra {
namespace fers2 {

class FERSConfiguration {
public:
  static bool FromFile(const std::string& path, FERSConfiguration* out, std::string* error = nullptr);

  bool LoadIntoLibrary(std::string* error = nullptr) const;

  const std::string& source_file() const { return source_file_; }
  const std::vector<std::string>& open_paths() const { return open_paths_; }
  const std::map<std::string, std::string>& default_params() const { return default_params_; }

  void SetBoardOverride(int board_id, const std::string& param_name, const std::string& value);

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
