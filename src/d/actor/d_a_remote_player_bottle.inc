/**
 * d_a_remote_player_bottle.inc
 * Remote player puppet: what is IN the held bottle, and the bottle's own texture animations
 * (puppet-visual-gaps #13, agent H).
 *
 * Dusk-only. Included right after d_a_remote_player_held.inc, whose loaders (held_load_aram_bmd,
 * held_load_aram_anm, held_heap_fits) and bottle model (mpHeldModel[kPlayerHeldBottle]) it uses.
 *
 * Two of daAlink_c's bottle visuals, both set up by setBottleModel (d_a_alink_bottle.inc:50-142):
 *
 *   - the CONTENTS model, his mpHookTipModel for a bottle: the fairy (O_GD_NV) or worm (O_GD_WORM),
 *     each with a looping BCK on his mHookTipBck, or the bee larva (O_GD_HK) with none (:66-82).
 *     setItemMatrix plays the BCK and puts the model on the bottle's own matrix (d_a_alink.cpp:
 *     6053-6057); his draw enters it right after the bottle (:19775-19777).
 *   - the three BTK SLOTS field_0x0718 / 071c / 0720. All three are loaded and entered at once,
 *     slot 1, slot 2, then slot 0 (:56-64): BOTTLE_YURE in slot 0 always, and in slots 1/2 the
 *     drink set (BOTTLE_D_YURE / BOTTLE_D_NOMU) for `checkDrinkBottleItem ||
 *     checkOilBottleItemNotGet`, else the open set (BOTTLE_NUKU / BOTTLE_DASU). The procs then
 *     switch between them with entryTexMtxAnimator and step them with simpleAnmPlay (bottle.inc
 *     :146-289, :448-498; kandelaar.inc:263-296).
 *
 * The wire carries his ANSWERS (player_bridge.cpp fill_held_item): which slot's animator is
 * entered on his bottle's materials and that animation's own frame, the set, and which contents
 * model he is drawing (heldBck ids 11-13 + bits 5-7, heldBckFrame; dusk/player_held.hpp).
 *
 * Frames: the BTK's comes from the wire every tick and nothing else moves it. The contents BCK is
 * NOT on the wire — it is a cosmetic loop, so it free-runs here from frame 0 the tick the contents
 * appear, one step per sim tick, exactly as his simpleAnmPlay steps it. Each animation object is
 * the puppet's own and has exactly that one driver (memory: J3D anms hold their own frame).
 */

#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"

