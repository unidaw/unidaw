#include "plugins/identity/IdentityProcessor.h"

IdentityProcessor::IdentityProcessor()
    : juce::AudioProcessor(juce::AudioProcessor::BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
  addParameter(gainParam_ = new juce::AudioParameterFloat(
      "gain", "Gain", 0.0f, 1.0f, 1.0f));
}

IdentityProcessor::~IdentityProcessor() = default;

const juce::String IdentityProcessor::getName() const {
  return "Identity";
}

void IdentityProcessor::prepareToPlay(double, int) {}

void IdentityProcessor::releaseResources() {}

bool IdentityProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  const auto& mainIn = layouts.getMainInputChannelSet();
  const auto& mainOut = layouts.getMainOutputChannelSet();
  return mainIn == mainOut && (mainIn == juce::AudioChannelSet::mono() ||
                               mainIn == juce::AudioChannelSet::stereo());
}

void IdentityProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  buffer.clear();

  const float gain = gainParam_ ? gainParam_->get() : 0.0f;
  const int numChannels = buffer.getNumChannels();
  const int numSamples = buffer.getNumSamples();

  for (const auto metadata : midiMessages) {
    const auto message = metadata.getMessage();
    if (!message.isNoteOn()) {
      continue;
    }
    const int start = std::max(0, metadata.samplePosition);
    const int end = std::min(numSamples, start + 10);
    for (int ch = 0; ch < numChannels; ++ch) {
      for (int i = start; i < end; ++i) {
        buffer.addSample(ch, i, gain);
      }
    }
  }
}

juce::AudioProcessorEditor* IdentityProcessor::createEditor() { return nullptr; }

bool IdentityProcessor::hasEditor() const { return false; }

bool IdentityProcessor::acceptsMidi() const { return true; }

bool IdentityProcessor::producesMidi() const { return false; }

bool IdentityProcessor::isMidiEffect() const { return false; }

double IdentityProcessor::getTailLengthSeconds() const { return 0.0; }

int IdentityProcessor::getNumPrograms() { return 1; }

int IdentityProcessor::getCurrentProgram() { return 0; }

void IdentityProcessor::setCurrentProgram(int) {}

const juce::String IdentityProcessor::getProgramName(int) { return {}; }

void IdentityProcessor::changeProgramName(int, const juce::String&) {}

void IdentityProcessor::getStateInformation(juce::MemoryBlock&) {}

void IdentityProcessor::setStateInformation(const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new IdentityProcessor();
}
