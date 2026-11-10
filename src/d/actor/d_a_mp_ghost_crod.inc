/* Dominion Rod ball (0x08): the ghost side. Included at the end of d_a_mp_ghost.cpp.
 * Stub until its phase-1 agent fills it in: no heap, so kind_info().implemented stays false and
 * the channel never asks for this ghost. */

int daMpGhost_c::createHeapCrod() {
    return FALSE;
}

void daMpGhost_c::executeCrod() {}

int daMpGhost_c::drawCrod() {
    return 1;
}

void daMpGhost_c::eventCrod(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    (void)i_event;
    (void)i_blob;
    (void)i_blobLen;
}