namespace {

/// The five BTKs setBottleModel can put in his three slots, in mpBottleBtk order.
enum {
    l_bottleBtkYure = 0,  ///< BOTTLE_YURE: slot 0 (field_0x0718), both sets
    l_bottleBtkDYure,     ///< BOTTLE_D_YURE: slot 1 (field_0x071c), drink set
    l_bottleBtkDNomu,     ///< BOTTLE_D_NOMU: slot 2 (field_0x0720), drink set
    l_bottleBtkNuku,      ///< BOTTLE_NUKU: slot 1, open set
    l_bottleBtkDasu,      ///< BOTTLE_DASU: slot 2, open set
    l_bottleBtkNum,
};
const u16 l_bottleBtkRes[l_bottleBtkNum] = {
    dRes_ID_ALANM_BTK_BOTTLE_YURE_e,
    dRes_ID_ALANM_BTK_BOTTLE_D_YURE_e,
    dRes_ID_ALANM_BTK_BOTTLE_D_NOMU_e,
    dRes_ID_ALANM_BTK_BOTTLE_NUKU_e,
    dRes_ID_ALANM_BTK_BOTTLE_DASU_e,
};
const char* const l_bottleBtkName[l_bottleBtkNum] = {
    "bottle YURE", "bottle D_YURE", "bottle D_NOMU", "bottle NUKU", "bottle DASU"};

/// mBottleBtkSet: which set's setBottleModel entries this puppet replayed.
const u8 l_bottleSetNone = 0;
const u8 l_bottleSetOpen = 1;
const u8 l_bottleSetDrink = 2;
/// No bottle since the last one: whatever comes next replays (or clears) the entries.
const u8 l_bottleSetUnknown = 0xFF;

/// setBottleModel's contents (d_a_alink_bottle.inc:66-82): BMD and his buffer literal, the BCK
/// (0xFFFF none) which he reads with 0xC00, indexed by PlayerBottleContents.
struct BottleContentsEntry {
    u16 mBmdIdx;
    u32 mBmdBuf;
    u16 mBckIdx;
    const char* mName;
};
const BottleContentsEntry l_bottleContents[4] = {
    {0xFFFF, 0, 0xFFFF, "none"},
    {dRes_ID_ALANM_BMD_O_GD_NV_e, 0x1400, dRes_ID_ALANM_BCK_O_GD_NV_e, "fairy"},
    {dRes_ID_ALANM_BMD_O_GD_WORM_e, 0x2400, dRes_ID_ALANM_BCK_O_GD_WORM_e, "worm"},
    {dRes_ID_ALANM_BMD_O_GD_HK_e, 0x1000, 0xFFFF, "bee"},
};
const u32 l_bottleContentsBckBuf = 0xC00;
const u16 l_bottleLogMax = 64;

/**
 * Free heap every bottle load must leave behind. setupClawshot, loadStepTurnAnimations and
 * setupSwordFx run after setupHeldItems: measured, they used ~30 KB starting from 77,776 free at
 * create()'s 0x80000 budget (items run, 2026-11-10), and the clawshot's AL_HS_KUSARI fallback
 * gates on ~40 KB (held_heap_fits). So nothing here may take them below 64 KB — these loads are
 * the ones that give way. At that budget the five BTKs fit and the contents models DO NOT: they
 * need create()'s budget raised by about 0x10000 (the solid heap is shrunk to fit afterwards, so
 * the lasting cost is only what is actually used — the "bottle contents" line measures it).
 */
const u32 l_bottleLaterReserve = 0x10000;

bool bottle_heap_keeps_reserve(u32 i_need, const char* i_what) {
    JKRHeap* heap = JKRGetCurrentHeap();
    const u32 freeNow = heap != NULL ? static_cast<u32>(heap->getFreeSize()) : 0;
    if (freeNow < l_bottleLaterReserve + i_need) {
        Log.warn("Puppet bottle part '{}' skipped: {} free, it wants {} plus the {} kept for the "
                 "loads after it",
            i_what, freeNow, i_need, l_bottleLaterReserve);
        return false;
    }
    return true;
}

/// The wire's heldBck (id + drink-set bit) as an mpBottleBtk index, or -1.
int bottle_btk_index(u8 i_bck) {
    const bool drink = (i_bck & dusk::mp::kPlayerHeldBckBottleDrinkSet) != 0;
    switch (i_bck & dusk::mp::kPlayerHeldBckIdMask) {
    case dusk::mp::kPlayerHeldBckBottleYure:
        return l_bottleBtkYure;
    case dusk::mp::kPlayerHeldBckBottleSlot1:
        return drink ? l_bottleBtkDYure : l_bottleBtkNuku;
    case dusk::mp::kPlayerHeldBckBottleSlot2:
        return drink ? l_bottleBtkDNomu : l_bottleBtkDasu;
    default:
        return -1;
    }
}

/// daAlink_c::simpleAnmPlay (d_a_alink.cpp:5845-5863), which is a member and so not callable here.
void bottle_simple_anm_play(J3DAnmBase* i_anm) {
    f32 frame = i_anm->getFrame() + 1.0f;
    if (frame >= i_anm->getFrameMax()) {
        if (i_anm->getAttribute() == 2) {
            frame -= i_anm->getFrameMax();
        } else {
            frame = i_anm->getFrameMax();
        }
    }
    i_anm->setFrame(frame);
}

/// Every (material, texture matrix) i_b animates is one i_a animates too, and as many. True for
/// all five means only ONE slot can be entered at a time, so "which slot" is the whole state.
bool bottle_btk_same_targets(J3DAnmTextureSRTKey* i_a, J3DAnmTextureSRTKey* i_b) {
    int validA = 0;
    for (u16 i = 0; i < i_a->getUpdateMaterialNum(); i++) {
        validA += i_a->isValidUpdateMaterialID(i) ? 1 : 0;
    }
    int validB = 0;
    for (u16 j = 0; j < i_b->getUpdateMaterialNum(); j++) {
        if (!i_b->isValidUpdateMaterialID(j)) {
            continue;
        }
        validB++;
        bool found = false;
        for (u16 i = 0; i < i_a->getUpdateMaterialNum() && !found; i++) {
            found = i_a->isValidUpdateMaterialID(i) &&
                    i_a->getUpdateMaterialID(i) == i_b->getUpdateMaterialID(j) &&
                    i_a->getUpdateTexMtxID(i) == i_b->getUpdateTexMtxID(j);
        }
        if (!found) {
            return false;
        }
    }
    return validA == validB;
}

}  // namespace

