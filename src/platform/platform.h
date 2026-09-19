/*
 * octemu — the host-specific surface, in one header.
 *
 * Everything the emulator needs from the operating system that is not POSIX
 * lives behind these four calls. main.c holds the POLICY (when a disk is handed
 * over, when it is taken back, whether to steal focus, whether --midi was
 * asked for) and prints everything the user sees; a port holds only the
 * mechanism. No port should print anything except the one case noted below.
 *
 * ONE FILE PER PLATFORM, named after `uname` in lower case:
 * src/platform/darwin.c today. There is deliberately NO default
 * implementation: a platform that has not been ported fails to build with
 * "no such file: src/platform/linux.c", which names the file to write. A
 * stubbed-out no-op would instead boot, look like it worked, and silently
 * never hand the card to the host.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OT_PLATFORM_H
#define OT_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Bring this process's window to the front, over the terminal it was launched
 * from. Called at most once, only for a windowed run, and only when a person
 * (not a harness) launched the emulator — main.c decides which.
 *
 * A port with no notion of application activation may do nothing: this is the
 * one call whose absence is cosmetic, so a silent no-op is honest here.
 */
void platform_activate_ui(void);

/*
 * USB DISK MODE: hand the card image to the HOST as a mounted disk, and take
 * it back again. `image` is a raw MBR/FAT32 disk image file; the guest has
 * already unmounted its own filesystem, so no coordination beyond this is
 * needed.
 *
 * attach writes the host's name for the attached device — the whole disk, not a
 * slice of it (e.g. "/dev/disk4") — into `dev`, at most `cap` bytes including
 * the NUL, and returns true. On failure it returns false and leaves `dev` an
 * empty string: main.c reports that and carries on, because a card the host
 * could not mount is no reason to kill a running emulator. detach takes a
 * string a previous attach produced and returns true if the host let go of it.
 *
 * A port must guarantee:
 *   - attach is IDEMPOTENT. main.c will not call it twice without a detach in
 *     between, but a duplicate 'attach' line on the wire must not produce two
 *     host mounts of one file.
 *   - NEITHER CALL BLOCKS THE RUN LOOP for long. They are made from the main
 *     loop, between panel frames, so the guest keeps running while the host
 *     mounts; a few hundred milliseconds is the budget (the Darwin port spends
 *     one hdiutil process). Anything slower than that belongs on a thread
 *     inside the port, with attach returning the device name it will have.
 *   - detach is safe to call from the exit path, after the guest is gone.
 *     main.c calls it there if a run dies while still host-mounted, and an
 *     un-ejected image outlives the process.
 *   - neither call writes to the image. The host mount is the host's; the
 *     guest's writes are the guest's, and DISK MODE is what separates them.
 */
bool platform_disk_attach(const char *image, char *dev, size_t cap);
bool platform_disk_detach(const char *dev);

/*
 * --midi: publish the guest's MIDI DIN port as two virtual endpoints on the
 * host, named "<name>" (the Octatrack's MIDI OUT, a source the host reads)
 * and "<name> In" (its MIDI IN, a destination the host writes). `sock_path`
 * is the unix socket QEMU gives UART0 — a transparent byte stream at the
 * guest's 31250 baud. A NULL `name` means "Octatrack Emulator".
 *
 * This is the one call that may print: it returns false HAVING SAID WHY on
 * stderr, because the reasons are all host-specific (no such endpoint API, the
 * socket never appeared, the MIDI server refused) and only the port knows
 * which. main.c then runs on without a MIDI bridge rather than failing a run
 * that is otherwise fine, so a port with no MIDI support is a one-line
 * function that says so and returns false — NOT a `return true` that leaves
 * the user hunting for endpoints that were never created.
 *
 * Must not block: it owns whatever threads it needs, and the SDL loop is not
 * one of them.
 */
bool platform_midi_start(const char *sock_path, const char *name);

#endif
