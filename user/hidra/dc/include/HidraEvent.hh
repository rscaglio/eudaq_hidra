#pragma once

#include "HidraXdcEvent.hh"
#include "HidraFersEvent.hh"

struct HidraEvent {
    HidraXdcEvent  xdc;
    HidraFersEvent fers;
};
