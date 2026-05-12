/*
 * Intentionally empty. Shadows libwally-core's amalgamation include of
 * secp256k1.c so the wally translation unit does NOT compile secp256k1
 * inline. The real secp256k1 lives in its own static library target
 * (see secp256k1 in app/src/main/cpp/CMakeLists.txt) and is linked in
 * at the kern_android.so step.
 *
 * This directory must come first on wally's include path so that
 *   #include "src/secp256k1/src/secp256k1.c"
 * in libwally-core/upstream/src/amalgamation/combined.c hits this file
 * rather than the upstream one.
 */
