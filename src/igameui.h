#ifndef LEAFLET_IGAMEUI_H
#define LEAFLET_IGAMEUI_H

/* GoldSrc GameUI007 layout (same as HL1 IGameUI). Used only to call into
 * the already-initialized GameUI.dll singleton -- we do not implement it. */

class IBaseInterface
{
public:
    virtual ~IBaseInterface() {}
};

typedef IBaseInterface *(*CreateInterfaceFn)(const char *name, int *returnCode);

class IGameUI : public IBaseInterface
{
public:
    virtual void Initialize(CreateInterfaceFn *factories, int count) = 0;
    virtual void Start(void *engineFuncs, int interfaceVersion, void *system) = 0;
    virtual void Shutdown(void) = 0;
    virtual int ActivateGameUI(void) = 0;
    virtual int ActivateDemoUI(void) = 0;
    virtual int HasExclusiveInput(void) = 0;
    virtual void RunFrame(void) = 0;
    virtual void ConnectToServer(const char *game, int ip, int port) = 0;
    virtual void DisconnectFromServer(void) = 0;
    virtual void HideGameUI(void) = 0;
    virtual bool IsGameUIActive(void) = 0;
    virtual void LoadingStarted(const char *resourceType, const char *resourceName) = 0;
    virtual void LoadingFinished(const char *resourceType, const char *resourceName) = 0;
    virtual void StartProgressBar(const char *progressType, int progressSteps) = 0;
    virtual int ContinueProgressBar(int progressPoint, float progressFraction) = 0;
    virtual void StopProgressBar(bool error, const char *failureReason, const char *extendedReason) = 0;
    virtual int SetProgressBarStatusText(const char *statusText) = 0;
    virtual void SetSecondaryProgressBar(float progress) = 0;
    virtual void SetSecondaryProgressBarText(const char *statusText) = 0;
};

#define GAMEUI_INTERFACE_VERSION "GameUI007"

#endif
