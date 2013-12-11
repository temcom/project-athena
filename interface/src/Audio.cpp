//
//  Audio.cpp
//  interface
//
//  Created by Stephen Birarda on 1/22/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <cstring>
#include <sys/stat.h>

#ifdef __APPLE__
#include <CoreAudio/AudioHardware.h>
#endif

#include <QtMultimedia/QAudioInput>
#include <QtMultimedia/QAudioOutput>
#include <QSvgRenderer>

#include <AngleUtil.h>
#include <NodeList.h>
#include <NodeTypes.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>
#include <StdDev.h>
#include <UUID.h>

#include "Application.h"
#include "Audio.h"
#include "Menu.h"
#include "Util.h"

static const float JITTER_BUFFER_LENGTH_MSECS = 12;
static const short JITTER_BUFFER_SAMPLES = JITTER_BUFFER_LENGTH_MSECS * NUM_AUDIO_CHANNELS * (SAMPLE_RATE / 1000.0);

static const float AUDIO_CALLBACK_MSECS = (float)BUFFER_LENGTH_SAMPLES_PER_CHANNEL / (float)SAMPLE_RATE * 1000.0;

// Mute icon configration
static const int ICON_SIZE = 24;
static const int ICON_LEFT = 20;
static const int BOTTOM_PADDING = 110;

Audio::Audio(Oscilloscope* scope, int16_t initialJitterBufferSamples, QObject* parent) :
    QObject(parent),
    _audioInput(NULL),
    _desiredInputFormat(),
    _inputFormat(),
    _inputDevice(NULL),
    _inputBuffer(),
    _numInputCallbackBytes(0),
    _audioOutput(NULL),
    _desiredOutputFormat(),
    _outputFormat(),
    _outputDevice(NULL),
    _outputBuffer(),
    _numOutputCallbackBytes(0),
    _nextOutputSamples(NULL),
    _ringBuffer(true),
    _scope(scope),
    _averagedLatency(0.0),
    _measuredJitter(0),
    _jitterBufferSamples(initialJitterBufferSamples),
    _lastInputLoudness(0),
    _lastVelocity(0),
    _lastAcceleration(0),
    _totalPacketsReceived(0),
    _collisionSoundMagnitude(0.0f),
    _collisionSoundFrequency(0.0f),
    _collisionSoundNoise(0.0f),
    _collisionSoundDuration(0.0f),
    _proceduralEffectSample(0),
    _numFramesDisplayStarve(0),
    _muted(false)
{
    
}

void Audio::init(QGLWidget *parent) {
    switchToResourcesParentIfRequired();
    _micTextureId = parent->bindTexture(QImage("./resources/images/mic.svg"));
    _muteTextureId = parent->bindTexture(QImage("./resources/images/mute.svg"));
}

void Audio::reset() {
    _ringBuffer.reset();
}

QAudioDeviceInfo defaultAudioDeviceForMode(QAudio::Mode mode) {
#ifdef __APPLE__
    if (QAudioDeviceInfo::availableDevices(mode).size() > 1) {
        AudioDeviceID defaultDeviceID = 0;
        uint32_t propertySize = sizeof(AudioDeviceID);
        AudioObjectPropertyAddress propertyAddress = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMaster
        };
        
        if (mode == QAudio::AudioOutput) {
            propertyAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        }
        
        
        OSStatus getPropertyError = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                               &propertyAddress,
                                                               0,
                                                               NULL,
                                                               &propertySize,
                                                               &defaultDeviceID);
        
        if (!getPropertyError && propertySize) {
            CFStringRef deviceName = NULL;
            propertySize = sizeof(deviceName);
            propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
            getPropertyError = AudioObjectGetPropertyData(defaultDeviceID, &propertyAddress, 0,
                                                          NULL, &propertySize, &deviceName);
            
            if (!getPropertyError && propertySize) {
                // find a device in the list that matches the name we have and return it
                foreach(QAudioDeviceInfo audioDevice, QAudioDeviceInfo::availableDevices(mode)) {
                    if (audioDevice.deviceName() == CFStringGetCStringPtr(deviceName, kCFStringEncodingMacRoman)) {
                        return audioDevice;
                    }
                }
            }
        }
    }
#endif
    
    // fallback for failed lookup is the default device
    return (mode == QAudio::AudioInput) ? QAudioDeviceInfo::defaultInputDevice() : QAudioDeviceInfo::defaultOutputDevice();
}

