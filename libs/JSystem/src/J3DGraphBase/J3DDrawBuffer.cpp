/**
 * J3DDrawBuffer.cpp
 *
 */

#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "dusk/logging.h"
#include "tracy/Tracy.hpp"

#if TARGET_PC
namespace {

aurora::Module J3DDrawBufferLog{"J3D"};

/* ★ Entering the same packet into a draw buffer twice within one pass is never legitimate, and
 * before this it was not survivable either.
 *
 * Every entry path below PREPENDS, and the dedup walk in entryMatSort/entryMatAnmSort merges on
 * isSame(), which compares only mMaterialID — so a packet always matches ITSELF. A second entry
 * therefore ran addShapePacket() with the packet's own shape packet, linking the chain head to
 * itself. J3DMatPacket::draw then walked that chain forever, and the GX command stream reached
 * 2.15 GB in a single frame before anything noticed.
 *
 * So: refuse the duplicate and say who caused it. Refusing is the conservative choice — the packet
 * is already in this buffer and will be drawn exactly once, which is what the second entry was
 * asking for anyway.
 *
 * The report names the model rather than only the packet, because the packet address alone does
 * not say which actor is at fault; the shape packet carries its J3DModel back-pointer. */
const int l_duplicateEntryReportMax = 4;
int l_duplicateEntryReports = 0;

void reportDuplicateEntry(const char* i_where, J3DPacket* i_packet, J3DModel* i_model, u32 i_slot) {
    if (l_duplicateEntryReports >= l_duplicateEntryReportMax) {
        return;
    }
    l_duplicateEntryReports++;

    J3DDrawBufferLog.warn(
        "{}: packet {:#x} (model {:#x}) is already the head of slot {}; refusing to enter it twice "
        "in one pass, which would link its shape-packet chain to itself.",
        i_where, reinterpret_cast<uintptr_t>(i_packet), reinterpret_cast<uintptr_t>(i_model),
        i_slot);

    if (l_duplicateEntryReports == l_duplicateEntryReportMax) {
        J3DDrawBufferLog.warn("further duplicate draw-buffer entry reports suppressed");
    }
}

J3DModel* modelOf(J3DMatPacket* i_packet) {
    J3DShapePacket* shapePacket = i_packet->getShapePacket();
    return shapePacket != NULL ? shapePacket->getModel() : NULL;
}

}  // namespace
#endif

void J3DDrawBuffer::calcZRatio() {
    mZRatio = (mZFar - mZNear) / (f32)mEntryTableSize;
}

void J3DDrawBuffer::initialize() {
    mDrawMode = J3DDrawBufDrawMode_Head;
    mSortMode = J3DDrawBufSortMode_Mat;
    mZNear = 1.0f;
    mZFar = 10000.0f;
    mpZMtx = NULL;
    mpCallBackPacket = NULL;
    mEntryTableSize = 0x20;
    calcZRatio();
}

int J3DDrawBuffer::allocBuffer(u32 size) {
    mpBuffer = JKR_NEW_ARRAY_ARGS(J3DPacket*, size, 0x20);
    if (mpBuffer == NULL)
        return kJ3DError_Alloc;

    mEntryTableSize = size;

    frameInit();
    calcZRatio();
    return kJ3DError_Success;
}

J3DDrawBuffer::~J3DDrawBuffer() {
    frameInit();

    JKR_DELETE_ARRAY(mpBuffer);
    mpBuffer = NULL;
}

void J3DDrawBuffer::frameInit() {
    u32 bufSize = mEntryTableSize;
    for (u32 i = 0; i < bufSize; i++)
        mpBuffer[i] = NULL;

    mpCallBackPacket = NULL;
}

