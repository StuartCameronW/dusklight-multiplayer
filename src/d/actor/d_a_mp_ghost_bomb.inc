/* Bomb, water bomb, bombling (0x03-0x05): the ghost side. Included at the end of d_a_mp_ghost.cpp.
 * Stub until its phase-1 agent fills it in: no heap, so kind_info().implemented stays false and
 * the channel never asks for this ghost. */

int daMpGhost_c::createHeapBomb() {
    return FALSE;
}

void daMpGhost_c::executeBomb() {}

int daMpGhost_c::drawBomb() {
    return 1;
}

void daMpGhost_c::eventBomb(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    (void)i_event;
    (void)i_blob;
    (void)i_blobLen;
}
