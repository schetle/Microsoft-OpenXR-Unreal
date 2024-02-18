#include "SecondaryViewConfiguration.h"

#include "MicrosoftOpenXR.h"
#include "OpenXRCore.h"

namespace
{
	TArray<XrViewConfigurationType> PluginSupportedSecondaryViewConfigTypes = {
		XR_VIEW_CONFIGURATION_TYPE_SECONDARY_MONO_FIRST_PERSON_OBSERVER_MSFT};

#define XR_ENUM_CASE_STR(name, val) \
	case name:                      \
		return TEXT(#name);
	constexpr const TCHAR* ViewConfigTypeToString(XrViewConfigurationType v)
	{
		switch (v)
		{
			XR_LIST_ENUM_XrViewConfigurationType(XR_ENUM_CASE_STR) default : return TEXT("Unknown");
		}
	}
}	 // namespace

namespace MicrosoftOpenXR
{
	void FSecondaryViewConfigurationPlugin::Register()
	{
		// Secondary view feature can trigger engine bug in 5.0 so this plugin is disabled until it is fixed.
		// IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), this);
	}

	void FSecondaryViewConfigurationPlugin::Unregister()
	{
		// Secondary view feature can trigger engine bug in 5.0 so this plugin is disabled until it is fixed.
		// IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), this);
	}

	bool FSecondaryViewConfigurationPlugin::GetRequiredExtensions(TArray<const ANSICHAR*>& OutExtensions)
	{
		OutExtensions.Add(XR_MSFT_SECONDARY_VIEW_CONFIGURATION_EXTENSION_NAME);
		return true;
	}

	bool FSecondaryViewConfigurationPlugin::GetOptionalExtensions(TArray<const ANSICHAR*>& OutExtensions)
	{
		OutExtensions.Add(XR_MSFT_FIRST_PERSON_OBSERVER_EXTENSION_NAME);
		return true;
	}

	void FSecondaryViewConfigurationPlugin::PostGetSystem(XrInstance InInstance, XrSystemId InSystem)
	{
		Instance = InInstance;
		System = InSystem;
	}