bool adjustedFormatForAudioDevice(const QAudioDeviceInfo& audioDevice,
                                  const QAudioFormat& desiredAudioFormat,
                                  QAudioFormat& adjustedAudioFormat) {
    if (!audioDevice.isFormatSupported(desiredAudioFormat)) {
        qDebug() << "The desired format for audio I/O is" << desiredAudioFormat << "\n";
        qDebug() << "The desired audio format is not supported by this device.\n";
        
        if (desiredAudioFormat.channelCount() == 1) {
            adjustedAudioFormat = desiredAudioFormat;
            adjustedAudioFormat.setChannelCount(2);
            
            if (audioDevice.isFormatSupported(adjustedAudioFormat)) {
                return true;
            } else {
                adjustedAudioFormat.setChannelCount(1);
            }
        }
        
        if (audioDevice.supportedSampleRates().contains(SAMPLE_RATE * 2)) {
            // use 48, which is a sample downsample, upsample
            adjustedAudioFormat = desiredAudioFormat;
            adjustedAudioFormat.setSampleRate(SAMPLE_RATE * 2);
            
            // return the nearest in case it needs 2 channels
            adjustedAudioFormat = audioDevice.nearestFormat(adjustedAudioFormat);
            return true;
        }
        
        return false;
    } else {
        // set the adjustedAudioFormat to the desiredAudioFormat, since it will work
        adjustedAudioFormat = desiredAudioFormat;
        return true;
    }
}

void nearestNeighborResampling(int16_t* sourceSamples, int16_t* destinationSamples,
                               int numDestinationSamples,
                               const QAudioFormat& sourceAudioFormat, const QAudioFormat& destinationAudioFormat) {
    if (sourceAudioFormat == destinationAudioFormat) {
        memcpy(destinationSamples, sourceSamples, numDestinationSamples * sizeof(int16_t));
    } else {
        float sourceMultiplier = sourceAudioFormat.sampleRate() / (float) destinationAudioFormat.sampleRate();
        
        // take into account the number of channels in source and destination
        // accomodate for the case where have an output with > 2 channels
        // this is the case with our HDMI capture
        sourceMultiplier *= sourceAudioFormat.channelCount()
            / (float) (destinationAudioFormat.channelCount() > 2 ? 2 : destinationAudioFormat.channelCount());
        
        for (int i = 0; i < numDestinationSamples; i++) {
            destinationSamples[i] = sourceSamples[static_cast<int>(i * sourceMultiplier)];
        }
    }
}

const int CALLBACK_ACCELERATOR_RATIO = 2;

void Audio::start() {
    
    // set up the desired audio format
    _desiredInputFormat.setSampleRate(SAMPLE_RATE);
    _desiredInputFormat.setSampleSize(16);
    _desiredInputFormat.setCodec("audio/pcm");
    _desiredInputFormat.setSampleType(QAudioFormat::SignedInt);
    _desiredInputFormat.setByteOrder(QAudioFormat::LittleEndian);
    _desiredInputFormat.setChannelCount(1);
    
    _desiredOutputFormat = _desiredInputFormat;
    _desiredOutputFormat.setChannelCount(2);
    
    QAudioDeviceInfo inputDeviceInfo = defaultAudioDeviceForMode(QAudio::AudioInput);
    
    qDebug() << "The audio input device is" << inputDeviceInfo.deviceName() << "\n";
    
    if (adjustedFormatForAudioDevice(inputDeviceInfo, _desiredInputFormat, _inputFormat)) {
        qDebug() << "The format to be used for audio input is" << _inputFormat << "\n";
        
        _audioInput = new QAudioInput(inputDeviceInfo, _inputFormat, this);
        _numInputCallbackBytes = BUFFER_LENGTH_BYTES_PER_CHANNEL * _inputFormat.channelCount()
            * (_inputFormat.sampleRate() / SAMPLE_RATE)
            / CALLBACK_ACCELERATOR_RATIO;
        _audioInput->setBufferSize(_numInputCallbackBytes);
        
        QAudioDeviceInfo outputDeviceInfo = defaultAudioDeviceForMode(QAudio::AudioOutput);
        
        qDebug() << "The audio output device is" << outputDeviceInfo.deviceName() << "\n";

        if (adjustedFormatForAudioDevice(outputDeviceInfo, _desiredOutputFormat, _outputFormat)) {
            qDebug() << "The format to be used for audio output is" << _outputFormat << "\n";
            
            _inputDevice = _audioInput->start();
            connect(_inputDevice, SIGNAL(readyRead()), SLOT(handleAudioInput()));
            
            _audioOutput = new QAudioOutput(outputDeviceInfo, _outputFormat, this);
            _numOutputCallbackBytes = BUFFER_LENGTH_BYTES_PER_CHANNEL * _outputFormat.channelCount()
                * (_outputFormat.sampleRate() / SAMPLE_RATE)
                / CALLBACK_ACCELERATOR_RATIO;
            _audioOutput->setBufferSize(_numOutputCallbackBytes);
            _outputDevice = _audioOutput->start();
            
            gettimeofday(&_lastReceiveTime, NULL);
        }
        
        return;
    }
    
    qDebug() << "Unable to set up audio I/O because of a problem with input or output formats.\n";
}

