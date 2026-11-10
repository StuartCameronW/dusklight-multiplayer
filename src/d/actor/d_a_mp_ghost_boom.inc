/* Gale Boomerang (0x07): the ghost side. Included at the end of d_a_mp_ghost.cpp.
 * Stub until its phase-1 agent fills it in: no heap, so kind_info().implemented stays false and
 * the channel never asks for this ghost. */

int daMpGhost_c::createHeapBoom() {
    return FALSE;
}

void daMpGhost_c::executeBoom() {}

int daMpGhost_c::drawBoom() {
    return 1;
}

void daMpGhost_c::eventBoom(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    (void)i_event;
    (void)i_blob;
    (void)i_blobLen;
}
