/* KonaOS namespace acceptance test contract (test artifact).
 *
 * Shared by the `nsdev` synthetic publisher (tests/target/ns/nsdev.c), its
 * driver manifest (tests/ns/manifests/nsdev.json), the client program
 * (tests/ns/ns.c) and the simulator replay.  It is the namespace
 * conformance protocol, never the native ABI: the object lifecycle, the
 * argument blocks and the FILE protocol come from abi/kos/object.h,
 * abi/kos/ipc.h and abi/kos/protocol.h, and every constant here belongs to
 * the acceptance set.
 *
 * The image boots the real SIO and console drivers plus this one
 * synthetic publisher, so the DEVICE provider's table carries production
 * metadata (`/dev/serial/0`, `/dev/console`) beside two test-only names
 * that give the rehearsal a second and third protocol type and a second
 * grouping container:
 *
 *   /dev/input/keyboard  INPUT  -> one endpoint of the synthetic driver
 *   /dev/video           VIDEO  -> the same endpoint, another name
 *
 * The endpoint's dispatch answers three operations, so a call that
 * arrives through a bound handle proves both the routing and the frame
 * hand-over:
 *
 *   NS_OP_PING       arg0 = a word; returns it back (payload echoed)
 *   NS_OP_WHOAMI     returns NS_MAGIC
 *   NS_OP_DESC       returns the frame's descriptor id
 */

#ifndef KONAOS_TARGET_NS_TEST_H
#define KONAOS_TARGET_NS_TEST_H

#include "kos/ipc.h"
#include "kos/object.h"
#include "kos/types.h"

/* Test-only driver class: 1-8 are the production driver classes
 * (abi/kos/protocol.h), 16+ is the IPC image's synthetic set. */
#define NS_CLASS_DEV 30u

/* Every synthetic endpoint exports version 1, instance 0. */
#define NS_ENDPOINT_VERSION 1u

#define NS_OP_PING   0u /* payload[0..1] echoed; returns NS_MAGIC         */
#define NS_OP_WHOAMI 1u /* returns NS_MAGIC                                */
#define NS_OP_DESC   2u /* returns the frame's descriptor id               */

/* The synthetic publisher's fixed reply: 'N' 'S'. */
#define NS_MAGIC 0x4E53u

/* The publication objects the manifest declares (host metadata; a bank-6
 * endpoint serves one object per publication). */
#define NS_OBJECT_KEYBOARD 1u
#define NS_OBJECT_VIDEO    2u

#endif /* KONAOS_TARGET_NS_TEST_H */
