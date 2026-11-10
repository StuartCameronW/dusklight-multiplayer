/**
 * d_a_remote_player_event.inc
 * Remote player puppet: the sender's events (agent EV). Spec: .claude/future-plans/puppet-events.md
 * P4 (receiver basics) and P5 (the item held above his head), plus question 4.
 *
 *   - The v21 event byte as received: kind, stand-in, hidden. The pose itself needs nothing here:
 *     talk, chest, item get and door play ALANM animations that block 2 already carries, and the
 *     STB stand-in is an ordinary idle (network_manager.cpp make_stand_in_idle).
 *   - Hidden (the sender's FLG0_PLAYER_NO_DRAW: Midna warp, cannon, boss doors, Henna's boat): the
 *     puppet draws nothing and the name tag goes with it.
 *   - Question 4, Stuart's recommended answer: while THIS end's own Link is in an STB cutscene
 *     (local_event_kind() == Cutscene here), the partner is not drawn either, so he does not stand
 *     around inside the cutscene. Receiver-only; the bridge passes the flag in.
 *   - During an item get, the item's model above his head, as daDitem_c places it.
 *
 * ★ SAVE SAFETY. The receiver never creates the game's item-get actor (daDitem_c). Its actionEvent
 * calls execItemGet (d_a_demo_item.cpp:173), which would grant the item into THIS end's save, and
 * its set_pos hard-reads dComIfGp_getPlayer(0) (:344-381), which is the wrong Link. Only the model
 * is drawn here, from the same archive and BMD index daDitem_c uses (:466-472).
 *
 * Model only: no BTK/BRK/BCK/BTP. Items that have them (the rupees' colour BRK, for one) are
 * logged once each, so the list of what would need the anims comes from real runs.
 *
 * ★ HEAP. The puppet's 0x90000 solid heap is fixed at create and trimmed to fit (create(),
 * f_op_actor_mng.cpp:492), so nothing can be added to it later. The item is not known until the
 * event, so the J3DModel goes on its OWN small solid heap carved from the game heap at runtime,
 * which is what the action override's slot heaps already do after create (daPy_anmHeap_c's
 * mAnimeHeap, destroyed by hand in setupActionOverride). It is sized from daDitem_c's own estimate
 * for model + anims (0x3390, d_a_demo_item.cpp:495) with generous PC headroom (0x10000), then
 * trimmed with mDoExt_adjustSolidHeap exactly as fopAcM_entrySolidHeap trims an actor heap,
 * and destroyed on release. The archive is the game's global, reference-counted one
 * (dComIfG_resLoad), the call daDitem_c itself makes: item archives are never freeAll'ed by
 * anyone, unlike Link's outfit and shield heaps (see mountOwnArchive), so a shared reference is
 * safe here, and the J3DModelData is shared the way every rupee on the ground shares its own.
 *
 * Included once, last, at the end of d_a_remote_player.cpp. All state is zeroed by fopAcM_ct, which
 * is "no event, nothing held".
 */

#include "JSystem/JKernel/JKRSolidHeap.h"
#include "SSystem/SComponent/c_lib.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"

namespace {

/// daDitem_c's offsets, file-static there (d_a_demo_item.cpp:18). Human Link only: the puppet is
/// not a wolf yet as far as this file knows (l_wolf_offset 13,135,57 and l_horse_offset 0,20,54
/// are the other two, :20-22).
const cXyz l_evtItemPlayerOffset(0.0f, 115.0f, 54.0f);

/// daDitem_c::set_mtx's spin, per execute (d_a_demo_item.cpp:405-409).
const s16 l_evtItemSpinStep = 0xFFFF / 250;

/// His field_0x30bc for human Link (d_a_alink_wolf.inc:569): the joint mLeftFootPos is read from
/// (d_a_alink.cpp:5614), whose height set_pos puts under the item (d_a_demo_item.cpp:371-379).
const u16 l_evtLeftFootJointNo = 21;

/// daDitem_c's heap estimate is 0x3390 for model + anims (d_a_demo_item.cpp:495); PC structures run
/// about 2x (create()'s measured fit). Generous, because only the build needs it: the heap is
/// trimmed to what the model used straight after, and that figure is logged.
const u32 l_evtItemHeapSize = 0x10000;

/// The model flags CreateItemHeap uses with no anims (d_a_itembase.cpp:34-35).
const u32 l_evtItemModelFlags = 0x80000;
const u32 l_evtItemDiffFlags = 0x11000084;

/// Edge and load lines per puppet. The event edges are few per session; the cap only stops a
/// flapping sender from flooding the log.
const u16 l_evtLogMax = 120;

const char* evt_kind_name(u8 i_kind) {
    switch (i_kind) {
    case 0:
        return "none";
    case 1:
        return "talk";
    case 2:
        return "chest";
    case 3:
        return "get-item";
    case 4:
        return "door";
    case 5:
        return "in-place";
    case 6:
        return "scripted";
    case 7:
        return "cutscene";
    }
    return "?";
}

/// PlayerEventKind's GetItem (player_state.hpp); this file does not include the wire layout.
const u8 l_evtKindGetItem = 3;

/// daDitem_c::set_mtx does not spin these (d_a_demo_item.cpp:406).
bool evt_item_spins(u8 i_item) {
    return i_item != dItemNo_DUNGEON_EXIT_e && i_item != dItemNo_DUNGEON_BACK_e &&
           i_item != dItemNo_LV7_DUNGEON_EXIT_e && i_item != dItemNo_DUNGEON_EXIT_2_e;
}

}  // namespace

