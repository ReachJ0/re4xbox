// Silent stand-ins for the audio and movie middleware, so the game links and boots before the real audio layer
// (an AX voice mixer over the Xbox's DirectSound/APU) and the Sofdec decoder exist. Nothing here makes sound or
// decodes video. Voices are never granted (AXAcquireVoice returns NULL), movies are never created
// (mwPlyCreateSofdec returns NULL). Not tried on hardware; whether the game tolerates a NULL voice or movie
// everywhere is unverified.
#include <dolphin/types.h>
#include <dolphin/ai.h>
#include <dolphin/ax.h>
#include <dolphin/axart.h>
#include <dolphin/axfx.h>
#include <dolphin/mix.h>
#include <dolphin/seq.h>
#include <dolphin/syn.h>
#include "mwply.h"

// CRI ADX (declared by the game sources, not by headers)
extern "C" {
void ADXGC_SetupDvdFs(int) {}
void ADXM_ExecMain() {}
void ADXM_SetCbErr(void (*)(void*, const char*), void*) {}
void ADXT_SetOutputMono(int) {}
}

void AIInit(u8*) {}
void AIReset(void) {}

void AXARTInit(void) {}
void AXARTQuit(void) {}
void AXARTServiceSounds(void) {}

AXVPB* AXAcquireVoice(u32, void (*)(void*), u32) { return 0; }
void AXFreeVoice(AXVPB*) {}
u32 AXGetDspCycles(void) { return 0; }
u32 AXGetMaxDspCycles(void) { return 0; }
void AXInitEx(u32) {}
void AXQuit(void) {}
void AXRegisterAuxACallback(void (*)(void*, void*), void*) {}
void AXRegisterAuxBCallback(void (*)(void*, void*), void*) {}
AXCallback AXRegisterCallback(AXCallback) { return 0; }
extern "C" void AXSetCompressor(u32) {}   // declared in snd_sdk.h, not in dolphin/ax.h
void AXSetMode(u32) {}
void AXSetVoiceAddr(AXVPB*, AXPBADDR*) {}
void AXSetVoiceAdpcm(AXVPB*, AXPBADPCM*) {}
void AXSetVoiceAdpcmLoop(AXVPB*, AXPBADPCMLOOP*) {}
void AXSetVoiceEndAddr(AXVPB*, u32) {}
void AXSetVoiceLoop(AXVPB*, u16) {}
void AXSetVoiceLoopAddr(AXVPB*, u32) {}
void AXSetVoiceLpf(AXVPB*, AXPBLPF*) {}
void AXSetVoiceLpfCoefs(AXVPB*, u16, u16) {}
void AXSetVoicePriority(AXVPB*, u32) {}
void AXSetVoiceSrc(AXVPB*, AXPBSRC*) {}
void AXSetVoiceSrcRatio(AXVPB*, f32) {}
void AXSetVoiceSrcType(AXVPB*, u32) {}
void AXSetVoiceState(AXVPB*, u16) {}
void AXSetVoiceType(AXVPB*, u16) {}

extern "C" void AXFXSetHooks(void* (*)(u32), void (*)(void*)) {}   // snd_sdk.h
void AXFXChorusCallback(AXFX_BUFFERUPDATE*, AXFX_CHORUS*) {}
int AXFXChorusInit(AXFX_CHORUS*) { return 1; }
int AXFXChorusSettings(AXFX_CHORUS*) { return 1; }
int AXFXChorusShutdown(AXFX_CHORUS*) { return 1; }
void AXFXDelayCallback(AXFX_BUFFERUPDATE*, AXFX_DELAY*) {}
int AXFXDelayInit(AXFX_DELAY*) { return 1; }
int AXFXDelaySettings(AXFX_DELAY*) { return 1; }
int AXFXDelayShutdown(AXFX_DELAY*) { return 1; }
void AXFXReverbHiCallback(AXFX_BUFFERUPDATE*, AXFX_REVERBHI*) {}
void AXFXReverbHiCallbackDpl2(AXFX_BUFFERUPDATE_DPL2*, AXFX_REVERBHI_DPL2*) {}
int AXFXReverbHiInit(AXFX_REVERBHI*) { return 1; }
int AXFXReverbHiInitDpl2(AXFX_REVERBHI_DPL2*) { return 1; }
int AXFXReverbHiSettings(AXFX_REVERBHI*) { return 1; }
int AXFXReverbHiSettingsDpl2(AXFX_REVERBHI_DPL2*) { return 1; }
int AXFXReverbHiShutdown(AXFX_REVERBHI*) { return 1; }
int AXFXReverbHiShutdownDpl2(AXFX_REVERBHI_DPL2*) { return 1; }
void AXFXReverbStdCallback(AXFX_BUFFERUPDATE*, AXFX_REVERBSTD*) {}
int AXFXReverbStdInit(AXFX_REVERBSTD*) { return 1; }
int AXFXReverbStdSettings(AXFX_REVERBSTD*) { return 1; }
int AXFXReverbStdShutdown(AXFX_REVERBSTD*) { return 1; }

void MIXInit(void) {}
void MIXInitChannel(AXVPB*, u32, int, int, int, int, int, int) {}
void MIXQuit(void) {}
void MIXReleaseChannel(AXVPB*) {}
void MIXSetAuxA(AXVPB*, int) {}
void MIXSetAuxB(AXVPB*, int) {}
void MIXSetInput(AXVPB*, int) {}
void MIXSetPan(AXVPB*, int) {}
void MIXSetSPan(AXVPB*, int) {}
void MIXSetSoundMode(u32) {}
void MIXUpdateSettings(void) {}

void SEQInit(void) {}
void SEQQuit(void) {}
void SEQRunAudioFrame(void) {}

u32 SYNGetActiveNotes(SYNSYNTH*) { return 0; }
void SYNInit(void) {}
void SYNInitSynth(SYNSYNTH*, void*, u32, u32, u32, u32, u32) {}
void SYNMidiInput(SYNSYNTH*, u8*) {}
void SYNQuit(void) {}
void SYNQuitSynth(SYNSYNTH*) {}
void SYNRunAudioFrame(void) {}
void SYNSetMasterVolume(SYNSYNTH*, s32) {}

// Sofdec movie player
int mwPlyCalcWorkCprmSfd(MWS_PLY_CPRM_SFD*) { return 0; }
MWPLY mwPlyCreateSofdec(MWS_PLY_CPRM_SFD*) { return 0; }
void mwPlyFxCnvFrmARGB8888(MWPLY, MWS_FRM*, void*) {}
void mwPlyFxCnvFrmY84C44(MWPLY, MWS_FRM*, void*, void*) {}
void mwPlyFxSetOutBufPitchHeight(MWPLY, int, int) {}
void mwPlyFxSetOutBufSize(MWPLY, int, int) {}
void mwPlyGetCurFrm(MWPLY, MWS_FRM*) {}
void mwPlyGetHdrInf(void*, int, MWS_SFD_HDRINF*) {}
int mwPlyGetNumSkipDec(MWPLY) { return 0; }
int mwPlyGetNumSkipDisp(MWPLY) { return 0; }
void mwPlyInitSfdFx(MWS_PLY_INIT_SFD*) {}
void mwPlyRelCurFrm(MWPLY) {}
