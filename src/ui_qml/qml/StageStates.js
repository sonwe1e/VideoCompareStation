.pragma library

// U03 stage states: one mutually exclusive answer to "what is the review canvas showing
// right now". Pure over the same booleans the existing display surfaces read, so the state
// and the surfaces cannot drift apart:
//
//   empty        first blank - nothing open, nothing pending, no failure
//   opening      a new task is opening and there is no old picture to keep
//   waitingFrame the old picture stays while the next frame / new file is on its way
//   openFailed   the open failed (graphics unavailable, or an error with no kept frame);
//                an error with a kept frame stays here too - the old session is preserved
//                behind the banner
//   missingFrame a frame exists but at least one source's frame for it is missing
//   ready        everything presented, nothing to say
//
// Priority follows the existing overlay stack: failures outrank progress, progress over a
// missing old picture outranks blank, and a missing single source only speaks once a frame
// is actually being shown.

var empty = "empty";
var opening = "opening";
var waitingFrame = "waitingFrame";
var openFailed = "openFailed";
var missingFrame = "missingFrame";
var ready = "ready";

var allStates = [empty, opening, waitingFrame, openFailed, missingFrame, ready];

function resolveStageState(hasContent, graphicsReady, busy, frameShown, hasErrors, waitingForFrame, anySourceMissing) {
    if (!graphicsReady || (hasErrors && !frameShown))
        return openFailed;
    if (busy && !frameShown)
        return opening;
    if (!hasContent)
        return empty;
    if (hasErrors)
        return openFailed;
    if (waitingForFrame || busy)
        return waitingFrame;
    // "Missing" describes a shown frame's per-source gap; without a frame on stage the
    // per-source missing flags carry no meaning (the controller defaults them to true).
    if (anySourceMissing && frameShown)
        return missingFrame;
    return ready;
}
