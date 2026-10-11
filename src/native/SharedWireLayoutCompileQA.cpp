// QA-only compile unit: actual new wire header executes its own size/offset
// assertions; existing public contracts receive independent checks here.
#include <cstddef>
#include "VRSIMNative.h"
#include "SharedStereoWire.h"
#include "GpuProjectionPass.h"
#include "SharedStereoBridge.h"
#ifdef VRSIM_SHARED_3072
static_assert(VRSIM::kStereoBridgeWidthV1==3072u && VRSIM::kStereoBridgeHeightV1==3072u,"compiled3072edge" );
#else
static_assert(VRSIM::kStereoBridgeWidthV1==1536u && VRSIM::kStereoBridgeHeightV1==1536u,"compiled1536edge" );
#endif
static_assert(sizeof(VRSIM::ProjectionPlanV1)==56,"Plan56 changed");
static_assert(offsetof(VRSIM::ProjectionPlanV1,tanL)==8,"Plan offset changed");
static_assert(offsetof(VRSIM::ProjectionPlanV1,ty)==48,"Plan tail offset changed");
static_assert(sizeof(VRSIM::GpuPipelineStatsV1)==120,"Stats120 changed");
static_assert(offsetof(VRSIM::GpuPipelineStatsV1,sessionEpoch)==112,"Stats epoch changed");
static_assert(sizeof(VRSIM::SharedPreparedPairV1)==64,"Ticket64 changed");
static_assert(offsetof(VRSIM::SharedPreparedPairV1,epoch)==16,"Ticket epoch changed");
static_assert(offsetof(VRSIM::SharedPreparedPairV1,format88)==56,"Ticket tail changed");
static_assert(sizeof(VRSIM::SharedBridgeStatsV1)==72,"BridgeStats72 changed");
static_assert(offsetof(VRSIM::SharedBridgeStatsV1,initCount)==24,"BridgeStats alignment changed");
static_assert(offsetof(VRSIM::SharedBridgeStatsV1,quarantineCount)==64,"BridgeStats tail changed");