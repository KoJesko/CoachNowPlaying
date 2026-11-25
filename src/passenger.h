#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace ep {

// Passenger information for coaches:
//  - the TV at the front ceiling (MAN Lion's Coach, CarPlay mod: /material/cpc/pax_tv.dds, BC3 1024x760, 1 mip) shows
//    destination, arrival time, clock, remaining distance and a progress bar (drawn by eta_screen.cpp via PaxDraw)
//  - a few minutes before arrival a chime plays and a voice announces the destination (Windows speech, SAPI)
// Telemetry input:
void PaxSetJob(const std::string& sourceCity, const std::string& destinationCity, float plannedKm);  // empty = no job
// Coach job (bus_job config, Coaches DLC): the stop the bus is driving to now. Empty name = no coach job.
// stage = bus.stages.current, finalStop = this is the last stage of the line.
void PaxSetBusStop(const std::string& nextStop, int stage, bool finalStop);
void PaxSetVehicle(const std::string& idText);  // brand id + id + name of the player's vehicle
void PaxSetTimeScale(float scale);              // in-game seconds per real second

// State for the TV picture; key changes whenever the picture has to be redrawn.
struct PaxView {
    std::wstring source, destination;
    float progress = -1.0f;  // 0..1 along the job, < 0 = unknown
    bool arriving = false;   // within the announcement window
    std::wstring key;
};
PaxView PaxGetView(uint32_t gameMinutes, bool haveTime, float navSeconds, float navMeters);
// Draws the TV picture (top-down BGRA, w x h).
std::vector<uint8_t> PaxDraw(UINT w, UINT h, const PaxView& v, uint32_t gameMinutes, bool haveTime, float navSeconds,
                             float navMeters);
// Called every frame: starts the announcement when the time has come.
void PaxTick(uint32_t gameMinutes, float navSeconds, float navMeters);

}  // namespace ep
