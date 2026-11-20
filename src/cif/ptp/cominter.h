#ifndef COMINTER_H
#define COMINTER_H

/* CCOL AUTOMAAT COM interface.
 *
 * These are the raw byte-transport symbols the CCOL PTP module (ptpcom.c)
 * calls when built for the target controller (AUTOMAAT defined). They are
 * IMPLEMENTED by the OpenItsContainer PTP transport (tlcfi_ptp, see
 * src/cif/ptp/PtpTransport.cpp); this header is only their declaration.
 *
 * In a traditional CCOL build the fabrikant (runtime vendor) supplies both the
 * COM driver and this header. OpenItsContainer IS that runtime, so it ships
 * cominter.h as part of the SDK. Signatures mirror wincom.h (the PC/test
 * variant selected when AUTOMAAT is NOT defined).
 *
 * Contract (see PtpTransport.cpp): portnr is the controller's PTP->PORTNR, an
 * engineer-chosen link key matched against tlcfi.json's ptpLinks[].linkId;
 * lees_char_COM_IBER returns EOB ((char)-1) when the read ring is empty, and
 * the controller polls bezet_COM_IBER before reading.
 */

#ifdef __cplusplus
extern "C" {
#endif

    /* Incoming characters (COM_IBER): socket -> controller */
    void init_COM_IBER(int portnr);
    char lees_char_COM_IBER(int portnr);
    int  bezet_COM_IBER(int portnr);

    /* Outgoing characters (COM_UBER): controller -> socket */
    void init_COM_UBER(int portnr);
    void schrijf_char_COM_UBER(int portnr, char c);

#ifdef __cplusplus
}
#endif

#endif /* COMINTER_H */