/**
 * Sender side. One texture matrix holds one animator, so when his three BTKs share their targets
 * (checked and logged by setupBottleContents on every end) exactly one is found. If they ever do
 * not, the highest slot entered is named — the puppet still replays setBottleModel's entries, so
 * the others stay on its model too, only with frames it is not told.
 */
u8 daRemotePlayer_c::bottleBtkEntered(
    J3DModel* i_bottle, J3DAnmTextureSRTKey* const* i_slots, J3DAnmTextureSRTKey** o_anm) {
    static const u8 l_slotId[3] = {dusk::mp::kPlayerHeldBckBottleYure,
        dusk::mp::kPlayerHeldBckBottleSlot1, dusk::mp::kPlayerHeldBckBottleSlot2};
    *o_anm = NULL;
    if (i_bottle == NULL) {
        return dusk::mp::kPlayerHeldBckNone;
    }
    J3DModelData* data = i_bottle->getModelData();
    bool entered[3] = {false, false, false};
    for (u16 m = 0; m < data->getMaterialNum(); m++) {
        J3DMaterialAnm* matAnm = data->getMaterialNodePointer(m)->getMaterialAnm();
        if (matAnm == NULL) {
            continue;
        }
        for (int t = 0; t < 8; t++) {
            // removeTexMtxAnimator only clears the flag and leaves the pointer: test both.
            const J3DTexMtxAnm& texMtxAnm = matAnm->getTexMtxAnm(t);
            if (!texMtxAnm.getAnmFlag() || texMtxAnm.getAnimation() == NULL) {
                continue;
            }
            for (int s = 0; s < 3; s++) {
                if (i_slots[s] != NULL && texMtxAnm.getAnimation() == i_slots[s]) {
                    entered[s] = true;
                }
            }
        }
    }
    for (int s = 2; s >= 0; s--) {
        if (entered[s]) {
            *o_anm = i_slots[s];
            return l_slotId[s];
        }
    }
    return dusk::mp::kPlayerHeldBckNone;
}

/**
 * Build the contents models and the five BTKs, once, at createHeap time, LAST among the held
 * items (a short heap costs these first). Only with a bottle model: they are its contents and its
 * materials' animations. Every load is non-fatal.
 *
 * The BTKs are his loadAramItemBtk's load (0x400, frame 0, searchUpdateMaterialID — d_a_alink.cpp:
 * 14364-14373) on the puppet's bottle data. Each is entered once here and taken off again: the
 * first entry is where createTexMtxForAnimator makes any J3DTexMtx a material lacks (it then
 * returns 4 WITHOUT entering, J3DMaterialAttach.cpp:137-167), so that allocation happens in this
 * solid heap and never at runtime; taking them off again leaves the bottle drawing exactly as it
 * did before this file existed until the wire names a slot.
 */
