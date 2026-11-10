#pragma once

#include <cstdint>

/**
 * The replicated held-item block's layout (PlayerState block 3): which item model the sender's Link
 * is DRAWING in his hands, and how.
 *
 * Public and alone for the same reason as player_equip.hpp: the protocol layer packs these values
 * and the puppet actor (d/actor/d_a_remote_player.cpp) unpacks them, and neither may include the
 * other. Enumerations only.
 *
 * Every value is the sender's ANSWER, read after daAlink_c's own logic has decided it — the item
 * model that setItemMatrix placed and checkItemDraw() let through (d_a_alink.cpp:6039-6091,
 * :19086-19096, :19771-19773), never mEquipItem alone. A bow he is carrying behind a raised shield
 * is not drawn, so it is not sent.
 */

namespace dusk::mp {

/**
 * Which item model is in his hands. One value, because daAlink_c holds ONE mHeldItemModel at a time
 * (setItemModel, d_a_alink.cpp:14161-14212). The lantern is the exception — it is a separate model
 * that can be out alongside another item — so it rides in PlayerHeldFlags instead.
 *
 * Not reproduced, and sent as kPlayerHeldNone: the fishing rod and bombs (separate actors), the
 * grass whistle (a stage-archive model), the wolf-change model (0x106) and the spinner (ridden, not
 * held). The iron ball IS reproduced (kPlayerHeldIronBall), with its world-space placement in a
 * block of its own (PlayerState's v19 block B).
 */
enum PlayerHeldItem : std::uint8_t {
    kPlayerHeldNone = 0,
    /// Any bottle: checkBottleItem(mEquipItem) (setBottleModel, d_a_alink_bottle.inc:50-142).
    kPlayerHeldBottle = 1,
    /// checkBowItem: al_bow (d_a_alink_bow.inc:511-525).
    kPlayerHeldBow = 2,
    /// dItemNo_PACHINKO_e: al_pachi (d_a_alink_bow.inc:55-68).
    kPlayerHeldSlingshot = 3,
    /// dItemNo_HOOKSHOT_e: one clawshot, always DRAWN in the left hand (see the .cpp).
    kPlayerHeldClawshot = 4,
    /// dItemNo_W_HOOKSHOT_e: two clawshots; kPlayerHeldClawRight / kPlayerHeldClawBoth say how.
    kPlayerHeldDoubleClawshot = 5,
    /// dItemNo_COPY_ROD_e: the Dominion Rod, al_crod (d_a_alink_copyrod.inc:108-128).
    kPlayerHeldCopyRod = 6,
    /// dItemNo_HORSE_FLUTE_e: the horse-call pendant, al_pend (d_a_alink_whistle.inc:53-61).
    kPlayerHeldHorseCall = 7,
    /// The boomerang while it sits in his hand — daBoomerang_c's procWait/setKeepMatrix
    /// (d_a_boomerang.cpp:686-699, :932-935). In flight it is a world actor and is not sent.
    kPlayerHeldBoomerang = 8,
    /// dItemNo_IRONBALL_e: al_ib, the BALL, which is not in his hand — setIronBallPos places it in
    /// world space (d_a_alink_ironball.inc:465-770) and hsChainShape_c draws the chain between it
    /// and his right hand (d_a_alink_hook.inc:58-147). Where it is rides in PlayerState's iron-ball
    /// block, not here.
    kPlayerHeldIronBall = 9,
    kPlayerHeldKindNum = 10,
};

/// Bit flags in PlayerState::heldFlags.
enum PlayerHeldFlags : std::uint8_t {
    /// The lantern is out (FLG2_UNK_1, set by setKandelaarModel, d_a_alink_kandelaar.inc:211-216):
    /// daAlink_c draws mpKanteraModel (d_a_alink.cpp:19822-19825).
    kPlayerHeldLanternShown = 1 << 0,
    /// ...and it is in his left hand rather than on his hip: `mEquipItem == KANTERA ||
    /// checkOilBottleItemNotGet(mEquipItem)`, setItemMatrix's own test (d_a_alink.cpp:6098).
    kPlayerHeldLanternInHand = 1 << 1,
    /// Bow/slingshot in the LEFT hand with his offset: checkBowGrabLeftHand() (riding, not aiming;
    /// d_a_alink_bow.inc:527-529). Otherwise the right item joint.
    kPlayerHeldBowLeftHand = 1 << 2,
    /// Bottle in the RIGHT hand, pouring oil into the lantern: checkOilBottleItemNotGet
    /// (d_a_alink.cpp:6042-6046). Otherwise the left item joint.
    kPlayerHeldBottlePour = 1 << 3,
    /// The bottle's joint 1 (its cork) is in the right hand: bottleModelCallBack's `mItemMode != 0`
    /// (d_a_alink_bottle.inc:13-21), only while the callback is installed (not pouring).
    kPlayerHeldBottleCork = 1 << 4,
    /// Either clawshot: the PRIMARY one (mHeldItemModel, the one that fires) is in the right hand —
    /// setHookshotPos's `field_0x3020 != 0` (d_a_alink_hook.inc:848-870). For the single clawshot
    /// this means changeHookshotDrawModel draws his SECOND pair (field_0x0710 / 0714, the one in
    /// the left hand) in its place (d_a_alink_hook.inc:346-356) — a roof or wall hang flips it
    /// (:1736-1741, :1996-2001). Sent for the single clawshot since the v20 clawshot block.
    kPlayerHeldClawRight = 1 << 5,
    /// Double clawshot: the second one is drawn too — not guarding (d_a_alink.cpp:19779-19789).
    kPlayerHeldClawBoth = 1 << 6,
    /// An arrow is nocked: the actor in mItemAcKeep is a daArrow_c still in procWait (checkWait(),
    /// not deleteArrow()ed), so it draws itself on his left item joint every frame
    /// (daArrow_c::setKeepMatrix / draw, d_a_arrow.cpp:402-423, :1073-1129). Independent of
    /// heldItem: the arrow is its own actor. Its kind is kPlayerHeldBckBombArrow in heldBck.
    kPlayerHeldArrowNocked = 1 << 7,
};

/**
 * PlayerState::heldBck (protocol v19 block A), bits 0-3: WHICH BCK is animating the bow or
 * slingshot model itself — daAlink_c's mItemBck, the string pull and limb bend. It is whatever
 * changeItemBck() last loaded into mAnmHeap9 (d_a_alink.cpp:14386-14397), entered on the item model
 * with field_0x33dc every tick by setItemMatrix (:6083-6089). These are every resource
 * setBowModel / setSlingModel / changeItemBck can put there (d_a_alink_bow.inc); the puppet maps
 * the id back to the resource. Ids are the wire's, not resource numbers, so they fit in 4 bits.
 *
 * Since the v20 clawshot block also the clawshot's gun spin, HS_SPIN, which setHookshotModel puts
 * in mAnmHeap9 / mItemBck (d_a_alink_hook.inc:428-433) and setHookshotPos steps forward while the
 * tip flies out and back while it returns (:872-890). Its table row names kPlayerHeldClawshot for
 * BOTH clawshot kinds: the id says which MODEL it poses, and the two kinds share one model.
 */
enum PlayerHeldItemBck : std::uint8_t {
    kPlayerHeldBckNone = 0,
    kPlayerHeldBckBowJumpClose = 1,     ///< BVJMPCL: setBowModel's and setBowNormalAnime's
    kPlayerHeldBckBowJumpHang = 2,      ///< BVJMPCH: setBowHangAnime
    kPlayerHeldBckBowShoot = 3,         ///< BASHOOT: setBowReadyAnime, and an uncharged release
    kPlayerHeldBckBowShootCharged = 4,  ///< BASHOOTTAME: a charged release (field_0x301c == 0)
    kPlayerHeldBckBowReload = 5,        ///< BARELORD: setBowReloadAnime, drawing the string
    kPlayerHeldBckBowReloadCharge = 6,  ///< BARELORDTAME: held at full draw
    kPlayerHeldBckSlingWait = 7,        ///< PWAIT: setSlingModel's, and setBowNormalAnime's
    kPlayerHeldBckSlingShoot = 8,       ///< PSHOOT
    kPlayerHeldBckSlingReload = 9,      ///< PRELORD
    kPlayerHeldBckClawSpin = 10,        ///< HS_SPIN: the clawshot gun (v20 clawshot block)
    // --- bottle contents (agent H) ---
    /* BOTTLE only: which of daAlink_c's three bottle BTK slots is ENTERED on the bottle's
     * materials (setBottleModel loads all three, d_a_alink_bottle.inc:56-64; the procs switch with
     * entryTexMtxAnimator). Not BCKs: the puppet does not look these up in its BCK table. Which
     * RESOURCE slots 1 and 2 hold is kPlayerHeldBckBottleDrinkSet. Id 14 is still free. */
    kPlayerHeldBckBottleYure = 11,      ///< field_0x0718: BOTTLE_YURE, the slosh (both sets)
    kPlayerHeldBckBottleSlot1 = 12,     ///< field_0x071c: BOTTLE_D_YURE / BOTTLE_NUKU (uncork)
    kPlayerHeldBckBottleSlot2 = 13,     ///< field_0x0720: BOTTLE_D_NOMU (drink) / BOTTLE_DASU (pour)
    kPlayerHeldBckNum = 14,
};
constexpr std::uint8_t kPlayerHeldBckIdMask = 0x0F;
/// heldBck bit 4: the nocked arrow (kPlayerHeldArrowNocked) is a bomb arrow — daArrow_c type 1,
/// AL_ARROWB, rather than type 0, AL_ARROW (daArrow_c::createHeap, d_a_arrow.cpp:24-46).
constexpr std::uint8_t kPlayerHeldBckBombArrow = 0x10;

// --- bottle contents (agent H) ---
/* heldBck bits 5-7, meaningful only while heldItem is kPlayerHeldBottle (the bow's bit 4 is never
 * set then: it needs a nocked arrow). No new bytes and no protocol bump: an older receiver ignores
 * the bottle's ids and these bits, and an older sender sends 0 (no BTK, no contents), which is
 * what the puppet drew before. */
/// Bits 5-6: the contents model daAlink_c is DRAWING in the bottle — his mpHookTipModel, non-NULL
/// and drawn under checkItemDraw() (d_a_alink.cpp:19775-19777), named by the mEquipItem
/// setBottleModel built it for (d_a_alink_bottle.inc:66-82).
constexpr std::uint8_t kPlayerHeldBckBottleContentsShift = 5;
constexpr std::uint8_t kPlayerHeldBckBottleContentsMask = 0x60;
enum PlayerBottleContents : std::uint8_t {
    kPlayerBottleContentsNone = 0,
    kPlayerBottleContentsFairy = 1,  ///< dItemNo_FAIRY_e: O_GD_NV + its looping BCK
    kPlayerBottleContentsWorm = 2,   ///< dItemNo_WORM_e: O_GD_WORM + its looping BCK
    kPlayerBottleContentsBee = 3,    ///< dItemNo_BEE_CHILD_e: O_GD_HK, no BCK
};
/// Bit 7: setBottleModel loaded the DRINK set into slots 1/2 (BOTTLE_D_YURE / BOTTLE_D_NOMU), its
/// `checkDrinkBottleItem || checkOilBottleItemNotGet` (d_a_alink_bottle.inc:56-62); clear = the
/// open set (BOTTLE_NUKU / BOTTLE_DASU).
constexpr std::uint8_t kPlayerHeldBckBottleDrinkSet = 0x80;

/* PlayerState::heldBckFrame (v19 block A): the frame that BCK was entered with — mItemBck's
 * animation frame after setItemMatrix clamped field_0x33dc below getFrameMax() — as a fraction of
 * getFrameMax(), x255. 255 is "the end", which the receiver clamps to frameMax - 0.001 as he does.
 * Meaningless while the id is kPlayerHeldBckNone. For a single clawshot drawn swapped
 * (kPlayerHeldClawRight) it is 0: the gun he then draws is field_0x0710, which setHookshotPos
 * enters at frame 0 (d_a_alink_hook.inc:911). For a bottle (agent H) it is the entered BTK's own
 * frame / getFrameMax(), x255. */

/* PlayerState::heldMat, per kind. The material animation frames Link's held model is showing.
 *
 * Bottle: bits 0-3 the BRK frame (contents colour, 0..11: setBottleModel's table), bits 4-5 the BTP
 * frame (0 corked / 1 half milk / 2 open), bit 6 set = material 0's shape is HIDDEN.
 * Copy rod: the BRK frame as a fraction of its length, x255 (checkCopyRodTopUse's glow).
 * Everything else: 0. */
constexpr std::uint8_t kPlayerHeldMatBottleBrkMask = 0x0F;
constexpr std::uint8_t kPlayerHeldMatBottleBtpShift = 4;
constexpr std::uint8_t kPlayerHeldMatBottleBtpMask = 0x30;
constexpr std::uint8_t kPlayerHeldMatBottleMat0Hidden = 0x40;

/* PlayerState::clawFlags (v20 clawshot block), while heldItem is a clawshot. Two SLOTS, named for
 * daAlink_c's two sets, not for hands (kPlayerHeldClawRight says which hand slot 0 is in):
 *   slot 0: mHeldItemModel + mpHookTipModel; its chain runs mHeldItemRootPos -> mHookshotTopPos
 *   slot 1: field_0x0710 + field_0x0714; its chain runs field_0x3810 -> mIronBallBgChkPos
 * (setHookshotPos, d_a_alink_hook.inc:848-1245; hsChainShape_c::draw, :147-245). */
/// Slot 0's tip is off its gun: `mHeldItemRootPos.abs(mHookshotTopPos) > 1`, the very test his draw
/// makes before it draws the chain (d_a_alink.cpp:19793-19796). Its position is on the wire then.
constexpr std::uint8_t kPlayerClawTipOut0 = 1 << 0;
/// Slot 1's tip is off its gun: `field_0x3810.abs(mIronBallBgChkPos) > 1` — hanging from it, or
/// it is still flying back (field_0x3024, :1213-1238). Its position is on the wire then.
constexpr std::uint8_t kPlayerClawTipOut1 = 1 << 1;
/// checkHookshotWait() (mode 0/1, :392-394): his slot-0 tip casts into the body's shadow only then
/// (d_a_alink.cpp:19324-19326).
constexpr std::uint8_t kPlayerClawWait = 1 << 2;
/// dComIfGp_checkPlayerStatus1(0, 0x10000), the roof hang: no tip shadows at all then
/// (d_a_alink.cpp:19324-19335).
constexpr std::uint8_t kPlayerClawRoofHang = 1 << 3;
/// Bits 4-7: field_0x3026 (getHookshotStopTime), clamped to 15. setHookshotCatchNow sets 5 and it
/// counts down; the slot-0 chain's wobble is 2.5 x this, its sign flipping on odd values
/// (d_a_alink_hook.inc:170-174, :396-398).
constexpr std::uint8_t kPlayerClawStopShift = 4;
constexpr std::uint8_t kPlayerClawStopMask = 0xF0;

/* PlayerState::ironBallState (v19 block B), while kPlayerHeldIronBall is out. */
/// mItemVar0.field_0x3018 in the low nibble: the throw state (0 carried .. 8 caught again), which
/// setIronBallPos and setIronBallChainPos branch on (d_a_alink_ironball.inc:198-449, :465-810)...
constexpr std::uint8_t kPlayerIronBallStateMask = 0x0F;
/// ...and bit 7 set while he is in PROC_IRON_BALL_SUBJECT / PROC_IRON_BALL_MOVE, the two procs in
/// which the carried ball takes its rotation from his left item joint instead of his facing
/// (setIronBallPos, d_a_alink_ironball.inc:511-516).
constexpr std::uint8_t kPlayerIronBallJointRot = 0x80;

}  // namespace dusk::mp