void daRemotePlayer_c::setNetworkEvent(u8 i_kind, bool i_standIn, bool i_hidden, u8 i_item,
    u8 i_itemScale, bool i_localCutscene) {
    const bool hiddenWas = isEventHidden();
    if (i_kind != mNetEvtKind || i_standIn != mNetEvtStandIn || i_hidden != mNetEvtHidden ||
        i_localCutscene != mEvtLocalCutscene)
    {
        if (mEvtLogCount < l_evtLogMax) {
            mEvtLogCount++;
            const std::string item =
                i_kind == l_evtKindGetItem ? fmt::format(", item 0x{:02X}", i_item) : std::string();
            Log.info("Puppet {} event kind {}->{} (standIn {}, hidden {}, local cutscene {}{})",
                mPlayerId, evt_kind_name(mNetEvtKind), evt_kind_name(i_kind), i_standIn ? 1 : 0,
                i_hidden ? 1 : 0, i_localCutscene ? 1 : 0, item);
        }
    }
    mNetEvtKind = i_kind;
    mNetEvtStandIn = i_standIn;
    mNetEvtHidden = i_hidden;
    mEvtLocalCutscene = i_localCutscene;
    // dItemNo_NONE_e: the sender has no daDitem_c partner (yet), so there is nothing to draw.
    mNetEvtItemValid = i_kind == l_evtKindGetItem && i_item != dItemNo_NONE_e;
    mNetEvtItem = mNetEvtItemValid ? i_item : 0;
    mNetEvtItemScale = mNetEvtItemValid ? i_itemScale : 0;
    if (hiddenWas != isEventHidden() && mEvtLogCount < l_evtLogMax) {
        mEvtLogCount++;
        Log.info("Puppet {} {} (sender hidden {}, local cutscene {})", mPlayerId,
            isEventHidden() ? "HIDDEN: draws nothing" : "shown again", mNetEvtHidden ? 1 : 0,
            mEvtLocalCutscene ? 1 : 0);
    }
}

/**
 * Build the item's J3DModel on its own solid heap. CreateItemHeap's model line with no anims
 * (d_a_itembase.cpp:30-53). The archive is resident: dComIfG_resLoad just completed for it.
 */
bool daRemotePlayer_c::buildEventItemModel() {
    const s16 bmd = dItem_data::getBmdName(mEvtItemNo);
    J3DModelData* modelData =
        static_cast<J3DModelData*>(dComIfG_getObjectRes(mEvtItemArc, bmd));
    if (modelData == NULL) {
        Log.warn("Puppet {} item model 0x{:02X}: '{}' has no BMD {}", mPlayerId, mEvtItemNo,
            mEvtItemArc, bmd);
        return false;
    }

    JKRSolidHeap* heap = mDoExt_createSolidHeapFromGame(l_evtItemHeapSize, 0x20);
    if (heap == NULL) {
        Log.warn("Puppet {} item model 0x{:02X}: no game heap for {} bytes", mPlayerId, mEvtItemNo,
            l_evtItemHeapSize);
        return false;
    }
    // fopAcM_callCallback's bracket (f_op_actor_mng.cpp:381-386).
    JKRHeap* oldHeap = mDoExt_setCurrentHeap(heap);
    J3DModel* itemModel =
        mDoExt_J3DModel__create(modelData, l_evtItemModelFlags, l_evtItemDiffFlags);
    mDoExt_setCurrentHeap(oldHeap);
    if (itemModel == NULL) {
        mDoExt_destroySolidHeap(heap);
        Log.warn("Puppet {} item model 0x{:02X}: J3DModel did not fit in {} bytes", mPlayerId,
            mEvtItemNo, l_evtItemHeapSize);
        return false;
    }
    const u32 used = heap->getHeapSize() - heap->getFreeSize();
    mDoExt_adjustSolidHeap(heap);

    mpEvtItemHeap = heap;
    mpEvtItemModel = itemModel;
    mEvtItemAngle = 0;  // daDitem_c is created with a NULL angle (d_a_alink_demo.inc:2322)
    mEvtItemPosed = false;
    mEvtItemLoggedDraw = false;
    dKy_tevstr_init(&mEvtItemTevStr, tevStr.room_no, 0xFF);

    const bool needsAnims = dItem_data::getBtkName(mEvtItemNo) > 0 ||
                            dItem_data::getBrkName(mEvtItemNo) > 0 ||
                            dItem_data::getBckName(mEvtItemNo) > 0 ||
                            dItem_data::getBtpName(mEvtItemNo) > 0;
    if (mEvtLogCount < l_evtLogMax) {
        mEvtLogCount++;
        Log.info("Puppet {} item model 0x{:02X} built from '{}' bmd {}: {} bytes of game heap{}",
            mPlayerId, mEvtItemNo, mEvtItemArc, bmd, used,
            needsAnims ? fmt::format(" — drawn WITHOUT its anims (btk {} brk {} bck {} btp {}, "
                                     "tev frame {})",
                             dItem_data::getBtkName(mEvtItemNo),
                             dItem_data::getBrkName(mEvtItemNo),
                             dItem_data::getBckName(mEvtItemNo),
                             dItem_data::getBtpName(mEvtItemNo),
                             static_cast<int>(dItem_data::getTevFrm(mEvtItemNo))) :
                         std::string());
    }
    return true;
}