void daRemotePlayer_c::setupBottleContents() {
    for (int i = 0; i < 4; i++) {
        mpBottleContentsModel[i] = NULL;
        mpBottleContentsBckAnm[i] = NULL;
    }
    for (int i = 0; i < l_bottleBtkNum; i++) {
        mpBottleBtk[i] = NULL;
    }
    mpBottleBtkEntered = NULL;
    mBottleBtkSet = l_bottleSetUnknown;
    mBottleWantedBtk = 0;
    mBottleWantedContents = dusk::mp::kPlayerBottleContentsNone;
    mBottleContentsPosed = dusk::mp::kPlayerBottleContentsNone;
    mBottleContentsShown = dusk::mp::kPlayerBottleContentsNone;
    mBottleLogCount = 0;

    J3DModel* bottle = mpHeldModel[dusk::mp::kPlayerHeldBottle];
    if (bottle == NULL) {
        Log.warn("Puppet {} bottle contents/BTKs skipped: no bottle model", mPlayerId);
        return;
    }
    JKRHeap* heap = JKRGetCurrentHeap();
    const s32 freeBefore = heap != NULL ? heap->getFreeSize() : 0;

    J3DModelData* data = bottle->getModelData();
    J3DMaterialTable& table = data->getMaterialTable();
    int btkBuilt = 0;
    for (int i = 0; i < l_bottleBtkNum; i++) {
        if (!bottle_heap_keeps_reserve(0x400 * 4, l_bottleBtkName[i])) {
            continue;
        }
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            held_load_aram_anm(l_bottleBtkRes[i], 4, l_bottleBtkName[i]));  // J3DAnmTextureSRTKey
        if (btk == NULL) {
            continue;
        }
        btk->setFrame(0.0f);
        btk->searchUpdateMaterialID(data);
        int rv = table.entryTexMtxAnimator(btk);
        if (rv == 4) {
            rv = table.entryTexMtxAnimator(btk);
        }
        table.removeTexMtxAnimator(btk);
        if (rv != 0) {
            Log.warn("Puppet {} {}: entryTexMtxAnimator returned {}; it may not animate", mPlayerId,
                l_bottleBtkName[i], rv);
        }
        mpBottleBtk[i] = btk;
        btkBuilt++;
    }
    bool sameTargets = mpBottleBtk[l_bottleBtkYure] != NULL;
    for (int i = 1; i < l_bottleBtkNum && sameTargets; i++) {
        if (mpBottleBtk[i] != NULL) {
            sameTargets = bottle_btk_same_targets(mpBottleBtk[l_bottleBtkYure], mpBottleBtk[i]);
        }
    }

    int contentsBuilt = 0;
    int bckBuilt = 0;
    for (int c = 1; c < 4; c++) {
        const BottleContentsEntry& entry = l_bottleContents[c];
        // held_heap_fits' own gate for this BMD, on top of the reserve.
        if (!bottle_heap_keeps_reserve(entry.mBmdBuf * 2 + 0x8000, entry.mName)) {
            continue;
        }
        // His initModel(loadAramBmd(..), 0): init_model's 0x80000 / 0|0x11000084.
        J3DModel* tip = init_model(
            held_load_aram_bmd(entry.mBmdIdx, entry.mBmdBuf, entry.mName), 0, entry.mName);
        if (tip == NULL) {
            continue;
        }
        mpBottleContentsModel[c] = tip;
        contentsBuilt++;
        if (entry.mBckIdx == 0xFFFF) {
            continue;
        }
        // His mHookTipBck: init(bck, FALSE, LOOP, 1, 0, -1, false), entry at frame 0 (:67-72).
        J3DAnmTransform* bck =
            bottle_heap_keeps_reserve(l_bottleContentsBckBuf * 4, entry.mName) ?
                static_cast<J3DAnmTransform*>(held_load_aram_anm(
                    entry.mBckIdx, 8, entry.mName, l_bottleContentsBckBuf)) :  // J3DAnmTransformKey
                NULL;
        if (bck != NULL && held_heap_fits(0, entry.mName) &&
            mBottleContentsBck[c].init(bck, FALSE, J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, false))
        {
            mBottleContentsBck[c].entry(tip->getModelData(), 0.0f);
            mpBottleContentsBckAnm[c] = bck;
            bckBuilt++;
        } else {
            Log.warn("Puppet {} bottle contents '{}': no BCK, it will hold its bind pose",
                mPlayerId, entry.mName);
        }
    }

    // The measured cost: compare it with the create() heap budget.
    Log.info("Puppet {} bottle contents: models {}/3 (fairy {}, worm {}, bee {}), BCKs {}/2, BTKs "
             "{}/5 ({} targets), {} bytes of heap ({} -> {} free)",
        mPlayerId, contentsBuilt, mpBottleContentsModel[1] != NULL ? "ok" : "MISSING",
        mpBottleContentsModel[2] != NULL ? "ok" : "MISSING",
        mpBottleContentsModel[3] != NULL ? "ok" : "MISSING", bckBuilt, btkBuilt,
        sameTargets ? "shared" : "DIFFERENT", heap != NULL ? freeBefore - heap->getFreeSize() : 0,
        freeBefore, heap != NULL ? heap->getFreeSize() : 0);
    if (!sameTargets && btkBuilt > 1) {
        Log.warn("Puppet {} bottle BTKs animate different materials: only the slot on the wire is "
                 "stepped; the others keep the frame they were entered at",
            mPlayerId);
    }
}

