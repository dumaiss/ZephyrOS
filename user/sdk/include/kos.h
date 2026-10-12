/* KonaOS user SDK umbrella header (work/28c-m9-user-sdk).
 *
 * A native program includes only "kos.h"; it never includes a kernel
 * header and never writes entry assembly.  The SDK re-exports the public
 * native ABI types (via the ABI's kos headers) and declares the libkos
 * wrappers and the klibc subset.  See docs/sdk.md.
 */

#ifndef KOS_H
#define KOS_H

#include "kos/console.h"
#include "kos/fs.h"
#include "kos/ipc.h"
#include "kos/process.h"
#include "kos/seg.h"
#include "kos/xmem.h"
#include "klibc/string.h"
#include "klibc/fmt.h"

#endif /* KOS_H */