/// Drop the model and its heap, and the archive reference. A load still in flight cannot be
/// released (dComIfG_resDelete asserts phase id != 1, d_com_inf_game.cpp:1369, and deleting the
/// dRes_info_c would destroy a live DVD command), so it is orphaned: updateNetworkEvent finishes it
/// and lets go then.
void daRemotePlayer_c::releaseEventItem() {
    if (mpEvtItemHeap != NULL) {
        // The model lives entirely in its heap; nothing else points at it.
        mDoExt_destroySolidHeap(mpEvtItemHeap);
        mpEvtItemHeap = NULL;
    }
    mpEvtItemModel = NULL;
    mEvtItemPosed = false;
    if (!mEvtItemHeld) {
        return;
    }
    if (mEvtItemLoading) {
        mEvtItemOrphan = true;
    } else {
        dComIfG_resDelete(&mEvtItemPhase, mEvtItemArc);
        mEvtItemArc = NULL;
    }
    mEvtItemHeld = false;
    mEvtItemLoading = false;
    if (mEvtLogCount < l_evtLogMax) {
        mEvtLogCount++;
        Log.info("Puppet {} item model 0x{:02X} released{}", mPlayerId, mEvtItemNo,
            mEvtItemOrphan ? " (its load was in flight: the reference drops when it lands)" : "");
    }
}

