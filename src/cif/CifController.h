#ifndef TLCCC_CIFCONTROLLER_H_INCLUDED
#define TLCCC_CIFCONTROLLER_H_INCLUDED

/*
 * CVN-C controller mapping contract.
 *
 * A controller hosted in this container must expose its object names so the
 * container can automatically match TLC-FI objects (string ids discovered from
 * the TLC) to CVN-C array indices. This replaces any hand-written id<->index
 * mapping file.
 *
 * The object COUNTS are taken from the standard CVN-C interface constants in
 * CIF_INC.h (CIF_PB_AANT_US_FC / _US_OV / _IS_D / _IS_OV), which a conformant
 * controller sizes to its actual configuration. This header only adds the
 * name lookup.
 */

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Object categories. CVN-C physically stores these in two shared arrays
 * (CIF_GUS[] = signal groups then outputs; CIF_IS[] = detectors then inputs),
 * but the mapping contract differentiates all four.
 */
typedef enum
{
    CIF_OBJ_SIGNALGROUP = 0,
    CIF_OBJ_DETECTOR    = 1,
    CIF_OBJ_INPUT       = 2,
    CIF_OBJ_OUTPUT      = 3
} CifObjectType;

/*
 * Returns the controller's name for the object of `type` at CIF index `index`.
 *
 *  - `index` is TYPE-LOCAL and 0-based (0..count-1 within the type, where count
 *    is the matching CIF_PB_AANT_* constant). Signal groups and detectors start
 *    at index 0 of their array; outputs and inputs are also addressed 0-based
 *    within their own type (the container applies the CVN offset).
 *  - The returned name must match the TLC-FI object id EXACTLY (case-sensitive).
 *  - The returned pointer must remain valid for the lifetime of the process
 *    (point to static storage).
 *  - Return NULL if `index` has no object. For a conformant controller whose
 *    CIF_PB_AANT_* equal its real counts this never happens for a valid index;
 *    the container treats NULL defensively by skipping that slot.
 *
 * MANDATORY for any controller hosted in the container.
 */
const char* getCifItemName(CifObjectType type, int index);

#ifdef __cplusplus
}
#endif

#endif /* TLCCC_CIFCONTROLLER_H_INCLUDED */
