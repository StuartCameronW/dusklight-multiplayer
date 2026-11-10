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
 * Not reproduced, and sent as kPlayerHeldNone: the iron ball (its model is the BALL, swung in world
 * space by setIronBallPos, d_a_alink_ironball.inc:465-770), the fishing rod and bombs (separate
 * actors), the grass whistle (a stage-archive model), the wolf-change model (0x106) and the spinner
 * (ridden, not held).
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
    kPlayerHeldKindNum = 9,
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
    /// Double clawshot: the PRIMARY one (mHeldItemModel, the one that fires) is in the right hand —
    /// setHookshotPos's `field_0x3020 != 0` (d_a_alink_hook.inc:848-870).
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
    kPlayerHeldBckNum = 10,
};
constexpr std::uint8_t kPlayerHeldBckIdMask = 0x0F;
/// heldBck bit 4: the nocked arrow (kPlayerHeldArrowNocked) is a bomb arrow — daArrow_c type 1,
/// AL_ARROWB, rather than type 0, AL_ARROW (daArrow_c::createHeap, d_a_arrow.cpp:24-46).
constexpr std::uint8_t kPlayerHeldBckBombArrow = 0x10;

/* PlayerState::heldBckFrame (v19 block A): the frame that BCK was entered with — mItemBck's
 * animation frame after setItemMatrix clamped field_0x33dc below getFrameMax() — as a fraction of
 * getFrameMax(), x255. 255 is "the end", which the receiver clamps to frameMax - 0.001 as he does.
 * Meaningless while the id is kPlayerHeldBckNone. */

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

}  // namespace dusk::mp
