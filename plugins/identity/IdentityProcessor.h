#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class IdentityProcessor final : public juce::AudioProcessor {
 public:
  IdentityProcessor();
  ~IdentityProcessor() override;

  const juce::String getName() const override;
  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>& buffer,
                    juce::MidiBuffer& midiMessages) override;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override;
  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;

  double getTailLengthSeconds() const override;
  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String& newName) override;

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

 private:
  juce::AudioParameterFloat* gainParam_ = nullptr;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