void Audio::handleAudioInput() {
    
    static char monoAudioDataPacket[MAX_PACKET_SIZE];
    
    static int numBytesPacketHeader = numBytesForPacketHeader((unsigned char*) &PACKET_TYPE_MICROPHONE_AUDIO_NO_ECHO);
    static int leadingBytes = numBytesPacketHeader + sizeof(glm::vec3) + sizeof(glm::quat) +  NUM_BYTES_RFC4122_UUID;
    static int16_t* monoAudioSamples = (int16_t*) (monoAudioDataPacket + leadingBytes);
    
    QByteArray inputByteArray = _inputDevice->readAll();
    
    int numResampledBytes =  inputByteArray.size() / (_numInputCallbackBytes / BUFFER_LENGTH_BYTES_PER_CHANNEL);
    qDebug() << "NSB:" << numResampledBytes << "\n";
    
    // zero out the monoAudioSamples array
    memset(monoAudioSamples, 0, numResampledBytes);
    
    if (Menu::getInstance()->isOptionChecked(MenuOption::EchoLocalAudio) && !_muted) {
        _outputBuffer.resize(inputByteArray.size());
        //  if local loopback enabled, copy input to output
        nearestNeighborResampling((int16_t*) inputByteArray.data(), (int16_t*) _outputBuffer.data(),
                                  inputByteArray.size() / sizeof(int16_t),
                                  _inputFormat, _outputFormat);
    } else {
        _outputBuffer.fill(0, inputByteArray.size());
    }
    
    // add input data just written to the scope
    //    QMetaObject::invokeMethod(_scope, "addStereoSamples", Qt::QueuedConnection,
    //                              Q_ARG(QByteArray, inputByteArray), Q_ARG(bool, true));
    
    // add procedural effects to the appropriate input samples
    //    addProceduralSounds(monoAudioSamples + (_isBufferSendCallback
    //                                            ? BUFFER_LENGTH_SAMPLES_PER_CHANNEL / CALLBACK_ACCELERATOR_RATIO : 0),
    //                        (int16_t*) stereoOutputBuffer.data(),
    //                        BUFFER_LENGTH_SAMPLES_PER_CHANNEL / CALLBACK_ACCELERATOR_RATIO);
    
    NodeList* nodeList = NodeList::getInstance();
    Node* audioMixer = nodeList->soloNodeOfType(NODE_TYPE_AUDIO_MIXER);
    
    if (audioMixer) {
        if (audioMixer->getActiveSocket()) {
            MyAvatar* interfaceAvatar = Application::getInstance()->getAvatar();
            
            glm::vec3 headPosition = interfaceAvatar->getHeadJointPosition();
            glm::quat headOrientation = interfaceAvatar->getHead().getOrientation();
            
            // we need the amount of bytes in the buffer + 1 for type
            // + 12 for 3 floats for position + float for bearing + 1 attenuation byte
            
            PACKET_TYPE packetType = Menu::getInstance()->isOptionChecked(MenuOption::EchoServerAudio)
            ? PACKET_TYPE_MICROPHONE_AUDIO_WITH_ECHO : PACKET_TYPE_MICROPHONE_AUDIO_NO_ECHO;
            
            char* currentPacketPtr = monoAudioDataPacket + populateTypeAndVersion((unsigned char*) monoAudioDataPacket,
                                                                                  packetType);
            
            // pack Source Data
            QByteArray rfcUUID = NodeList::getInstance()->getOwnerUUID().toRfc4122();
            memcpy(currentPacketPtr, rfcUUID.constData(), rfcUUID.size());
            currentPacketPtr += rfcUUID.size();
            
            // memcpy the three float positions
            memcpy(currentPacketPtr, &headPosition, sizeof(headPosition));
            currentPacketPtr += (sizeof(headPosition));
            
            // memcpy our orientation
            memcpy(currentPacketPtr, &headOrientation, sizeof(headOrientation));
            currentPacketPtr += sizeof(headOrientation);
            
            if (!_muted) {
                float loudness = 0;
                //                    loudness /= BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
                _lastInputLoudness = loudness;
                
                // we aren't muted - pull our input audio to send off to the mixer
                nearestNeighborResampling((int16_t*) inputByteArray.data(),
                                          monoAudioSamples, numResampledBytes / sizeof(int16_t),
                                          _inputFormat, _desiredInputFormat);
                
            } else {
                _lastInputLoudness = 0;
            }
            
            nodeList->getNodeSocket().writeDatagram(monoAudioDataPacket,
                                                    numResampledBytes + leadingBytes,
                                                    audioMixer->getActiveSocket()->getAddress(),
                                                    audioMixer->getActiveSocket()->getPort());
            
            Application::getInstance()->getBandwidthMeter()->outputStream(BandwidthMeter::AUDIO)
            .updateValue(BUFFER_LENGTH_BYTES_PER_CHANNEL + leadingBytes);
        } else {
            nodeList->pingPublicAndLocalSocketsForInactiveNode(audioMixer);
        }
    }
    
    if (_outputDevice) {
        // if there is anything in the ring buffer, decide what to do
        
        if (_ringBuffer.getEndOfLastWrite()) {
            if (_ringBuffer.isStarved() && _ringBuffer.diffLastWriteNextOutput() <
                ((_outputBuffer.size() / sizeof(int16_t)) + _jitterBufferSamples * (_ringBuffer.isStereo() ? 2 : 1))) {
                //  If not enough audio has arrived to start playback, keep waiting
            } else if (!_ringBuffer.isStarved() && _ringBuffer.diffLastWriteNextOutput() == 0) {
                //  If we have started and now have run out of audio to send to the audio device,
                //  this means we've starved and should restart.
                _ringBuffer.setIsStarved(true);
                
                // show a starve in the GUI for 10 frames
                _numFramesDisplayStarve = 10;
                
            } else {
                //  We are either already playing back, or we have enough audio to start playing back.
                if (_ringBuffer.isStarved()) {
                    _ringBuffer.setIsStarved(false);
                    _ringBuffer.setHasStarted(true);
                }
                
                int numOutputBufferSamples = _outputBuffer.size() / sizeof(int16_t);
                
                // copy the packet from the RB to the output
                nearestNeighborResampling(_ringBuffer.getNextOutput(),
                                          (int16_t*) _outputBuffer.data(),
                                          numOutputBufferSamples,
                                          _desiredOutputFormat, _outputFormat);
                
                _ringBuffer.setNextOutput(_ringBuffer.getNextOutput() + numOutputBufferSamples);
                
                if (_ringBuffer.getNextOutput() >= _ringBuffer.getBuffer() + RING_BUFFER_LENGTH_SAMPLES) {
                    _ringBuffer.setNextOutput(_ringBuffer.getBuffer());
                }
            }
        }
        
        // add output (@speakers) data just written to the scope
        //    QMetaObject::invokeMethod(_scope, "addStereoSamples", Qt::QueuedConnection,
        //                              Q_ARG(QByteArray, stereoOutputBuffer), Q_ARG(bool, false));
        
        _outputDevice->write(_outputBuffer);
    }
    
    gettimeofday(&_lastCallbackTime, NULL);
}

