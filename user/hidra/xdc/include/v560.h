#include <cstdint>

#define v560Checkword 0xB0BFE22A

enum V560CHAN {
  sFastGate = 0,
  sIsPhys = 1,
  sIsPed = 2,
  sWW = 3,
  sEndOfSpill = 4,
  sV792Gate = 8,
  sTDCCommStop = 9,
  sV862Gate = 10,
  sLeakageGate = 11
};


struct V560Data {
  uint32_t CHECKWORD1 = 0xB0BFE22A;
  uint32_t fastGateC = 0;
  uint32_t physTrigC = 0;
  uint32_t pedTrigC = 0;
  uint32_t wwC = 0;
  uint32_t endOfSpillC = 0;
  uint32_t v792GateC = 0;
  uint32_t TDCCommStopC = 0;
  uint32_t V862GateC = 0;
  uint32_t LeakageGateC = 0;
};