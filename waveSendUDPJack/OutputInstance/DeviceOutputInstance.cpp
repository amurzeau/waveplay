#include "DeviceOutputInstance.h"
#include <stdio.h>

DeviceOutputInstance::DeviceOutputInstance(OscContainer* parent)
    : OscContainer(parent, "deviceOutput"),
      stream(nullptr),
      oscDeviceName(this, "deviceName", "default_out"),
      oscClockDrift(this, "clockDrift", 1.0f),
      oscDeviceSampleRate(this, "deviceSampleRate", 48000) {
	oscClockDrift.setOscConverters([](float v) { return v - 1.0f; }, [](float v) { return v + 1.0f; });
	oscClockDrift.setChangeCallback([this](float newValue) {
		for(auto& resamplingFilter : resamplingFilters) {
			resamplingFilter.setClockDrift(newValue);
		}
	});
	oscDeviceSampleRate.setChangeCallback([this](float newValue) {
		for(auto& resamplingFilter : resamplingFilters) {
			resamplingFilter.setTargetSamplingRate(newValue);
		}
	});
}

std::vector<std::string> DeviceOutputInstance::getDeviceList() {
	std::vector<std::string> result;
	int numDevices = Pa_GetDeviceCount();

	for(int i = 0; i < numDevices; i++) {
		const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
		std::string name;

		/*if(deviceInfo->maxOutputChannels <= 0)
		    continue;*/

		name = std::string(Pa_GetHostApiInfo(deviceInfo->hostApi)->name) + "::" + std::string(deviceInfo->name);
		result.push_back(name);
	}

	return result;
}

int DeviceOutputInstance::getDeviceIndex(const std::string& name) {
	int numDevices = Pa_GetDeviceCount();

	if(name == "default_out") {
		return Pa_GetDefaultOutputDevice();
	}

	for(int i = 0; i < numDevices; i++) {
		const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
		std::string devName;

		devName = std::string(Pa_GetHostApiInfo(deviceInfo->hostApi)->name) + "::" + std::string(deviceInfo->name);
		if(devName == name)
			return i;
	}

	return -1;
}

void DeviceOutputInstance::stop() {
	if(stream) {
		Pa_StopStream(stream);
		Pa_CloseStream(stream);
	}
	stream = nullptr;
}

const char* DeviceOutputInstance::getName() {
	return "device";
}

int DeviceOutputInstance::start(int index, size_t numChannel, int sampleRate, int jackBufferSize) {
	PaStreamParameters outputParameters;
	int outputDeviceIndex;

	stream = nullptr;

	outputDeviceIndex = getDeviceIndex(oscDeviceName);

	doDebug = outputDeviceIndex == 24;

	if(outputDeviceIndex < 0 || outputDeviceIndex >= Pa_GetDeviceCount()) {
		printf("Bad portaudio output device %s\n", oscDeviceName.c_str());
		return paInvalidDevice;
	}

	resampledBuffer.reserve(sampleRate);
	resamplingFilters.resize(numChannel);
	ringBuffers.clear();
	for(size_t i = 0; i < numChannel; i++) {
		std::unique_ptr<jack_ringbuffer_t, void (*)(jack_ringbuffer_t*)> buffer(nullptr, &jack_ringbuffer_free);
		buffer.reset(jack_ringbuffer_create(jackBufferSize * 10 * sizeof(jack_default_audio_sample_t)));
		ringBuffers.emplace_back(std::move(buffer));

		resamplingFilters[i].reset(sampleRate);
		resamplingFilters[i].setTargetSamplingRate(oscDeviceSampleRate);
		resamplingFilters[i].setClockDrift(oscClockDrift);
	}

	outputParameters.device = outputDeviceIndex;
	outputParameters.channelCount = numChannel;
	outputParameters.sampleFormat = paFloat32 | paNonInterleaved;  // 32 bit floating point output
	outputParameters.suggestedLatency = jackBufferSize * 1.0f / sampleRate;
	outputParameters.hostApiSpecificStreamInfo = NULL;

	int ret = Pa_OpenStream(&stream,
	                        nullptr,
	                        &outputParameters,
	                        oscDeviceSampleRate,
	                        paFramesPerBufferUnspecified,
	                        paClipOff | paDitherOff,
	                        &renderCallback,
	                        this);
	if(ret != paNoError) {
		printf("Portaudio open error: %s(%d), device: %s::%s\n",
		       Pa_GetErrorText(ret),
		       ret,
		       Pa_GetHostApiInfo(Pa_GetDeviceInfo(outputDeviceIndex)->hostApi)->name,
		       Pa_GetDeviceInfo(outputDeviceIndex)->name);
		return ret;
	} else {
		printf("Using output device %d %s, %s with latency %.3f\n",
		       outputDeviceIndex,
		       Pa_GetHostApiInfo(Pa_GetDeviceInfo(outputDeviceIndex)->hostApi)->name,
		       Pa_GetDeviceInfo(outputDeviceIndex)->name,
		       Pa_GetStreamInfo(stream)->outputLatency);
	}

	bufferLatencyMeasurePeriodSize = 60 * sampleRate / jackBufferSize;
	bufferLatencyNr = 0;
	bufferLatencyHistory.clear();
	bufferLatencyHistory.reserve(bufferLatencyMeasurePeriodSize);
	previousAverageLatency = 0;
	clockDriftPpm = 0;
	isPaRunning = false;
	printf("Using buffer size %d, device sample rate: %d\n", jackBufferSize, (int) oscDeviceSampleRate);

	Pa_StartStream(stream);

	return ret;
}