void Audio::addReceivedAudioToBuffer(const QByteArray& audioByteArray) {
    const int NUM_INITIAL_PACKETS_DISCARD = 3;
    const int STANDARD_DEVIATION_SAMPLE_COUNT = 500;
    
    timeval currentReceiveTime;
    gettimeofday(&currentReceiveTime, NULL);
    _totalPacketsReceived++;
    
    double timeDiff = diffclock(&_lastReceiveTime, &currentReceiveTime);
    
    //  Discard first few received packets for computing jitter (often they pile up on start)
    if (_totalPacketsReceived > NUM_INITIAL_PACKETS_DISCARD) {
        _stdev.addValue(timeDiff);
    }
    
    if (_stdev.getSamples() > STANDARD_DEVIATION_SAMPLE_COUNT) {
        _measuredJitter = _stdev.getStDev();
        _stdev.reset();
        //  Set jitter buffer to be a multiple of the measured standard deviation
        const int MAX_JITTER_BUFFER_SAMPLES = RING_BUFFER_LENGTH_SAMPLES / 2;
        const float NUM_STANDARD_DEVIATIONS = 3.f;
        if (Menu::getInstance()->getAudioJitterBufferSamples() == 0) {
            float newJitterBufferSamples = (NUM_STANDARD_DEVIATIONS * _measuredJitter) / 1000.f * SAMPLE_RATE;
            setJitterBufferSamples(glm::clamp((int)newJitterBufferSamples, 0, MAX_JITTER_BUFFER_SAMPLES));
        }
    }
    
    if (_ringBuffer.diffLastWriteNextOutput() + PACKET_LENGTH_SAMPLES >
        PACKET_LENGTH_SAMPLES + (ceilf((float) (_jitterBufferSamples * 2) / PACKET_LENGTH_SAMPLES) * PACKET_LENGTH_SAMPLES)) {
        // this packet would give us more than the required amount for play out
        // discard the first packet in the buffer
        
        _ringBuffer.setNextOutput(_ringBuffer.getNextOutput() + PACKET_LENGTH_SAMPLES);
        
        if (_ringBuffer.getNextOutput() == _ringBuffer.getBuffer() + RING_BUFFER_LENGTH_SAMPLES) {
            _ringBuffer.setNextOutput(_ringBuffer.getBuffer());
        }
    }
    
    _ringBuffer.parseData((unsigned char*) audioByteArray.data(), audioByteArray.size());
    
    Application::getInstance()->getBandwidthMeter()->inputStream(BandwidthMeter::AUDIO).updateValue(PACKET_LENGTH_BYTES
                                                                                                    + sizeof(PACKET_TYPE));
    
    _lastReceiveTime = currentReceiveTime;
}

