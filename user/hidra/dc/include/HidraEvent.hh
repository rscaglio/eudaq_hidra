#pragma once

#include "HidraXdcEvent.hh"
#include "HidraFersEvent.hh"
#include "HidraTrackerEvent.hh"
#include "HidraEventMeta.hh"

struct HidraEvent {
    HidraXdcEvent     xdc;
    HidraFersEvent    fers;
    HidraTrackerEvent tracker;
    HidraEventMeta    meta;
};