int J3DDrawBuffer::entryMatSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(122, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (pMatPacket->isChanged()) {
#if TARGET_PC
        if (pMatPacket == mpBuffer[0]) {
            reportDuplicateEntry("entryMatSort(changed)", pMatPacket, modelOf(pMatPacket), 0);
            return 0;
        }
#endif
        pMatPacket->setNextPacket(mpBuffer[0]);
        mpBuffer[0] = pMatPacket;
        return 1;
    }

    J3DTexture* pTexture = j3dSys.getTexture();
    u16 texNo = pMatPacket->getMaterial()->getTexNo(0);
    J3D_ASSERT_NULLPTR(150, pTexture != NULL);

    u32 hash;
    if (texNo == 0xFFFF) {
        hash = 0;
    } else {
        hash = ((uintptr_t)pTexture->getResTIMG(texNo) + pTexture->getResTIMG(texNo)->imageOffset) >> 5;
    }
    u32 slot = hash & (mEntryTableSize - 1);

    if (mpBuffer[slot] == NULL) {
        mpBuffer[slot] = pMatPacket;
        return 1;
    }

    J3DMatPacket* packet;
    for (packet = (J3DMatPacket*)mpBuffer[slot]; packet != NULL; packet = (J3DMatPacket*)packet->getNextPacket())
    {
#if TARGET_PC
        if (packet == pMatPacket) {
            reportDuplicateEntry("entryMatSort", pMatPacket, modelOf(pMatPacket), slot);
            return 0;
        }
#endif
        if (packet->isSame(pMatPacket)) {
            packet->addShapePacket(pMatPacket->getShapePacket());
            return 0;
        }
    }

    pMatPacket->setNextPacket(mpBuffer[slot]);
    mpBuffer[slot] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryMatAnmSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(199, pMatPacket != NULL);

    J3DMaterialAnm* pMaterialAnm = pMatPacket->mpMaterialAnm;
    u32 slot = (uintptr_t)pMaterialAnm & (mEntryTableSize - 1);

    if (pMaterialAnm == NULL) {
        return entryMatSort(pMatPacket);
    }

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpBuffer[slot] == NULL) {
        mpBuffer[slot] = pMatPacket;
        return 1;
    }

    J3DMatPacket* packet;
    for (packet = (J3DMatPacket*)mpBuffer[slot]; packet != NULL; packet = (J3DMatPacket*)packet->getNextPacket())
    {
#if TARGET_PC
        if (packet == pMatPacket) {
            reportDuplicateEntry("entryMatAnmSort", pMatPacket, modelOf(pMatPacket), slot);
            return 0;
        }
#endif
        if (packet->mpMaterialAnm == pMaterialAnm) {
            packet->addShapePacket(pMatPacket->getShapePacket());
            return 0;
        }
    }

    pMatPacket->setNextPacket(mpBuffer[slot]);
    mpBuffer[slot] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryZSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(257, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    Vec tmp;
    tmp.x = mpZMtx[0][3];
    tmp.y = mpZMtx[1][3];
    tmp.z = mpZMtx[2][3];

    f32 value = -J3DCalcZValue(j3dSys.getViewMtx(), tmp);

    u32 index;
    if (mZNear + mZRatio < value) {
        if (mZFar - mZRatio > value) {
            index = value / mZRatio;
        } else {
            index = mEntryTableSize - 1;
        }
    } else {
        index = 0;
    }

    index = (mEntryTableSize - 1) - index;
#if TARGET_PC
    if (pMatPacket == mpBuffer[index]) {
        reportDuplicateEntry("entryZSort", pMatPacket, modelOf(pMatPacket), index);
        return 0;
    }
#endif
    pMatPacket->setNextPacket(mpBuffer[index]);
    mpBuffer[index] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryModelSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(316, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpCallBackPacket != NULL) {
        mpCallBackPacket->addChildPacket(pMatPacket);
        return 1;
    }

    return 0;
}

int J3DDrawBuffer::entryInvalidSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(343, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpCallBackPacket != NULL) {
        mpCallBackPacket->addChildPacket(pMatPacket->getShapePacket());
        return 1;
    }

    return 0;
}

int J3DDrawBuffer::entryNonSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(370, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

#if TARGET_PC
    if (pMatPacket == mpBuffer[0]) {
        reportDuplicateEntry("entryNonSort", pMatPacket, modelOf(pMatPacket), 0);
        return 0;
    }
#endif
    pMatPacket->setNextPacket(mpBuffer[0]);
    mpBuffer[0] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryImm(J3DPacket* pPacket, u16 index) {
    J3D_ASSERT_NULLPTR(394, pPacket != NULL);
    J3D_ASSERT_RANGE(395, index < mEntryTableSize);

#if TARGET_PC
    /* daMirror_c::draw() enters a single static packet here on every draw pass, so this one is
     * reachable without any duplicate model entry at all. */
    if (pPacket == mpBuffer[index]) {
        reportDuplicateEntry("entryImm", pPacket, NULL, index);
        return 0;
    }
#endif
    pPacket->setNextPacket(mpBuffer[index]);
    mpBuffer[index] = pPacket;
    return 1;
}

J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6] = {
    &J3DDrawBuffer::entryMatSort,   &J3DDrawBuffer::entryMatAnmSort,  &J3DDrawBuffer::entryZSort,
    &J3DDrawBuffer::entryModelSort, &J3DDrawBuffer::entryInvalidSort, &J3DDrawBuffer::entryNonSort,
};

J3DDrawBuffer::drawFunc J3DDrawBuffer::drawFuncTable[2] = {
    &J3DDrawBuffer::drawHead,
    &J3DDrawBuffer::drawTail,
};

int J3DDrawBuffer::entryNum;

void J3DDrawBuffer::draw() const {
    J3D_ASSERT_RANGE(411, mDrawMode < J3DDrawBufDrawMode_MAX);

    drawFunc func = drawFuncTable[mDrawMode];
    (this->*func)();
}

void J3DDrawBuffer::drawHead() const {
    ZoneScoped;
    u32 size = mEntryTableSize;
    J3DPacket** buf = mpBuffer;

    for (u32 i = 0; i < size; i++) {
        for (J3DPacket* packet = buf[i]; packet != NULL; packet = packet->getNextPacket()) {
            packet->draw();
        }
    }
}

void J3DDrawBuffer::drawTail() const {
    ZoneScoped;
    for (int i = mEntryTableSize - 1; i >= 0; i--) {
        for (J3DPacket* packet = mpBuffer[i]; packet != NULL; packet = packet->getNextPacket()) {
            packet->draw();
        }
    }
}