int DeviceOutputInstance::postProcessSamples(float** samples, size_t numChannel, uint32_t nframes) {
	if(!stream)
		return 0;

	for(size_t i = 0; i < numChannel; i++) {
		size_t dataSize;
		resamplingFilters[i].processSamples(resampledBuffer, samples[i], nframes);

		dataSize = resampledBuffer.size() * sizeof(resampledBuffer[0]);

		size_t availableData = jack_ringbuffer_write_space(ringBuffers[i].get());

		if(dataSize < availableData) {
			jack_ringbuffer_write(ringBuffers[i].get(), (const char*) resampledBuffer.data(), dataSize);
		} else {
			overflowOccured = true;
			overflowSize = availableData;
		}
	}

	//	jack_nframes_t current_frames;
	//	jack_time_t current_usecs;
	//	jack_time_t next_usecs;
	//	float period_usecs;
	//	jack_nframes_t call_frames;
	//	jack_time_t call_time_time;
	//	getLiveFrameTime(&current_frames, &current_usecs, &next_usecs, &period_usecs, &call_frames, &call_time_time);

	//	long available = Pa_GetStreamWriteAvailable(stream);
	//	currentJackTime = (call_time_time - current_usecs) / 1000000.0;

	//	const PaStreamInfo* streamInfo = Pa_GetStreamInfo(stream);

	//	currentPaTime =
	//	    currentJackTime + streamInfo->outputLatency - available / 48000.0 / resamplingFilters[0].getClockDrift();

	return 0;
}

int DeviceOutputInstance::renderCallback(const void* input,
                                         void* output,
                                         unsigned long frameCount,
                                         const PaStreamCallbackTimeInfo* timeInfo,
                                         PaStreamCallbackFlags statusFlags,
                                         void* userData) {
	DeviceOutputInstance* thisInstance = (DeviceOutputInstance*) userData;

	jack_default_audio_sample_t** outputBuffers = (jack_default_audio_sample_t**) output;

	bool underflowOccured = false;
	size_t availableData = 0;

	thisInstance->isPaRunning = true;

	for(size_t i = 0; i < thisInstance->ringBuffers.size(); i++) {
		availableData = jack_ringbuffer_read_space(thisInstance->ringBuffers[i].get());

		if(availableData >= frameCount * sizeof(outputBuffers[i][0])) {
			jack_ringbuffer_read(
			    thisInstance->ringBuffers[i].get(), (char*) outputBuffers[i], frameCount * sizeof(outputBuffers[i][0]));
		} else {
			memset(reinterpret_cast<char*>(outputBuffers[i]), 0, frameCount * sizeof(outputBuffers[i][0]));
			underflowOccured = true;
			thisInstance->underflowSize = availableData;
		}
	}

	if(underflowOccured) {
		thisInstance->underflowOccured = true;
		thisInstance->bufferLatencyHistory.clear();
	} else if(availableData) {
		thisInstance->bufferLatencyHistory.push_back(availableData / sizeof(jack_default_audio_sample_t));

		if(thisInstance->bufferLatencyHistory.size() == thisInstance->bufferLatencyMeasurePeriodSize) {
			double averageLatency = 0;
			for(auto value : thisInstance->bufferLatencyHistory) {
				averageLatency += value;
			}
			averageLatency /= thisInstance->bufferLatencyHistory.size();
			thisInstance->bufferLatencyHistory.clear();

			if(thisInstance->previousAverageLatency != 0) {
				double sampleLatencyDiffOver1s = averageLatency - thisInstance->previousAverageLatency;
				double sampleMeasureDuration = thisInstance->bufferLatencyMeasurePeriodSize * frameCount;

				// Ignore drift of more than 0.1%
				//				if(sampleLatencyDiffOver1s > (-sampleMeasureDuration / 1000) &&
				//				   sampleLatencyDiffOver1s < (sampleMeasureDuration / 1000)) {
				thisInstance->clockDriftPpm = 1000000.0 * sampleLatencyDiffOver1s / sampleMeasureDuration;
				//				}
			}

			thisInstance->previousAverageLatency = averageLatency;
		}
	}

	return paContinue;
}

void DeviceOutputInstance::onTimer() {
	//	static unsigned int counter;

	//	if((counter % 30) == 0 && doDebug)
	//		printf("%.6f, %.6f, %.6f, j: %.6f, b: %.6f\n",
	//		       currentJackTime - currentPaTime,
	//		       currentPaTime,
	//		       currentJackTime,
	//		       nextBufferedSampleJackTime / 48000.0,
	//		       jack_ringbuffer_read_space(ringBuffers[0].get()) / sizeof(float) / 48000.0);
	//	counter++;

	if(overflowOccured) {
		printf("%s: Overflow: %d, %d\n", oscDeviceName.c_str(), bufferLatencyNr, (int) overflowSize);
		overflowOccured = false;
	}
	if(underflowOccured) {
		printf("%s: underrun: %d, %d\n", oscDeviceName.c_str(), bufferLatencyNr, (int) underflowSize);
		underflowOccured = false;
	}
	if(clockDriftPpm) {
		printf("%s: average latency: %f\n", oscDeviceName.c_str(), previousAverageLatency);
		printf("%s: drift: %f\n", oscDeviceName.c_str(), clockDriftPpm);
		clockDriftPpm = 0;
	}

	if(!isPaRunning) {
		printf("%s: portaudio not running !\n", oscDeviceName.c_str());
	} else {
		isPaRunning = false;
	}
}
