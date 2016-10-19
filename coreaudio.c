/*
Copyright (c) 2011 Marc Vaillant

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <pthread.h>


#define kOutputBus 0
#define kInputBus 1
#define SAMPLE_RATE 44100

static int* _pcm;
static int _pcmSize;
static int _index;
static AudioComponentInstance* _audioUnit = 0;
static pthread_mutex_t _playingMutex;
static pthread_cond_t _playingCond;


void write_audio(void* dummy, int* pcm, int size)
{

	_pcm = pcm;
	_pcmSize = size / sizeof(int);
	_index = 0;
	pthread_mutex_lock(&_playingMutex);
  AudioOutputUnitStart(*_audioUnit);

	// block until finished playing
	pthread_cond_wait(&_playingCond, &_playingMutex);
	pthread_mutex_unlock(&_playingMutex);

}

static OSStatus playbackCallback(void *inRefCon, 
                                  AudioUnitRenderActionFlags *ioActionFlags, 
                                  const AudioTimeStamp *inTimeStamp, 
                                  UInt32 inBusNumber, 
                                  UInt32 inNumberFrames, 
                                  AudioBufferList *ioData) 
{    
	//    cout<<"index = "<<_index<<endl;
	//	cout<<"numBuffers = "<<ioData->mNumberBuffers<<endl;

	//int totalNumberOfSamples = _pcm.size();
	int totalNumberOfSamples = _pcmSize;
	for(UInt32 i = 0; i < ioData->mNumberBuffers; ++i)
	{
		//      cout<<"i = "<<i<<endl;
		int samplesLeft = totalNumberOfSamples - _index;
		int numSamples = ioData->mBuffers[i].mDataByteSize / 4;
		//	  cout<<"numSamples = "<<numSamples<<endl;
		if(samplesLeft > 0)
		{
			if(samplesLeft < numSamples)
			{
				memcpy(ioData->mBuffers[i].mData, &_pcm[_index], samplesLeft * 4);
				_index += samplesLeft;
				memset(((char*) (ioData->mBuffers[i].mData)) + samplesLeft * 4, 0, (numSamples - samplesLeft) * 4) ;
			}
			else
			{
				memcpy(ioData->mBuffers[i].mData, &_pcm[_index], numSamples * 4) ;
				_index += numSamples;
			}

			//		ofstream fp("buffer.txt", ios::app);
			//		for(UInt32 i = 0; i < ioData->mBuffers[0].mDataByteSize / 4; ++i)
			//		{
			//		  fp<<*((short*) ioData->mBuffers[0].mData + 2 * i)<<endl;
			//		}
			//		fp.close();


		}
		else
		{
			memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);

			// signal that pcm is finished playing
			pthread_mutex_lock(&_playingMutex);
			pthread_cond_signal(&_playingCond);
			pthread_mutex_unlock(&_playingMutex);

			// stop the audio unit
			AudioOutputUnitStop(*_audioUnit);
		}
	}


	return noErr;
}

void close_audio(void* cookie)
{
	AudioOutputUnitStop(*_audioUnit);
}

void close_dsp(void* s)
{
	AudioUnitUninitialize(*_audioUnit);
	AudioComponentInstanceDispose(*_audioUnit);
	free(_audioUnit);
	_audioUnit = 0;
	pthread_mutex_destroy(&_playingMutex);
  pthread_cond_destroy (&_playingCond);
}

void* open_dsp(char* dummy)
{

	// skip if already created audio unit
	
	if(_audioUnit)
		return 0;

	pthread_mutex_init(&_playingMutex, NULL);
  pthread_cond_init (&_playingCond, NULL);
	
	_audioUnit = (AudioComponentInstance*) malloc(sizeof(AudioComponentInstance));
  OSStatus status;
  //AudioComponentInstance audioUnit;

  // Describe audio component
  AudioComponentDescription desc;
  desc.componentType = kAudioUnitType_Output;
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
  desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
  desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
  desc.componentFlags = 0;
  desc.componentFlagsMask = 0;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  // Get component
  AudioComponent inputComponent = AudioComponentFindNext(NULL, &desc);

  // Get audio units
  status = AudioComponentInstanceNew(inputComponent, _audioUnit);
  //checkStatus(status);

  UInt32 flag = 1;
  // Enable IO for playback
  status = AudioUnitSetProperty(*_audioUnit, 
				  kAudioOutputUnitProperty_EnableIO, 
				  kAudioUnitScope_Output, 
				  kOutputBus,
				  &flag, 
				  sizeof(flag));
  //checkStatus(status);

  // Describe format

  AudioStreamBasicDescription audioFormat;
  audioFormat.mSampleRate = SAMPLE_RATE;
  audioFormat.mFormatID	= kAudioFormatLinearPCM;
  audioFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  audioFormat.mFramesPerPacket = 1;
  audioFormat.mChannelsPerFrame = 2;
  audioFormat.mBitsPerChannel = 16;
  audioFormat.mBytesPerPacket = 4;
  audioFormat.mBytesPerFrame = 4;

  // Apply format

  status = AudioUnitSetProperty(*_audioUnit, 
				  kAudioUnitProperty_StreamFormat, 
				  kAudioUnitScope_Input, 
				  kOutputBus, 
				  &audioFormat, 
				  sizeof(audioFormat));
//  checkStatus(status);

  // Set output callback
  AURenderCallbackStruct callbackStruct;
  callbackStruct.inputProc = playbackCallback;
  callbackStruct.inputProcRefCon = NULL;
  status = AudioUnitSetProperty(*_audioUnit, 
				  kAudioUnitProperty_SetRenderCallback, 
				  kAudioUnitScope_Global, 
				  kOutputBus,
				  &callbackStruct, 
				  sizeof(callbackStruct));

  // Initialize
  status = AudioUnitInitialize(*_audioUnit);

	return 0;

}

//int main()
//{    
//
//  //generate pcm tone  freq = 800, duration = 5s, rise/fall time = 5ms
//
//  generateTone(_pcm, 800, 500, SAMPLE_RATE, 5, 0.5);
//  _index = 0;
//
//  OSStatus status;
//  AudioComponentInstance audioUnit;
//
//  // Describe audio component
//  AudioComponentDescription desc;
//  desc.componentType = kAudioUnitType_Output;
//  desc.componentSubType = kAudioUnitSubType_DefaultOutput;
//  desc.componentFlags = 0;
//  desc.componentFlagsMask = 0;
//  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
//
//  // Get component
//  AudioComponent inputComponent = AudioComponentFindNext(NULL, &desc);
//
//  // Get audio units
//  status = AudioComponentInstanceNew(inputComponent, &audioUnit);
//  //checkStatus(status);
//
//  UInt32 flag = 1;
//  // Enable IO for playback
//  status = AudioUnitSetProperty(audioUnit, 
//				  kAudioOutputUnitProperty_EnableIO, 
//				  kAudioUnitScope_Output, 
//				  kOutputBus,
//				  &flag, 
//				  sizeof(flag));
//  //checkStatus(status);
//
//  // Describe format
//
//  AudioStreamBasicDescription audioFormat;
//  audioFormat.mSampleRate = SAMPLE_RATE;
//  audioFormat.mFormatID	= kAudioFormatLinearPCM;
//  audioFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
//  audioFormat.mFramesPerPacket = 1;
//  audioFormat.mChannelsPerFrame = 2;
//  audioFormat.mBitsPerChannel = 16;
//  audioFormat.mBytesPerPacket = 4;
//  audioFormat.mBytesPerFrame = 4;
//
//  // Apply format
//
//  status = AudioUnitSetProperty(audioUnit, 
//				  kAudioUnitProperty_StreamFormat, 
//				  kAudioUnitScope_Input, 
//				  kOutputBus, 
//				  &audioFormat, 
//				  sizeof(audioFormat));
////  checkStatus(status);
//
//  // Set output callback
//  AURenderCallbackStruct callbackStruct;
//  callbackStruct.inputProc = playbackCallback;
//  callbackStruct.inputProcRefCon = NULL;
//  status = AudioUnitSetProperty(audioUnit, 
//				  kAudioUnitProperty_SetRenderCallback, 
//				  kAudioUnitScope_Global, 
//				  kOutputBus,
//				  &callbackStruct, 
//				  sizeof(callbackStruct));
//
//  // Initialize
//  status = AudioUnitInitialize(audioUnit);
//  status = AudioOutputUnitStart(audioUnit);
//
//  // sleep
//  
//  sleep(2.0);
//  return 1;
//}

