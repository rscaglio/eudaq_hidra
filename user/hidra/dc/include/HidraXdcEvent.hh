#pragma once

#include <vector>

struct HidraXdcEvent {
  std::vector<double> ADCvalues;
  std::vector<double> ADCflags;
  std::vector<double> TDCvalues;
  std::vector<double> TDCflags;
};