void daRemotePlayer_c::updateNetworkEvent() {
    // An orphaned load: let it land, then drop the reference. Nothing new starts until it has.
    if (mEvtItemOrphan) {
        const int phase = dComIfG_resLoad(&mEvtItemPhase, mEvtItemArc);
        if (phase == cPhs_COMPLEATE_e) {
            dComIfG_resDelete(&mEvtItemPhase, mEvtItemArc);
            mEvtItemOrphan = false;
            mEvtItemArc = NULL;
        } else if (phase == cPhs_ERROR_e || phase == cPhs_UNK3_e) {
            mEvtItemOrphan = false;
            mEvtItemArc = NULL;
        } else {
            return;
        }
    }

    const bool want = mNetEvtItemValid;
    if (mEvtItemHeld && (!want || mNetEvtItem != mEvtItemNo)) {
        releaseEventItem();
        if (mEvtItemOrphan) {
            return;
        }
    }
    if (!want) {
        mEvtItemFailed = false;
        return;
    }

    if (!mEvtItemHeld) {
        // One attempt per item: a missing archive must not be retried every tick.
        if (mEvtItemFailed && mEvtItemNo == mNetEvtItem) {
            return;
        }
        mEvtItemNo = mNetEvtItem;
        mEvtItemFailed = false;
        const char* arc = dItem_data::getArcName(mEvtItemNo);
        if (arc == NULL || dItem_data::getBmdName(mEvtItemNo) < 0) {
            // daDitem_c::create's own refusal (d_a_demo_item.cpp:476-490).
            mEvtItemFailed = true;
            if (mEvtLogCount < l_evtLogMax) {
                mEvtLogCount++;
                Log.info("Puppet {} item 0x{:02X} has no get-item model (arc {}, bmd {})",
                    mPlayerId, mEvtItemNo, arc != NULL ? arc : "none",
                    dItem_data::getBmdName(mEvtItemNo));
            }
            return;
        }
        mEvtItemArc = arc;
        mEvtItemPhase.mpHandlerTable = NULL;
        mEvtItemPhase.id = 0;
        mEvtItemHeld = true;
        mEvtItemLoading = true;
        if (mEvtLogCount < l_evtLogMax) {
            mEvtLogCount++;
            Log.info("Puppet {} item model 0x{:02X}: loading '{}'", mPlayerId, mEvtItemNo, arc);
        }
    }

    if (mEvtItemLoading) {
        const int phase = dComIfG_resLoad(&mEvtItemPhase, mEvtItemArc);
        if (phase == cPhs_COMPLEATE_e) {
            mEvtItemLoading = false;
            if (!buildEventItemModel()) {
                releaseEventItem();
                mEvtItemFailed = true;
            }
        } else if (phase == cPhs_ERROR_e || phase == cPhs_UNK3_e) {
            // cPhs_UnCompleate reset the phase; there is no completed reference to release.
            mEvtItemHeld = false;
            mEvtItemLoading = false;
            mEvtItemArc = NULL;
            mEvtItemFailed = true;
            Log.warn("Puppet {} item model 0x{:02X}: archive load failed", mPlayerId, mEvtItemNo);
            return;
        }
    }

    if (mpEvtItemModel == NULL) {
        return;
    }
    /* daDitem_c::set_pos for human Link (d_a_demo_item.cpp:344-381): his position with the left
     * foot's height, offset along his facing. Read off this puppet's own body, posed by setMatrix
     * earlier this tick, never off the local Link. */
    cXyz feet = current.pos;
    if (model != NULL && model->getModelData()->getJointNum() > l_evtLeftFootJointNo) {
        feet.y = model->getAnmMtx(l_evtLeftFootJointNo)[1][3];
    }
    cXyz pos;
    cLib_offsetPos(&pos, &feet, shape_angle.y, &l_evtItemPlayerOffset);
    // daDitem_c::set_mtx (:403-414): the sender's scale, the spin, then the base matrix.
    if (evt_item_spins(mEvtItemNo)) {
        mEvtItemAngle += l_evtItemSpinStep;
    }
    const f32 s = mNetEvtItemScale / 255.0f;
    cXyz itemScale(s, s, s);
    mpEvtItemModel->setBaseScale(itemScale);
    mDoMtx_stack_c::transS(pos.x, pos.y, pos.z);
    mDoMtx_stack_c::YrotM(mEvtItemAngle);
    mpEvtItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
    mpEvtItemModel->calc();
    mEvtItemPosed = true;
}

/**
 * Once per draw() call, after its one-entry-per-sim-tick guard, so the item is entered exactly as
 * often as the body (★ Hang 4: an interpolated frame goes through mDoExt_modelEntryDL's diff path,
 * never a second entry). Lit as daDitem_c::setTevStr does (d_a_demo_item.cpp:416-419): its own
 * tevStr, type 14, MAJI. No mirror entry and no shadow: daDitem_c makes neither.
 */
bool daRemotePlayer_c::drawNetworkEvent() {
    if (isEventHidden()) {
        return true;
    }
    if (mpEvtItemModel == NULL || !mEvtItemPosed || mNetEvtItemScale == 0) {
        return false;
    }
    cXyz pos;
    mDoMtx_multVecZero(mpEvtItemModel->getBaseTRMtx(), &pos);
    mEvtItemTevStr.room_no = tevStr.room_no;
    g_env_light.settingTevStruct(14, &pos, &mEvtItemTevStr);
    g_env_light.setLightTevColorType_MAJI(mpEvtItemModel, &mEvtItemTevStr);
    mDoExt_modelEntryDL(mpEvtItemModel);
    if (!mEvtItemLoggedDraw) {
        mEvtItemLoggedDraw = true;
        if (mEvtLogCount < l_evtLogMax) {
            mEvtLogCount++;
            Log.info("Puppet {} item model 0x{:02X} first drawn at ({:.0f}, {:.0f}, {:.0f}), "
                     "scale {}/255, puppet at ({:.0f}, {:.0f}, {:.0f}) facing {}",
                mPlayerId, mEvtItemNo, pos.x, pos.y, pos.z, mNetEvtItemScale, current.pos.x,
                current.pos.y, current.pos.z, shape_angle.y);
        }
    }
    return false;
}

void daRemotePlayer_c::deleteNetworkEvent() {
    releaseEventItem();
    if (mEvtItemOrphan) {
        /* The actor is going and the load cannot be cancelled. The reference stays: the archive
         * remains resident for the session (a few KB), which is the safe side of a destroyed live
         * DVD command. */
        Log.warn("Puppet {} deleted while item archive '{}' was loading: its reference is left "
                 "in place",
            mPlayerId, mEvtItemArc != NULL ? mEvtItemArc : "?");
        mEvtItemOrphan = false;
    }
}