bool Audio::mousePressEvent(int x, int y) {
    if (_iconBounds.contains(x, y)) {
        _muted = !_muted;
        return true;
    }
    return false;
}

void Audio::render(int screenWidth, int screenHeight) {
    if (_audioInput) {
        glLineWidth(2.0);
        glBegin(GL_LINES);
        glColor3f(1,1,1);
        
        int startX = 20.0;
        int currentX = startX;
        int topY = screenHeight - 40;
        int bottomY = screenHeight - 20;
        float frameWidth = 20.0;
        float halfY = topY + ((bottomY - topY) / 2.0);
        
        // draw the lines for the base of the ring buffer
        
        glVertex2f(currentX, topY);
        glVertex2f(currentX, bottomY);
        
        for (int i = 0; i < RING_BUFFER_LENGTH_FRAMES / 2; i++) {
            glVertex2f(currentX, halfY);
            glVertex2f(currentX + frameWidth, halfY);
            currentX += frameWidth;
            
            glVertex2f(currentX, topY);
            glVertex2f(currentX, bottomY);
        }
        glEnd();
        
        //  Show a bar with the amount of audio remaining in ring buffer beyond current playback
        float remainingBuffer = 0;
        timeval currentTime;
        gettimeofday(&currentTime, NULL);
        float timeLeftInCurrentBuffer = 0;
        if (_lastCallbackTime.tv_usec > 0) {
            timeLeftInCurrentBuffer = AUDIO_CALLBACK_MSECS - diffclock(&_lastCallbackTime, &currentTime);
        }
        
        if (_ringBuffer.getEndOfLastWrite() != NULL)
            remainingBuffer = _ringBuffer.diffLastWriteNextOutput() / PACKET_LENGTH_SAMPLES * AUDIO_CALLBACK_MSECS;
        
        if (_numFramesDisplayStarve == 0) {
            glColor3f(0, 1, 0);
        } else {
            glColor3f(0.5 + (_numFramesDisplayStarve / 20.0f), 0, 0);
            _numFramesDisplayStarve--;
        }
        
        glBegin(GL_QUADS);
        glVertex2f(startX, topY + 2);
        glVertex2f(startX + (remainingBuffer + timeLeftInCurrentBuffer) / AUDIO_CALLBACK_MSECS * frameWidth, topY + 2);
        glVertex2f(startX + (remainingBuffer + timeLeftInCurrentBuffer) / AUDIO_CALLBACK_MSECS * frameWidth, bottomY - 2);
        glVertex2f(startX, bottomY - 2);
        glEnd();
        
        if (_averagedLatency == 0.0) {
            _averagedLatency = remainingBuffer + timeLeftInCurrentBuffer;
        } else {
            _averagedLatency = 0.99f * _averagedLatency + 0.01f * (remainingBuffer + timeLeftInCurrentBuffer);
        }
        
        //  Show a yellow bar with the averaged msecs latency you are hearing (from time of packet receipt)
        glColor3f(1,1,0);
        glBegin(GL_QUADS);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth - 2, topY - 2);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth + 2, topY - 2);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth + 2, bottomY + 2);
        glVertex2f(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth - 2, bottomY + 2);
        glEnd();
        
        char out[40];
        sprintf(out, "%3.0f\n", _averagedLatency);
        drawtext(startX + _averagedLatency / AUDIO_CALLBACK_MSECS * frameWidth - 10, topY - 9, 0.10, 0, 1, 0, out, 1,1,0);
        
        //  Show a red bar with the 'start' point of one frame plus the jitter buffer
        
        glColor3f(1, 0, 0);
        int jitterBufferPels = (1.f + (float)getJitterBufferSamples() / (float) PACKET_LENGTH_SAMPLES_PER_CHANNEL) * frameWidth;
        sprintf(out, "%.0f\n", getJitterBufferSamples() / SAMPLE_RATE * 1000.f);
        drawtext(startX + jitterBufferPels - 5, topY - 9, 0.10, 0, 1, 0, out, 1, 0, 0);
        sprintf(out, "j %.1f\n", _measuredJitter);
        if (Menu::getInstance()->getAudioJitterBufferSamples() == 0) {
            drawtext(startX + jitterBufferPels - 5, bottomY + 12, 0.10, 0, 1, 0, out, 1, 0, 0);
        } else {
            drawtext(startX, bottomY + 12, 0.10, 0, 1, 0, out, 1, 0, 0);
        }
        
        glBegin(GL_QUADS);
        glVertex2f(startX + jitterBufferPels - 2, topY - 2);
        glVertex2f(startX + jitterBufferPels + 2, topY - 2);
        glVertex2f(startX + jitterBufferPels + 2, bottomY + 2);
        glVertex2f(startX + jitterBufferPels - 2, bottomY + 2);
        glEnd();
        
    }
    renderToolIcon(screenHeight);
}

