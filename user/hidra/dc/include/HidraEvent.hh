#pragma once

#include "HidraXdcEvent.hh"
#include "HidraFersEvent.hh"
#include "HidraEventMeta.hh"

struct HidraEvent {
    HidraXdcEvent  xdc;
    HidraFersEvent fers;
    HidraEventMeta meta;
};
