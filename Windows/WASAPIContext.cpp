#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <avrt.h>
#include <comdef.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string_view>
#include <wrl/client.h>

#include "Common/Log.h"
#include "WASAPIContext.h"

using Microsoft::WRL::ComPtr;

static std::string ConvertWStringToUTF8(const std::wstring &wstr) {
	const int len = (int)wstr.size();
	const int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), len, 0, 0, NULL, NULL);
	std::string s;
	s.resize(size);
	if (size > 0) {
		WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), len, &s[0], size, NULL, NULL);
	}
	return s;
}

static std::wstring ConvertUTF8ToWString(const std::string_view source) {
	const int len = (int)source.size();
	const int size = MultiByteToWideChar(CP_UTF8, 0, source.data(), len, NULL, 0);
	std::wstring str;
	str.resize(size);
	if (size > 0) {
		MultiByteToWideChar(CP_UTF8, 0, source.data(), (int)source.size(), &str[0], size);
	}
	return str;
}

WASAPIContext::WASAPIContext() : notificationClient_(this) {
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
	if (FAILED(hr)) {
		// Bad!
		enumerator_ = nullptr;
		return;
	}
	enumerator_->RegisterEndpointNotificationCallback(&notificationClient_);
}

WASAPIContext::~WASAPIContext() {
	if (!enumerator_) {
		// Nothing can have been happening.
		return;
	}
	Stop();
	enumerator_->UnregisterEndpointNotificationCallback(&notificationClient_);
}

void WASAPIContext::EnumerateDevices(std::vector<AudioDeviceDesc> *output, bool captureDevices) {
	ComPtr<IMMDeviceCollection> collection;
	enumerator_->EnumAudioEndpoints(captureDevices ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection);

	if (!collection) {
		ERROR_LOG(Log::Audio, "Failed to enumerate devices");
		return;
	}

	UINT count = 0;
	collection->GetCount(&count);

	for (UINT i = 0; i < count; ++i) {
		ComPtr<IMMDevice> device;
		collection->Item(i, &device);

		ComPtr<IPropertyStore> props;
		device->OpenPropertyStore(STGM_READ, &props);

		PROPVARIANT nameProp;
		PropVariantInit(&nameProp);
		props->GetValue(PKEY_Device_FriendlyName, &nameProp);

		LPWSTR id_str = 0;
		if (SUCCEEDED(device->GetId(&id_str))) {
			AudioDeviceDesc desc;
			desc.name = ConvertWStringToUTF8(nameProp.pwszVal);
			desc.uniqueId = ConvertWStringToUTF8(id_str);
			output->push_back(desc);
			CoTaskMemFree(id_str);
		}

		PropVariantClear(&nameProp);
	}
}

bool WASAPIContext::InitOutputDevice(std::string_view uniqueId, LatencyMode latencyMode, bool *revertedToDefault) {
	Stop();

	*revertedToDefault = false;

	ComPtr<IMMDevice> device;
	if (uniqueId.empty()) {
		// Use the default device.
		if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
			return false;
		}
	} else {
		// Use whatever device.
		std::wstring wId = ConvertUTF8ToWString(uniqueId);
		if (FAILED(enumerator_->GetDevice(wId.c_str(), &device))) {
			// Fallback to default device
			printf("Falling back to default device...\n");
			*revertedToDefault = true;
			if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
				return false;
			}
		}
	}

	deviceId_ = uniqueId;

	HRESULT hr = E_FAIL;
	// Try IAudioClient3 first if not in "safe" mode. It's probably safe anyway, but still, let's use the legacy client as a safe fallback option.
	if (latencyMode != LatencyMode::Safe) {
		hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&audioClient3_);
	}
	if (SUCCEEDED(hr)) {
		audioClient3_->GetMixFormat(&format_);
		audioClient3_->GetSharedModeEnginePeriod(format_, &defaultPeriodFrames, &fundamentalPeriodFrames, &minPeriodFrames, &maxPeriodFrames);

		printf("default: %d fundamental: %d min: %d max: %d\n", (int)defaultPeriodFrames, (int)fundamentalPeriodFrames, (int)minPeriodFrames, (int)maxPeriodFrames);
		printf("initializing with %d frame period at %d Hz, meaning %0.1fms\n", (int)minPeriodFrames, (int)format_->nSamplesPerSec, FramesToMs(minPeriodFrames, format_->nSamplesPerSec));

		audioEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		HRESULT result = audioClient3_->InitializeSharedAudioStream(
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			minPeriodFrames,
			format_,
			nullptr
		);
		if (FAILED(result)) {
			printf("Error initializing shared audio stream: %08lx", result);
			audioClient3_.Reset();
			return false;
		}
		actualPeriodFrames_ = minPeriodFrames;

		audioClient3_->GetBufferSize(&reportedBufferSize_);
		audioClient3_->SetEventHandle(audioEvent_);
		audioClient3_->GetService(IID_PPV_ARGS(&renderClient_));
	} else {
		// Fallback to IAudioClient (older OS)
		device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);

		audioClient_->GetMixFormat(&format_);

		// Get engine period info
		REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
		audioClient_->GetDevicePeriod(&defaultPeriod, &minPeriod);

		audioEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		const REFERENCE_TIME duration = minPeriod;
		HRESULT hr = audioClient_->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			duration,  // This is a minimum, the result might be larger. We use GetBufferSize to check.
			0,  // ref duration, always 0 in shared mode.
			format_,
			nullptr
		);

		if (FAILED(hr)) {
			printf("ERROR: Failed to initialize audio with all attempted buffer sizes\n");
			audioClient_.Reset();
			return false;
		}
		audioClient_->GetBufferSize(&reportedBufferSize_);
		actualPeriodFrames_ = reportedBufferSize_;  // we don't have a better estimate.
		audioClient_->SetEventHandle(audioEvent_);
		audioClient_->GetService(IID_PPV_ARGS(&renderClient_));
	}

	latencyMode_ = latencyMode;

	Start();

	return true;
}