//  Take a pointer to the acquired microphone input samples and add procedural sounds
void Audio::addProceduralSounds(int16_t* monoInput, int16_t* stereoUpsampledOutput, int numSamples) {
    const float MAX_AUDIBLE_VELOCITY = 6.0;
    const float MIN_AUDIBLE_VELOCITY = 0.1;
    const int VOLUME_BASELINE = 400;
    const float SOUND_PITCH = 8.f;
    
    float speed = glm::length(_lastVelocity);
    float volume = VOLUME_BASELINE * (1.f - speed / MAX_AUDIBLE_VELOCITY);
    
    float sample;
    
    // Travelling noise
    //  Add a noise-modulated sinewave with volume that tapers off with speed increasing
    if ((speed > MIN_AUDIBLE_VELOCITY) && (speed < MAX_AUDIBLE_VELOCITY)) {
        for (int i = 0; i < numSamples; i++) {
            monoInput[i] += (int16_t)(sinf((float) (_proceduralEffectSample + i) / SOUND_PITCH )
                                      * volume * (1.f + randFloat() * 0.25f) * speed);
        }
    }
    const float COLLISION_SOUND_CUTOFF_LEVEL = 0.01f;
    const float COLLISION_SOUND_MAX_VOLUME = 1000.f;
    const float UP_MAJOR_FIFTH = powf(1.5f, 4.0f);
    const float DOWN_TWO_OCTAVES = 4.f;
    const float DOWN_FOUR_OCTAVES = 16.f;
    float t;
    if (_collisionSoundMagnitude > COLLISION_SOUND_CUTOFF_LEVEL) {
        for (int i = 0; i < numSamples; i++) {
            t = (float) _proceduralEffectSample + (float) i;
            
            sample = sinf(t * _collisionSoundFrequency)
                + sinf(t * _collisionSoundFrequency / DOWN_TWO_OCTAVES)
                + sinf(t * _collisionSoundFrequency / DOWN_FOUR_OCTAVES * UP_MAJOR_FIFTH);
            sample *= _collisionSoundMagnitude * COLLISION_SOUND_MAX_VOLUME;
            
            int16_t collisionSample = (int16_t) sample;
            
            monoInput[i] += collisionSample;
            
            for (int j = (i * 4); j < (i * 4) + 4; j++) {
                stereoUpsampledOutput[j] += collisionSample;
            }
            
            _collisionSoundMagnitude *= _collisionSoundDuration;
        }
    }
    _proceduralEffectSample += numSamples;
    
    //  Add a drum sound
    const float MAX_VOLUME = 32000.f;
    const float MAX_DURATION = 2.f;
    const float MIN_AUDIBLE_VOLUME = 0.001f;
    const float NOISE_MAGNITUDE = 0.02f;
    float frequency = (_drumSoundFrequency / SAMPLE_RATE) * PI_TIMES_TWO;
    if (_drumSoundVolume > 0.f) {
        for (int i = 0; i < numSamples; i++) {
            t = (float) _drumSoundSample + (float) i;
            sample = sinf(t * frequency);
            sample += ((randFloat() - 0.5f) * NOISE_MAGNITUDE);
            sample *= _drumSoundVolume * MAX_VOLUME;
            
            int16_t collisionSample = (int16_t) sample;
            
            monoInput[i] += collisionSample;
            
            for (int j = (i * 4); j < (i * 4) + 4; j++) {
                stereoUpsampledOutput[j] += collisionSample;
            }
            
            _drumSoundVolume *= (1.f - _drumSoundDecay);
        }
        _drumSoundSample += numSamples;
        _drumSoundDuration = glm::clamp(_drumSoundDuration - (AUDIO_CALLBACK_MSECS / 1000.f), 0.f, MAX_DURATION);
        if (_drumSoundDuration == 0.f || (_drumSoundVolume < MIN_AUDIBLE_VOLUME)) {
            _drumSoundVolume = 0.f;
        }
    }
}

