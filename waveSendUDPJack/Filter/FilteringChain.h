#ifndef FILTERINGCHAIN_H
#define FILTERINGCHAIN_H

#include "../OscAddress.h"
#include "../json.h"
#include "CompressorFilter.h"
#include "DelayFilter.h"
#include "DitheringFilter.h"
#include "EqFilter.h"
#include "ExpanderFilter.h"
#include "ReverbFilter.h"
#include <array>
#include <stddef.h>
#include <tuple>

class FilterChain : public OscContainer {
public:
	FilterChain(OscContainer* parent);

	void init(size_t numChannel);
	void reset(double fs);
	void processSamples(float* peakOutput, float** output, const float** input, size_t numChannel, size_t count);
	float processSideChannelSample(float input);

	void setParameters(const nlohmann::json& json);
	nlohmann::json getParameters();

private:
	std::vector<DelayFilter> delayFilters;
	std::vector<ReverbFilter> reverbFilters;
	bool eqFiltersEnabled = false;
	std::vector<EqFilter> eqFilters;
	std::vector<float> volume;
	OscVariable<float, ConverterLogScale> masterVolume;
	CompressorFilter compressorFilter;
	ExpanderFilter expanderFilter;
	bool mute = false;
	bool enabled = true;
};

#endif
