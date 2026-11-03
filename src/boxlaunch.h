#ifndef BOXLAUNCH_H
#define BOXLAUNCH_H

#include "launchparams.h"   // ContainerParams

class RegistryWrapper;

// ---------------------------------------------------------------------------
// BoxLaunch — a launch's steps around its Sandboxie box prefix (BoxPrefix) on Windows: Prepare before the game, Harvest
// after it. Elsewhere both do nothing (a Linux game has no box).
// ---------------------------------------------------------------------------
namespace BoxLaunch {

// Once the runtime is mounted and its edits applied: defines the launch's box; saves what a box left by an unfinished
// launch kept (see Discard); empties the box; fills it with everything the runtime has outside the game folder
// (BoxPrefix::Materialise); imports the package's registry into it — its base edits, then the kept keys from the
// instance's store, then its override edits (the order a Wine prefix gets them in); last, marks it as this launch's
// (CP.BoxPrepared). False when the game must NOT start: the unfinished session could not all be stored, or the box could
// not be emptied or marked. What the package places that could not be placed is said, and the game still starts.
bool Prepare(struct ContainerParams &CP);

// After the game, before the usual capture, and only for the box this launch prepared: ends what still runs in the box,
// copies the kept folders and files back to the runtime (BoxPrefix::SyncKeepsBack), where the capture finds them, and
// reads the kept registry keys out of the box into Session. HaveSession is false when the launch prepared no box (the
// capture then reads the runtime's hives, as for a Wine prefix). False when something kept was NOT brought back.
bool Harvest(struct ContainerParams &CP, RegistryWrapper &Session, bool &HaveSession);

// After the capture has stored what Harvest brought back (call it only then): empties the box this launch prepared.
// Until then the box is the session's only copy of what the game kept outside its game folder, so a launch that stops
// before (VidyaGod or the machine stopped, or a capture failed) leaves it, and the next launch's Prepare saves it first.
void Discard(struct ContainerParams &CP);

} // namespace BoxLaunch

#endif // BOXLAUNCH_H