//  Starts a collision sound.  magnitude is 0-1, with 1 the loudest possible sound.
void Audio::startCollisionSound(float magnitude, float frequency, float noise, float duration, bool flashScreen) {
    _collisionSoundMagnitude = magnitude;
    _collisionSoundFrequency = frequency;
    _collisionSoundNoise = noise;
    _collisionSoundDuration = duration;
    _collisionFlashesScreen = flashScreen;
}

void Audio::startDrumSound(float volume, float frequency, float duration, float decay) {
    _drumSoundVolume = volume;
    _drumSoundFrequency = frequency;
    _drumSoundDuration = duration;
    _drumSoundDecay = decay;
    _drumSoundSample = 0;
}

void Audio::renderToolIcon(int screenHeight) {
    
    _iconBounds = QRect(ICON_LEFT, screenHeight - BOTTOM_PADDING, ICON_SIZE, ICON_SIZE);
    glEnable(GL_TEXTURE_2D);
    
    glBindTexture(GL_TEXTURE_2D, _micTextureId);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    
    glTexCoord2f(1, 1);
    glVertex2f(_iconBounds.left(), _iconBounds.top());
    
    glTexCoord2f(0, 1);
    glVertex2f(_iconBounds.right(), _iconBounds.top());
    
    glTexCoord2f(0, 0);
    glVertex2f(_iconBounds.right(), _iconBounds.bottom());
    
    glTexCoord2f(1, 0);
    glVertex2f(_iconBounds.left(), _iconBounds.bottom());
    
    glEnd();
    
    if (_muted) {
        glBindTexture(GL_TEXTURE_2D, _muteTextureId);
        glBegin(GL_QUADS);
        
        glTexCoord2f(1, 1);
        glVertex2f(_iconBounds.left(), _iconBounds.top());
        
        glTexCoord2f(0, 1);
        glVertex2f(_iconBounds.right(), _iconBounds.top());
        
        glTexCoord2f(0, 0);
        glVertex2f(_iconBounds.right(), _iconBounds.bottom());
        
        glTexCoord2f(1, 0);
        glVertex2f(_iconBounds.left(), _iconBounds.bottom());
        
        glEnd();
    }
    
    glDisable(GL_TEXTURE_2D);
}