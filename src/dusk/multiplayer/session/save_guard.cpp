#include "save_guard.hpp"

#include <chrono>
#include <cstdlib>

#include "dusk/logging.h"
#include "dusk/ui/ui.hpp"

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

/**
 * ★ Test knob: DUSK_MP_TEST_SAVE_REFUSAL=1 exercises the refusal path without a session.
 *
 * The dangerous half of this feature is not "does the write get dropped" — that is one branch and
 * an md5 proves it. It is whether a DROPPED write leaves a caller waiting forever for a card
 * command that no longer exists (see consume_refused_card_write in the header). That path lives
 * behind the in-game save prompt, which a scripted run cannot reach, so without this knob the
 * softlock question could only ever be reasoned about rather than run.
 *
 * It forces only refuse_card_write(), NOT save_would_be_contaminated() — so on an instance that has
 * not joined a session (the host, or a single instance) the autosave predicate still passes and the
 * write is then refused at the choke point. That is exactly the sequence the save prompt would
 * produce, driven by a state machine that also polls SaveSync (autosave.cpp:112). Autosave fires on
 * every stage entry and every shutter door, so a run under this knob refuses many writes and would
 * wedge visibly if the completion latch were wrong. Setting it on a guest proves nothing: there the
 * predicate short-circuits first, which is the whole point of the predicate.
 *
 * Read once. Never set in normal play; a run with it on writes no save at all.
 */
bool test_refusal_forced() {
    static const bool forced = [] {
        const char* value = std::getenv("DUSK_MP_TEST_SAVE_REFUSAL");
        return value != nullptr && value[0] == '1';
    }();
    return forced;
}

/**
 * Sticky for the life of the process. See the header for why this is deliberately never cleared on
 * disconnect: the contamination is in g_dComIfG_gameInfo, and that outlives the socket.
 */
bool sContaminated = false;

/**
 * Rate limit for the player-facing toast. A save that is refused tends to be retried — the in-game
 * save prompt is a menu the player can mash — and a toast per attempt would bury the message it is
 * trying to deliver.
 */
constexpr std::chrono::seconds kToastCooldown{10};

std::chrono::steady_clock::time_point sLastToast{};
bool sToastedOnce = false;

/// Set by refuse_card_write, consumed by SaveSync so the caller's wait terminates. See the header.
bool sRefusedWritePending = false;

}  // namespace

bool save_would_be_contaminated() {
    return sContaminated;
}

void mark_save_contaminated(const char* i_reason) {
    if (sContaminated) {
        return;
    }

    sContaminated = true;
    Log.warn("Save ownership: this machine will not write its own memory card again this run — {}. "
             "The host owns the save for a co-op session; a guest's own .gci is left untouched "
             "rather than overwritten with the host's world. Restart Dusklight to save normally.",
        i_reason != nullptr ? i_reason : "reason not given");
}

bool refuse_card_write() {
    if (!sContaminated && !test_refusal_forced()) {
        return false;
    }

    // Whoever asked is waiting on SaveSync; make sure that wait can end.
    sRefusedWritePending = true;

    const auto now = std::chrono::steady_clock::now();
    if (!sToastedOnce || now - sLastToast >= kToastCooldown) {
        sToastedOnce = true;
        sLastToast = now;
        dusk::ui::push_toast({
            .title = "Saving is disabled",
            .content = "The host owns this world's save. Your own file is untouched.",
            .duration = std::chrono::seconds(6),
        });
        Log.warn("Refused a memory card write: this process joined a co-op session, so its game "
                 "state belongs to the host. Nothing was written.");
    }

    return true;
}

bool consume_refused_card_write() {
    if (!sRefusedWritePending) {
        return false;
    }

    sRefusedWritePending = false;
    return true;
}

}  // namespace dusk::mp
