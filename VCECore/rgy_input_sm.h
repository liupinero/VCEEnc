﻿// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#pragma once
#ifndef __RGY_INPUT_SM_H__
#define __RGY_INPUT_SM_H__

#include "rgy_input.h"
#include "rgy_shared_mem.h"

static const char *RGYInputSMPrmSM       = "RGYInputSMPrmSM";
static const char *RGYInputSMBuffer      = "RGYInputSMBuffer";

#pragma pack(push)
#pragma pack(1)
struct RGYInputSMSharedData {
    int w, h;
    int fpsN, fpsD;
    int pitch;
    RGY_CSP csp;
    RGY_PICSTRUCT picstruct;
    int frames;
    uint32_t bufSize;
    int64_t timestamp;
    int duration;
    bool afs;
    bool abort;
    bool reserved[2];
    uint32_t heBufEmpty;
    uint32_t heBufFilled;
};
#pragma pack(pop)

#if ENABLE_SM_READER

class RGYInputSMPrm : public RGYInputPrm {
public:
    uint32_t parentProcessID;

    RGYInputSMPrm(RGYInputPrm base);

    virtual ~RGYInputSMPrm() {};
};

class RGYInputSM : public RGYInput {
public:
    RGYInputSM();
    virtual ~RGYInputSM();

    virtual RGY_ERR LoadNextFrame(RGYFrame *pSurface) override;
    virtual void Close() override;
    virtual rgy_rational<int> getInputTimebase() override;

    bool isAfs();
protected:
    virtual RGY_ERR Init(const TCHAR *strFileName, VideoInfo *pInputInfo, const RGYInputPrm *prm) override;

    std::unique_ptr<RGYSharedMemWin> m_prm;
    std::unique_ptr<RGYSharedMem> m_sm;
    HANDLE m_heBufEmpty;
    HANDLE m_heBufFilled;
    HANDLE m_parentProcess;
};

#endif //#if ENABLE_SM_READER

#endif //__RGY_INPUT_RAW_H__
