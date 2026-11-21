/* LINE "nova" SDK (a login / friends service for the Japanese market). The offline build never signs in: every entry point is a logging stub that returns 0
 * (null / false / no callback). Generated from the import list of libg.so by tools/gen_line_stubs.py -- the names below are the C++ mangled imports.
 */
#include "../loader/log.h"
#include "shim.h"

static int line_stub(void) {
    BV_TRACE(1, "LINE SDK call ignored (offline build)");
    return 0;
}

const bv_shim bv_shims_line[] = {
    {"_ZN8linecorp4nova11AuthManager11verifyTokenERKNSt6__ndk18functionIFvbPKNS0_11CredentialsEPKNS0_5ErrorEEEE", (const void *)&line_stub},
    {"_ZN8linecorp4nova11AuthManager12refreshTokenERKNSt6__ndk18functionIFvbPKNS0_11CredentialsEPKNS0_5ErrorEEEE", (const void *)&line_stub},
    {"_ZN8linecorp4nova11AuthManager5loginERKNSt6__ndk18functionIFvbPKcPKNS0_11CredentialsEPKNS0_5ErrorEEEE", (const void *)&line_stub},
    {"_ZN8linecorp4nova11AuthManager6logoutERKNSt6__ndk18functionIFvbPKNS0_5ErrorEEEE", (const void *)&line_stub},
    {"_ZN8linecorp4nova12GraphService10getFriendsERKNSt6__ndk18functionIFvbPKNS0_5graph5model21ProfilePagingResponseEPKNS0_5ErrorEEEEii", (const void *)&line_stub},
    {"_ZN8linecorp4nova12GraphService10getProfileERKNSt6__ndk18functionIFvbPKNS0_5graph5model7ProfileEPKNS0_5ErrorEEEE", (const void *)&line_stub},
    {"_ZN8linecorp4nova7NovaSDK10initializeERKNS0_17NovaConfigurationE", (const void *)&line_stub},
    {"_ZN8linecorp4nova7NovaSDK11getInstanceEv", (const void *)&line_stub},
    {"_ZN8linecorp4nova7NovaSDK14getAuthManagerEv", (const void *)&line_stub},
    {"_ZN8linecorp4nova7NovaSDK15getGraphServiceEv", (const void *)&line_stub},
    {"_ZNK8linecorp4nova11AuthManager10isLoggedInEv", (const void *)&line_stub},
    SHIM_END};