	const void* FSecondaryViewConfigurationPlugin::OnBeginSession(XrSession InSession, const void* InNext)
	{
		check(Instance != XR_NULL_HANDLE);
		check(System != XR_NULL_SYSTEM_ID);

		uint32_t ConfigurationCount;
		XR_ENSURE_MSFT(xrEnumerateViewConfigurations(Instance, System, 0, &ConfigurationCount, nullptr));
		TArray<XrViewConfigurationType> AvailableViewConfigTypes;
		AvailableViewConfigTypes.SetNum(ConfigurationCount);
		XR_ENSURE_MSFT(xrEnumerateViewConfigurations(
			Instance, System, ConfigurationCount, &ConfigurationCount, AvailableViewConfigTypes.GetData()));

		// Generate the overlap of the view configuration types supported by this plugin and the runtime and
		// set up some of the core structs.
		EnabledViewConfigTypes.Reset();
		EnabledViewConfigEnvBlendModes.Reset();
		SecondaryViewState_GameThread.SecondaryViewConfigStates.Reset();
		EnabledViewConfigurationViews.Reset();
		for (XrViewConfigurationType ViewConfigType : PluginSupportedSecondaryViewConfigTypes)
		{
			if (!AvailableViewConfigTypes.Contains(ViewConfigType))
			{
				continue;	 // Runtime doesn't support this secondary view config type.
			}

			EnabledViewConfigTypes.Add(ViewConfigType);

			// Store the corresponding blend mode to use for this view configuration type.
			uint32_t EnvBlendModeCount = 0;
			XR_ENSURE_MSFT(xrEnumerateEnvironmentBlendModes(Instance, System, ViewConfigType, 0, &EnvBlendModeCount, nullptr));
			TArray<XrEnvironmentBlendMode> EnvBlendModes;
			EnvBlendModes.SetNum(EnvBlendModeCount);
			XR_ENSURE_MSFT(xrEnumerateEnvironmentBlendModes(
				Instance, System, ViewConfigType, EnvBlendModeCount, &EnvBlendModeCount, EnvBlendModes.GetData()));
			EnabledViewConfigEnvBlendModes.Add(EnvBlendModes[0]);

			XrSecondaryViewConfigurationStateMSFT viewState = {XR_TYPE_SECONDARY_VIEW_CONFIGURATION_STATE_MSFT};
			viewState.viewConfigurationType = ViewConfigType;
			SecondaryViewState_GameThread.SecondaryViewConfigStates.Add(viewState);

			// Enumerate the view configuration's views
			uint32_t ViewConfigCount = 0;
			XR_ENSURE_MSFT(xrEnumerateViewConfigurationViews(Instance, System, ViewConfigType, 0, &ViewConfigCount, nullptr));
			TArray<XrViewConfigurationView> Views;
			Views.SetNum(ViewConfigCount);
			for (uint32_t i = 0; i < ViewConfigCount; i++)
			{
				Views[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
			}
			XR_ENSURE_MSFT(xrEnumerateViewConfigurationViews(
				Instance, System, ViewConfigType, ViewConfigCount, &ViewConfigCount, Views.GetData()));
			EnabledViewConfigurationViews.Add(std::move(Views));
		}

		// It is only legal to chain in the secondary view configuration information if there is one or more supported secondary
		// view configurations being enabled.
		if (EnabledViewConfigTypes.Num() == 0)
		{
			return InNext;
		}

		SecondaryViewConfigurationSessionBeginInfo.type = XR_TYPE_SECONDARY_VIEW_CONFIGURATION_SESSION_BEGIN_INFO_MSFT;
		SecondaryViewConfigurationSessionBeginInfo.next = InNext;
		SecondaryViewConfigurationSessionBeginInfo.viewConfigurationCount = EnabledViewConfigTypes.Num();
		SecondaryViewConfigurationSessionBeginInfo.enabledViewConfigurationTypes = EnabledViewConfigTypes.GetData();
		return &SecondaryViewConfigurationSessionBeginInfo;
	}

	void* FSecondaryViewConfigurationPlugin::OnWaitFrame(XrSession InSession, void* InNext)
	{
		// If there are no enabled seconary view configs there is no need to query their state.
		if (EnabledViewConfigTypes.Num() == 0)
		{
			return InNext;
		}

		check(IsInGameThread());
		check(SecondaryViewState_GameThread.SecondaryViewConfigStates.Num() == EnabledViewConfigTypes.Num());

		SecondaryViewConfigurationFrameState.type = XR_TYPE_SECONDARY_VIEW_CONFIGURATION_FRAME_STATE_MSFT;
		SecondaryViewConfigurationFrameState.next = InNext;
		SecondaryViewConfigurationFrameState.viewConfigurationCount = SecondaryViewState_GameThread.SecondaryViewConfigStates.Num();
		SecondaryViewConfigurationFrameState.viewConfigurationStates =
			SecondaryViewState_GameThread.SecondaryViewConfigStates.GetData();
		return &SecondaryViewConfigurationFrameState;
	}

	const void* FSecondaryViewConfigurationPlugin::OnBeginFrame(XrSession InSession, XrTime DisplayTime, const void* InNext)
	{
		// Log when the active state of a secondary view config changes. Ideally this would be done immediately after xrWaitFrame
		// completes but there is no "PostWaitFrame" callback.
		const int SharedViewCount = FMath::Min(SecondaryViewState_RenderThread.SecondaryViewConfigStates.Num(),
			SecondaryViewState_GameThread.SecondaryViewConfigStates.Num());
		for (int ViewIndex = 0; ViewIndex < SharedViewCount; ViewIndex++)
		{
			const XrSecondaryViewConfigurationStateMSFT& RenderViewState =
				SecondaryViewState_RenderThread.SecondaryViewConfigStates[ViewIndex];
			const XrSecondaryViewConfigurationStateMSFT& GameViewState =
				SecondaryViewState_GameThread.SecondaryViewConfigStates[ViewIndex];
			if (GameViewState.active != RenderViewState.active)
			{
				UE_LOG(LogHMD, Log, TEXT("Secondary view configuration %s changed to %s"),
					ViewConfigTypeToString(RenderViewState.viewConfigurationType),
					RenderViewState.active ? TEXT("active") : TEXT("inactive"));
			}
		}

		// xrBeginFrame corresponds to the previous xrWaitFrame. After xrBeginFrame completes (after this callback is completed), a
		// subsequent xrWaitFrame can begin. Because xrBeginFrame acts as a synchronization point with xrWaitFrame, no lock is
		// needed to clone over state for the rendering operations.
		SecondaryViewState_RenderThread = SecondaryViewState_GameThread;

		return InNext;
	}

	FSecondaryViewConfigurationPlugin::PiplinedFrameState& FSecondaryViewConfigurationPlugin::GetSecondaryViewStateForThread()
	{
		if (IsInGameThread())
		{
			return SecondaryViewState_GameThread;
		}
		else
		{
			check(IsInRenderingThread() || IsInRHIThread());
			return SecondaryViewState_RenderThread;
		}
	}
}	 // namespace MicrosoftOpenXR