/// No bottle this tick: nothing is posed in it, and the next bottle starts as his setBottleModel
/// does — he builds a fresh model with fresh animators every time (d_a_alink_bottle.inc:51-64).
void daRemotePlayer_c::resetBottleShown() {
    mBottleContentsPosed = dusk::mp::kPlayerBottleContentsNone;
    mBottleBtkSet = l_bottleSetUnknown;
    mBottleWantedBtk = 0;
    mBottleWantedContents = dusk::mp::kPlayerBottleContentsNone;
}

/**
 * From setHeldItemMatrix's bottle case, after the bottle's base matrix and its BRK/BTP frames and
 * before its calc: the BTK slot the sender has entered, at his frame, and the contents model on the
 * bottle's matrix. i_inLeftHand: the checkBottleItem branch of setItemMatrix, the only one that
 * poses mpHookTipModel (d_a_alink.cpp:6047-6057) — not the oil pour.
 */
void daRemotePlayer_c::setBottleMatrix(J3DModel* i_bottle, bool i_inLeftHand) {
    const u8 bck = mNetHeldBck;
    const u8 wantBtk = static_cast<u8>(
        bck & (dusk::mp::kPlayerHeldBckIdMask | dusk::mp::kPlayerHeldBckBottleDrinkSet));
    u8 contents = static_cast<u8>((bck & dusk::mp::kPlayerHeldBckBottleContentsMask) >>
                                  dusk::mp::kPlayerHeldBckBottleContentsShift);
    const int btkIdx = bottle_btk_index(bck);
    const u8 set = btkIdx < 0 ?
                       l_bottleSetNone :
                       ((bck & dusk::mp::kPlayerHeldBckBottleDrinkSet) != 0 ? l_bottleSetDrink :
                                                                              l_bottleSetOpen);
    J3DMaterialTable& table = i_bottle->getModelData()->getMaterialTable();

    /* 1. setBottleModel's own entries, replayed when a bottle appears or its set changes: slot 1,
     * slot 2, then slot 0, all at frame 0 (d_a_alink_bottle.inc:56-64). With no slot on the wire
     * (an older sender) every animator comes off, which is how the bottle drew before. */
    if (set != mBottleBtkSet) {
        for (int i = 0; i < l_bottleBtkNum; i++) {
            if (mpBottleBtk[i] != NULL) {
                table.removeTexMtxAnimator(mpBottleBtk[i]);
            }
        }
        mpBottleBtkEntered = NULL;
        if (set != l_bottleSetNone) {
            const bool drink = set == l_bottleSetDrink;
            const int order[3] = {drink ? l_bottleBtkDYure : l_bottleBtkNuku,
                drink ? l_bottleBtkDNomu : l_bottleBtkDasu, l_bottleBtkYure};
            for (int i = 0; i < 3; i++) {
                J3DAnmTextureSRTKey* btk = mpBottleBtk[order[i]];
                if (btk != NULL) {
                    btk->setFrame(0.0f);
                    table.entryTexMtxAnimator(btk);
                    mpBottleBtkEntered = btk;
                }
            }
        }
        mBottleBtkSet = set;
    }

    /* 2. The slot he has entered now, if it is not the one this puppet entered last (pointer
     * guard: an entry per change, as his procs make one), at his frame. */
    J3DAnmTextureSRTKey* btk = btkIdx >= 0 ? mpBottleBtk[btkIdx] : NULL;
    if (btk != NULL) {
        if (btk != mpBottleBtkEntered) {
            table.entryTexMtxAnimator(btk);
            mpBottleBtkEntered = btk;
        }
        const f32 frameMax = btk->getFrameMax();
        f32 frame = frameMax * static_cast<f32>(mNetHeldBckFrame) / 255.0f;
        if (frame > frameMax) {
            frame = frameMax;
        }
        btk->setFrame(frame < 0.0f ? 0.0f : frame);
    }
    if (wantBtk != mBottleWantedBtk && mBottleLogCount < l_bottleLogMax) {
        mBottleLogCount++;
        Log.info("Puppet {} bottle BTK slot {} -> {} ({}, {} set, frame {}/255){}", mPlayerId,
            static_cast<unsigned>(mBottleWantedBtk & dusk::mp::kPlayerHeldBckIdMask),
            static_cast<unsigned>(wantBtk & dusk::mp::kPlayerHeldBckIdMask),
            btkIdx >= 0 ? l_bottleBtkName[btkIdx] : "none",
            set == l_bottleSetDrink ? "drink" : (set == l_bottleSetOpen ? "open" : "no"),
            static_cast<unsigned>(mNetHeldBckFrame),
            btkIdx >= 0 && btk == NULL ? " NOT LOADED" : "");
    }
    mBottleWantedBtk = wantBtk;

    // 3. The contents: setItemMatrix plays his BCK, then poses it on the bottle's matrix
    // (:6053-6057).
    if (contents >= 4) {
        contents = dusk::mp::kPlayerBottleContentsNone;
    }
    const bool fresh = contents != mBottleWantedContents;
    if (fresh && mBottleLogCount < l_bottleLogMax) {
        mBottleLogCount++;
        Log.info("Puppet {} bottle contents {} -> {} ({}){}", mPlayerId,
            static_cast<unsigned>(mBottleWantedContents), static_cast<unsigned>(contents),
            l_bottleContents[contents].mName,
            contents != dusk::mp::kPlayerBottleContentsNone &&
                    mpBottleContentsModel[contents] == NULL ?
                " NO MODEL, not drawn" :
                (contents != dusk::mp::kPlayerBottleContentsNone && !i_inLeftHand ?
                        " not posed (pouring)" :
                        ""));
    }
    mBottleWantedContents = contents;

    mBottleContentsPosed = dusk::mp::kPlayerBottleContentsNone;
    J3DModel* tip = mpBottleContentsModel[contents];
    if (contents == dusk::mp::kPlayerBottleContentsNone || tip == NULL || !i_inLeftHand) {
        return;
    }
    J3DAnmTransform* tipBck = mpBottleContentsBckAnm[contents];
    if (tipBck != NULL) {
        // setBottleModel enters it at frame 0 (:72, :79); his next setItemMatrix steps it first.
        if (fresh) {
            tipBck->setFrame(0.0f);
        }
        bottle_simple_anm_play(tipBck);
    }
    tip->setBaseTRMtx(i_bottle->getBaseTRMtx());
    tip->calc();
    mBottleContentsPosed = contents;
}

