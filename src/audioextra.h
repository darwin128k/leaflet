#ifndef AUDIOEXTRA_H
#define AUDIOEXTRA_H

#include <windows.h>

/* OpenAL / MetaAudio controls on the stock GameUI Audio page.
 * Extra widgets live in cstrike/resource/OptionsSubAudio.res; this binds
 * them to al_* cvars because this GameUI build does not read cvar_name. */
void AudioExtra_Init(HMODULE hGameUI);
void AudioExtra_Tick(void);
void AudioExtra_BindPage(void *audioPage);
void AudioExtra_BindVoicePage(void *voicePage);
float AudioExtra_VuLevel(int right);
void AudioExtra_SyncToggle(void *btn);
void AudioExtra_OnSliderPaint(void *slider);
void AudioExtra_EnsureDopplerSlider(void *audioPage);
int AudioExtra_HasMetaAudio(void);

#endif
