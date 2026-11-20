/*
 * Template CVN-C controller for the OpenItsContainer SDK.
 *
 * This single C translation unit is the integration boundary between your
 * controller and the container. It must:
 *   1. Size and allocate the CIF buffers (the CIF_AANT_* / CIF_MAX_* sizes
 *      below + CIF_PUBLIC + #include "cif/CIF_INC.h").
 *   2. Implement applicatieprogramma() — your control logic, called every tick.
 *   3. Implement getCifItemName() — map TLC-FI object ids to CIF array indices.
 *
 * Replace the example object names and sizes with your intersection's, and put
 * your control logic in applicatieprogramma(). Link your vendor's prebuilt
 * .libs via add_cvnc_controller(... CONTROLLER_LIBS your_vendor.lib).
 *
 * NOTE: this is pure C. A C++ controller must additionally pre-declare the
 * CIF_PB_AANT_* counts as `extern "C" { extern const short ...; }` before the
 * CIF_PUBLIC include (see the SDK README) — C gives those external linkage for
 * free, C++ does not.
 */

/* --- 1. CIF buffer sizes (must match your real object counts) --- */
#define CIF_AANT_US_FC      2   /* signal groups  -> CIF_GUS[] / CIF_WUS[] */
#define CIF_AANT_US_OV      1   /* other outputs  -> CIF_GUS[] / CIF_WUS[] */
#define CIF_AANT_IS_D       2   /* detectors      -> CIF_IS[] */
#define CIF_AANT_IS_OV      1   /* other inputs   -> CIF_IS[] */
#define CIF_AANT_DSI        36
#define CIF_AANT_GPS        10
#define CIF_AANT_WPS        10
#define CIF_AANT_KLOK       10
#define CIF_AANT_PARM1      1
#define CIF_AANT_PARM2      1
#define CIF_MAX_UBER        4096
#define CIF_MAX_IBER        4096
#define CIF_MAX_MON_IBER    1
#define CIF_MAX_MON_UBER    4096
#define CIF_MAX_RIS_UBER    4096
#define CIF_MAX_FILE_UBER   16384
#define CIF_MAX_FILEEXT     4

#define CIF_PUBLIC
#include "cif/CIF_INC.h"

#include "cif/CifController.h"

#include <stddef.h>

/* --- 3. Object name tables (TLC-FI id <-> CIF index, type-local) --- */
static const char* SIGNAL_GROUPS[CIF_AANT_US_FC] = { "01", "02" };
static const char* OUTPUTS[CIF_AANT_US_OV]       = { "out1" };
static const char* DETECTORS[CIF_AANT_IS_D]      = { "d1", "d2" };
static const char* INPUTS[CIF_AANT_IS_OV]        = { "in1" };

const char* getCifItemName(CifObjectType type, int index)
{
    switch (type) {
        case CIF_OBJ_SIGNALGROUP:
            return (index >= 0 && index < CIF_AANT_US_FC) ? SIGNAL_GROUPS[index] : NULL;
        case CIF_OBJ_OUTPUT:
            return (index >= 0 && index < CIF_AANT_US_OV) ? OUTPUTS[index] : NULL;
        case CIF_OBJ_DETECTOR:
            return (index >= 0 && index < CIF_AANT_IS_D) ? DETECTORS[index] : NULL;
        case CIF_OBJ_INPUT:
            return (index >= 0 && index < CIF_AANT_IS_OV) ? INPUTS[index] : NULL;
        default:
            return NULL;
    }
}

/* --- 2. Control logic ---
 * Called once with CIF_INIT at startup, then every tick (~100ms) with 0.
 * Read inputs/detectors from CIF_IS[], write desired signal-group states to
 * CIF_GUS[]. This template does nothing; replace with your control logic.
 */
s_int16 applicatieprogramma(s_int16 reden)
{
    (void)reden;
    return 0;
}
