/*
   Copyright (c) 2019 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#include "JammerNetzPackage.h"

#include "BuffersConfig.h"

JammerNetzSingleChannelSetup::JammerNetzSingleChannelSetup() :
	target(JammerNetzChannelTarget::Unused), volume(1.0f), balanceLeftRight(0.0f)
{
}

JammerNetzSingleChannelSetup::JammerNetzSingleChannelSetup(uint8 target) :
	target(target), volume(1.0f), balanceLeftRight(0.0f)
{
}


JammerNetzChannelSetup::JammerNetzChannelSetup()
{
}

JammerNetzChannelSetup::JammerNetzChannelSetup(std::vector<JammerNetzSingleChannelSetup> const &channelInfo)
{
	jassert(channelInfo.size() < MAXCHANNELSPERCLIENT);
	int i = 0;
	for (auto info : channelInfo) {
		if (i < MAXCHANNELSPERCLIENT) {
			channels[i++] = info;
		}
	}
}

AudioBlock::AudioBlock(double timestamp, uint64 messageCounter, uint16 sampleRate, JammerNetzChannelSetup const &channelSetup, std::shared_ptr<AudioBuffer<float>> audioBuffer) :
	timestamp(timestamp), messageCounter(messageCounter), sampleRate(sampleRate), channelSetup(channelSetup), audioBuffer(audioBuffer)
{
}

std::shared_ptr<JammerNetzMessage> JammerNetzMessage::deserialize(uint8 *data, size_t bytes)
{
	if (bytes >= sizeof(JammerNetzHeader)) {
		JammerNetzHeader *header = reinterpret_cast<JammerNetzHeader*>(data);

		// Check the magic 
		if (header->magic0 == '1' && header->magic1 == '2' && header->magic2 == '3') {
			switch (header->messageType) {
			case AUDIODATA:
				return std::make_shared<JammerNetzAudioData>(data, bytes);
			case CLIENTINFO:
				return std::make_shared<JammerNetzClientInfoMessage>(data, bytes);
			case FLARE:
				return std::make_shared<JammerNetzFlare>();
			default:
				std::cerr << "Unknown message type received, ignoring it" << std::endl;
			}
		}
	}
	return std::shared_ptr<JammerNetzMessage>();
}

int JammerNetzMessage::writeHeader(uint8 *output, uint8 messageType) const
{
	JammerNetzHeader *header = reinterpret_cast<JammerNetzHeader*>(output);
	header->magic0 = '1';
	header->magic1 = '2';
	header->magic2 = '3';
	header->messageType = messageType;
	return sizeof(JammerNetzHeader);
}

// Deserializing constructor
JammerNetzAudioData::JammerNetzAudioData(uint8 *data, int bytes) {
	if (bytes < sizeof(JammerNetzAudioHeader)) {
		jassert(false);
		return;
	}

	// Read the first audio block
	int bytesread = sizeof(JammerNetzHeader);
	audioBlock_ = readAudioHeaderAndBytes(data, bytesread);
	activeBlock_ = audioBlock_;
	
	// Check if there is more data - if yes, it is the FEC info
	while (bytesread < bytes) {
		//TODO - what to do with more than one FEC block?
		fecBlock_ = readAudioHeaderAndBytes(data, bytesread);
#ifdef JAMMER_IS_CLIENT2
		activeBlock_ = fecBlock_;
#endif
	}
}

JammerNetzAudioData::JammerNetzAudioData(uint64 messageCounter, double timestamp, JammerNetzChannelSetup const &channelSetup, std::shared_ptr<AudioBuffer<float>> audioBuffer) 
{
	audioBlock_ = std::make_shared<AudioBlock>();
	audioBlock_->messageCounter = messageCounter;
	audioBlock_->timestamp = timestamp;
	audioBlock_->channelSetup = channelSetup;
	audioBlock_->audioBuffer = audioBuffer;
	activeBlock_ = audioBlock_;
}

JammerNetzAudioData::JammerNetzAudioData(AudioBlock const &audioBlock)
{
	audioBlock_ = std::make_shared<AudioBlock>(audioBlock);
	activeBlock_ = audioBlock_;
}

std::shared_ptr<JammerNetzAudioData> JammerNetzAudioData::createFillInPackage(uint64 messageNumber) const
{	
	if (fecBlock_) {
		return std::make_shared<JammerNetzAudioData>(messageNumber, fecBlock_->timestamp, fecBlock_->channelSetup, fecBlock_->audioBuffer);
	}
	// No FEC data available, fall back to "repeat last package"
	//TODO - fake timestamp?
	return std::make_shared<JammerNetzAudioData>(messageNumber, audioBlock_->timestamp, audioBlock_->channelSetup, audioBlock_->audioBuffer);
}

std::shared_ptr<JammerNetzAudioData> JammerNetzAudioData::createPrePaddingPackage() const
{
	// When a client connects, we want a certain number of packages in the queue, else it will run empty again and the client will be disconnected immediately.
	auto silence = std::make_shared<AudioBuffer<float>>();
	*silence = *audioBlock_->audioBuffer; // Deep copy
	silence->clear();
	return std::make_shared<JammerNetzAudioData>(audioBlock_->messageCounter  - 1, audioBlock_->timestamp, audioBlock_->channelSetup, silence);
}

void JammerNetzAudioData::serialize(uint8 *output, int &byteswritten) const {
	jassert(audioBlock_);
	byteswritten = writeHeader(output, AUDIODATA);
	serialize(output, byteswritten, audioBlock_, 48000, 1);
	if (fecBlock_) {
		serialize(output, byteswritten, fecBlock_, 48000, 2);
	}
}

void JammerNetzAudioData::serialize(uint8 *output, int &byteswritten, std::shared_ptr<AudioBlock> src, uint16 sampleRate, uint16 reductionFactor) const
{
	JammerNetzAudioBlock *block = reinterpret_cast<JammerNetzAudioBlock *>(&output[byteswritten]);
	block->timestamp = src->timestamp;
	block->messageCounter = src->messageCounter;
	block->channelSetup = src->channelSetup;
	block->numberOfSamples = (uint16) src->audioBuffer->getNumSamples() / reductionFactor;
	block->numchannels = (uint8) src->audioBuffer->getNumChannels();
	block->sampleRate = sampleRate / reductionFactor;
	byteswritten += sizeof(JammerNetzAudioHeader);
	appendAudioBuffer(*src->audioBuffer, output, byteswritten, reductionFactor);
}

void JammerNetzAudioData::appendAudioBuffer(AudioBuffer<float> &buffer, uint8 *output, int &writeIndex, uint16 reductionFactor) const {
	for (int inputChannel = 0; inputChannel < buffer.getNumChannels(); inputChannel++) {
		if (reductionFactor == 1) {
		AudioData::Pointer<AudioData::Float32, AudioData::LittleEndian, AudioData::NonInterleaved, AudioData::Const> inputData(buffer.getReadPointer(inputChannel));
		int datasize = buffer.getNumSamples() * sizeof(uint16);
		AudioData::Pointer<AudioData::Int16, AudioData::LittleEndian, AudioData::NonInterleaved, AudioData::NonConst> dataToSend(&output[writeIndex]);
		dataToSend.convertSamples(inputData, buffer.getNumSamples());
		writeIndex += datasize;
	}
		else {
			float tempBuffer[MAXFRAMESIZE];
			const float* read = buffer.getReadPointer(inputChannel);

			long outputSamples = buffer.getNumSamples() / 2;
			for (int i = 0; i < buffer.getNumSamples(); i += reductionFactor) {
				tempBuffer[i / reductionFactor] = read[i];
			}

			AudioData::Pointer<AudioData::Float32, AudioData::LittleEndian, AudioData::NonInterleaved, AudioData::Const> inputData(tempBuffer);
			int datasize = buffer.getNumSamples() / reductionFactor * sizeof(uint16);
			AudioData::Pointer<AudioData::Int16, AudioData::LittleEndian, AudioData::NonInterleaved, AudioData::NonConst> dataToSend(&output[writeIndex]);
			dataToSend.convertSamples(inputData, outputSamples);
			writeIndex += datasize;
		}
	}
}

std::shared_ptr<juce::AudioBuffer<float>> JammerNetzAudioData::audioBuffer() const
{
	return activeBlock_->audioBuffer;
}

juce::uint64 JammerNetzAudioData::messageCounter() const
{
	return activeBlock_->messageCounter;
}

double JammerNetzAudioData::timestamp() const
{
	return activeBlock_->timestamp;
}

JammerNetzChannelSetup JammerNetzAudioData::channelSetup() const
{
	return activeBlock_->channelSetup;
}

std::shared_ptr<AudioBlock> JammerNetzAudioData::readAudioHeaderAndBytes(uint8 *data, int &bytesread) {
	auto result = std::make_shared<AudioBlock>();
	JammerNetzAudioBlock *block = reinterpret_cast<JammerNetzAudioBlock *>(&data[bytesread]);
	result->messageCounter = block->messageCounter;
	result->timestamp = block->timestamp;
	result->channelSetup = block->channelSetup;
	result->sampleRate = 48000;
	int upsampleRate = 48000 / block->sampleRate;
	jassert(block->numberOfSamples * upsampleRate == SAMPLE_BUFFER_SIZE);
	result->audioBuffer = std::make_shared<AudioBuffer<float>>(block->numchannels, block->numberOfSamples * upsampleRate);
	bytesread += sizeof(JammerNetzAudioHeader);
	readAudioBytes(data, block->numchannels, block->numberOfSamples, result->audioBuffer, bytesread, upsampleRate);
	return result;
}

void JammerNetzAudioData::readAudioBytes(uint8 *data, int numchannels, int numsamples, std::shared_ptr<AudioBuffer<float>> destBuffer, int &bytesRead, int upsampleRate) {
	for (int channel = 0; channel < numchannels; channel++) {
		//TODO we might not have enough bytes in the package for this operation
		if (upsampleRate == 1) {
		AudioData::Pointer <AudioData::Int16,
			AudioData::LittleEndian,
			AudioData::NonInterleaved,
			AudioData::Const> src_pointer(&data[bytesRead + channel * numsamples * sizeof(uint16)]);
		AudioData::Pointer<AudioData::Float32,
			AudioData::LittleEndian,
			AudioData::NonInterleaved,
			AudioData::NonConst> dst_pointer(destBuffer->getWritePointer(channel));
		dst_pointer.convertSamples(src_pointer, numsamples);
	}
		else {
			float tempBuffer[MAXFRAMESIZE];
			AudioData::Pointer <AudioData::Int16,
				AudioData::LittleEndian,
				AudioData::NonInterleaved,
				AudioData::Const> src_pointer(&data[bytesRead + channel * numsamples * sizeof(uint16)]);
			AudioData::Pointer<AudioData::Float32,
				AudioData::LittleEndian,
				AudioData::NonInterleaved,
				AudioData::NonConst> dst_pointer(tempBuffer);
			dst_pointer.convertSamples(src_pointer, numsamples);

			auto write = destBuffer->getWritePointer(channel);
			for (int i = 0; i < numsamples; i++) {
				for (int j = 0; j < upsampleRate; j++) {
					write[i * upsampleRate + j] = tempBuffer[i];
				}
			}
		}
	}
	bytesRead += numchannels * numsamples * sizeof(uint16);
}

JammerNetzFlare::JammerNetzFlare()
{
}

void JammerNetzFlare::serialize(uint8 *output, int &byteswritten) const
{
	writeHeader(output, FLARE);
	byteswritten += sizeof(JammerNetzHeader);
}

// Deserializing constructor
JammerNetzClientInfoMessage::JammerNetzClientInfoMessage(uint8 *data, size_t bytes) : data_(data, data + bytes)
{
	if ((bytes < sizeof(JammerNetzHeader) + sizeof(JammerNetzClientInfoHeader)) || 
		(bytes < (sizeof(JammerNetzHeader) + sizeof(JammerNetzClientInfoHeader) + getNumClients() * sizeof(JammerNetzClientInfo)))) {
		// Not enough bytes for message
		jassert(false);
		data_.clear();
	}
}

uint8 JammerNetzClientInfoMessage::getNumClients() const
{
	if (info()) return info()->clientInfoHeader.numConnectedClients;
	return 0;
}

juce::IPAddress JammerNetzClientInfoMessage::getIPAddress(uint8 clientNo) const
{
	if (clientNo < getNumClients()) {
		return IPAddress(info()->clientInfos[clientNo].ipAddress, info()->clientInfos[clientNo].isIPV6);
	}
	return IPAddress("0.0.0.0");
}

const JammerNetzClientInfoPackage * JammerNetzClientInfoMessage::info() const
{
	if (data_.empty()) {
		return nullptr;
	}
	return reinterpret_cast<const JammerNetzClientInfoPackage *>(data_.data());
}