/// From drawHeldItems, right after the bottle — his order (d_a_alink.cpp:19773-19777). ★ Hang 4:
/// one entry per pass; draw() carries the per-sim-tick guard.
void daRemotePlayer_c::drawBottleContents() {
    const u8 contents = mBottleContentsPosed;
    mBottleContentsShown = dusk::mp::kPlayerBottleContentsNone;
    if (contents != dusk::mp::kPlayerBottleContentsNone && mpBottleContentsModel[contents] != NULL)
    {
        drawModel(mpBottleContentsModel[contents]);
        mBottleContentsShown = contents;
    }
}

void daRemotePlayer_c::getBottleDrawnWire(u8& io_bck, u8& io_frame) const {
    if (mHeldDrawnKind != dusk::mp::kPlayerHeldBottle) {
        return;
    }
    u8 bck = static_cast<u8>(io_bck & dusk::mp::kPlayerHeldBckBombArrow);
    u8 frame = 0;
    int idx = -1;
    for (int i = 0; i < l_bottleBtkNum; i++) {
        if (mpBottleBtk[i] != NULL && mpBottleBtk[i] == mpBottleBtkEntered) {
            idx = i;
        }
    }
    if (idx >= 0) {
        static const u8 l_idxToWire[l_bottleBtkNum] = {
            dusk::mp::kPlayerHeldBckBottleYure,
            dusk::mp::kPlayerHeldBckBottleSlot1 | dusk::mp::kPlayerHeldBckBottleDrinkSet,
            dusk::mp::kPlayerHeldBckBottleSlot2 | dusk::mp::kPlayerHeldBckBottleDrinkSet,
            dusk::mp::kPlayerHeldBckBottleSlot1,
            dusk::mp::kPlayerHeldBckBottleSlot2,
        };
        bck |= l_idxToWire[idx];
        if (mBottleBtkSet == l_bottleSetDrink) {
            bck |= dusk::mp::kPlayerHeldBckBottleDrinkSet;
        }
        const J3DAnmTextureSRTKey* btk = mpBottleBtk[idx];
        if (btk->getFrameMax() > 0) {
            const f32 f = btk->getFrame() / btk->getFrameMax();
            frame = static_cast<u8>(f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : f * 255.0f + 0.5f));
        }
    }
    bck |= static_cast<u8>(mBottleContentsShown << dusk::mp::kPlayerHeldBckBottleContentsShift);
    io_bck = bck;
    io_frame = frame;
}
