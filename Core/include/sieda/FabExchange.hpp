// SiEDA Core — intelligent fabrication data: IPC-2581 (revision C, one XML file) and ODB++ (v7 job as .tgz). Both are
// written from the same per-layer feature list (copper with nets, pours, solder mask, silkscreen, drills), with the
// board profile, packages, placed components, nets and the BOM, so the fab and assembly house get one consistent set.
#pragma once

#include <string>

#include "sieda/Project.hpp"

namespace sieda {

/// IPC-2581C XML: Content (dictionaries), LogisticHeader, HistoryRecord, Bom and Ecad (layers, drill spans, profile,
/// packages, components, logical nets, layer features). Units: millimetres, Y up (the board's Y is flipped).
std::string exportIpc2581(const Project& project);

/// ODB++ v7 job "<name>/..." as a gzip-compressed tar (.tgz): matrix, misc/info, step "pcb" with profile, every layer's
/// features, component layers and eda/data (nets, packages). Binary: write it as bytes.
std::string exportOdbArchive(const Project& project);

}  // namespace sieda