void WASAPIContext::Start() {
	running_ = true;
	audioThread_ = std::thread([this]() { AudioLoop(); });
}

void WASAPIContext::Stop() {
	running_ = false;
	if (audioClient_) audioClient_->Stop();
	if (audioEvent_) SetEvent(audioEvent_);
	if (audioThread_.joinable()) audioThread_.join();

	renderClient_.Reset();
	audioClient_.Reset();
	if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
	if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
}

void WASAPIContext::FrameUpdate(bool allowAutoChange) {
	if (deviceId_.empty() && defaultDeviceChanged_ && allowAutoChange) {
		defaultDeviceChanged_ = false;
		Stop();
		Start();
	}
}

void WASAPIContext::AudioLoop() {
	DWORD taskID = 0;
	HANDLE mmcssHandle = nullptr;
	if (latencyMode_ == LatencyMode::Aggressive) {
		mmcssHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &taskID);
	}

	UINT32 available;
	if (audioClient3_) {
		audioClient3_->Start();
		audioClient3_->GetBufferSize(&available);
	} else {
		audioClient_->Start();
		audioClient_->GetBufferSize(&available);
	}

	while (running_) {
		const DWORD waitResult = WaitForSingleObject(audioEvent_, INFINITE);
		if (waitResult != WAIT_OBJECT_0) {
			// Something bad happened.
			break;
		}

		UINT32 padding = 0;
		if (audioClient3_) {
			audioClient3_->GetCurrentPadding(&padding);
		} else {
			audioClient_->GetCurrentPadding(&padding);
		}

		const UINT32 framesToWrite = available - padding;
		BYTE* buffer = nullptr;
		if (framesToWrite > 0 && SUCCEEDED(renderClient_->GetBuffer(framesToWrite, &buffer))) {
			callback_(reinterpret_cast<float *>(buffer), framesToWrite, format_->nChannels, format_->nSamplesPerSec, userdata_);
			renderClient_->ReleaseBuffer(framesToWrite, 0);
		}

		// In the old mode, we just estimate the "actualPeriodFrames_" from the framesToWrite.
		if (audioClient_ && framesToWrite < actualPeriodFrames_) {
			actualPeriodFrames_ = framesToWrite;
		}
	}

	if (audioClient3_) {
		audioClient3_->Stop();
	} else {
		audioClient_->Stop();
	}

	if (mmcssHandle) {
		AvRevertMmThreadCharacteristics(mmcssHandle);
	}
}

void WASAPIContext::DescribeOutputFormat(char *buffer, size_t bufferSize) const {
	const int numChannels = format_->nChannels;
	const int sampleBits = format_->wBitsPerSample;
	const int sampleRateHz = format_->nSamplesPerSec;
	const char *fmt = "N/A";
	if (format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)format_;
		if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
			fmt = "float";
		} else {
			fmt = "PCM";
		}
	} else {
		fmt = "PCM";  // probably
	}
	snprintf(buffer, bufferSize, "%d Hz %s %d-bit, %d ch", sampleRateHz, fmt, sampleBits, numChannels);
}
