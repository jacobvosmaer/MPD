/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "OSXOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "util/DynamicFifoBuffer.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "system/ByteOrder.hxx"
#include "Log.hxx"

#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreServices/CoreServices.h>

struct OSXOutput {
	AudioOutput base;

	/* configuration settings */
	OSType component_subtype;
	/* only applicable with kAudioUnitSubType_HALOutput */
	const char *device_name;

	uint output_left;
	uint output_right;
	bool set_channel_map;

	AudioComponentInstance au;
	AudioStreamBasicDescription *asbd;

	Mutex mutex;
	Cond condition;
	DynamicFifoBuffer<uint8_t> *buffer;

	OSXOutput()
		:base(osx_output_plugin) {}
};

static constexpr Domain osx_output_domain("osx_output");

static void
osx_os_status_to_cstring(OSStatus status, char *str, size_t size) {
	CFErrorRef cferr = CFErrorCreate(nullptr, kCFErrorDomainOSStatus, status, nullptr);
	CFStringRef cfstr = CFErrorCopyDescription(cferr);
	if (!CFStringGetCString(cfstr, str, size, kCFStringEncodingUTF8)) {
		/* conversion failed, return empty string */
		*str = '\0';
	}
	if (cferr)
		CFRelease(cferr);
	if (cfstr)
		CFRelease(cfstr);
}

static bool
osx_output_test_default_device(void)
{
	/* on a Mac, this is always the default plugin, if nothing
	   else is configured */
	return true;
}

static void
osx_output_configure(OSXOutput *oo, const ConfigBlock &block)
{
	const char *device = block.GetBlockValue("device");

	if (device == nullptr || 0 == strcmp(device, "default")) {
		oo->component_subtype = kAudioUnitSubType_DefaultOutput;
		oo->device_name = nullptr;
	}
	else if (0 == strcmp(device, "system")) {
		oo->component_subtype = kAudioUnitSubType_SystemOutput;
		oo->device_name = nullptr;
	}
	else {
		oo->component_subtype = kAudioUnitSubType_HALOutput;
		/* XXX am I supposed to strdup() this? */
		oo->device_name = device;
	}

	const BlockParam *output_left_bp = block.GetBlockParam("output_left");
	const BlockParam *output_right_bp = block.GetBlockParam("output_right");
	if (output_left_bp && output_right_bp) {
		oo->output_left = output_left_bp->GetUnsignedValue();
		oo->output_right = output_right_bp->GetUnsignedValue();
	} else {
		oo->output_left = 0;
		oo->output_right = 1;
	}
}

static AudioOutput *
osx_output_init(const ConfigBlock &block, Error &error)
{
	OSXOutput *oo = new OSXOutput();
	if (!oo->base.Configure(block, error)) {
		delete oo;
		return nullptr;
	}

	osx_output_configure(oo, block);

	return &oo->base;
}

static void
osx_output_finish(AudioOutput *ao)
{
	OSXOutput *oo = (OSXOutput *)ao;

	delete oo;
}

static bool
osx_output_set_device(OSXOutput *oo, Error &error)
{
	bool ret = true;
	OSStatus status;
	UInt32 size, numdevices, numchannels;
	AudioDeviceID *deviceids = nullptr;
	SInt32 *channelmap = nullptr;
	AudioObjectPropertyAddress propaddr;
	AudioStreamBasicDescription desc;
	CFStringRef cfname = nullptr;
	char errormsg[1024];
	char name[256];
	unsigned int i;

	if (oo->component_subtype != kAudioUnitSubType_HALOutput)
		goto done;

	/* how many audio devices are there? */
	propaddr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
	status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propaddr, 0, nullptr, &size);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to determine number of OS X audio devices: %s",
			     errormsg);
		ret = false;
		goto done;
	}

	/* what are the available audio device IDs? */
	numdevices = size / sizeof(AudioDeviceID);
	deviceids = new AudioDeviceID[numdevices];
	status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propaddr, 0, nullptr, &size, deviceids);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to determine OS X audio device IDs: %s",
			     errormsg);
		ret = false;
		goto done;
	}

	/* which audio device matches oo->device_name? */
	propaddr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
	size = sizeof(CFStringRef);
	for (i = 0; i < numdevices; i++) {
		status = AudioObjectGetPropertyData(deviceids[i], &propaddr, 0, nullptr, &size, &cfname);
		if (status != noErr) {
			osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
			error.Format(osx_output_domain, status,
				     "Unable to determine OS X device name "
				     "(device %u): %s",
				     (unsigned int) deviceids[i],
				     errormsg);
			ret = false;
			goto done;
		}

		if (!CFStringGetCString(cfname, name, sizeof(name), kCFStringEncodingUTF8)) {
			error.Set(osx_output_domain, "Unable to convert device name from CFStringRef to char*");
			ret = false;
			goto done;
		}

		if (strcmp(oo->device_name, name) == 0) {
			FormatDebug(osx_output_domain,
				    "found matching device: ID=%u, name=%s",
				    (unsigned)deviceids[i], name);
			break;
		}
	}
	if (i == numdevices) {
		FormatWarning(osx_output_domain,
			      "Found no audio device with name '%s' "
			      "(will use default audio device)",
			      oo->device_name);
		goto done;
	}

	status = AudioUnitSetProperty(oo->au,
				      kAudioOutputUnitProperty_CurrentDevice,
				      kAudioUnitScope_Global,
				      0,
				      &(deviceids[i]),
				      sizeof(AudioDeviceID));
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to set OS X audio output device: %s",
			     errormsg);
		ret = false;
		goto done;
	}

	FormatDebug(osx_output_domain,
		    "set OS X audio output device ID=%u, name=%s",
		    (unsigned)deviceids[i], name);

	size = sizeof (AudioStreamBasicDescription);
	status = AudioUnitGetProperty(oo->au,
				kAudioUnitProperty_StreamFormat,
				kAudioUnitScope_Output,
				0, 
				&desc,
				&size);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to get number of OS X audio output device channels: %s",
			     errormsg);
		ret = false;
		goto done;
	}

	numchannels = desc.mChannelsPerFrame;

	if (oo->output_left >= numchannels || oo->output_right >= numchannels) {
		error.Format(osx_output_domain,
			     "Invalid OS X audio output channel mapping (%u, %u): only %d channels available",
			     oo->output_left, oo->output_right, numchannels);
		ret = false;
		goto done;
	}

	channelmap = new SInt32[numchannels];
	size = numchannels * sizeof(SInt32);
	for (unsigned int j = 0; j < numchannels; ++j) {
		channelmap[j] = -1;
	}
	channelmap[oo->output_left] = 0;
	channelmap[oo->output_right] = 1;

	for (unsigned int j = 0; j < numchannels; ++j) {
		FormatDebug(osx_output_domain, "channelmap[%u] = %d", j, channelmap[j]);
	}

	status = AudioUnitSetProperty(oo->au, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 0, channelmap, size);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to set OS X audio output channel map: %s",
			     errormsg);
		ret = false;
		goto done;
	}

done:
	delete[] deviceids;
	delete[] channelmap;
	if (cfname)
		CFRelease(cfname);
	return ret;
}

static OSStatus
osx_render(void *vdata,
	   gcc_unused AudioUnitRenderActionFlags *io_action_flags,
	   gcc_unused const AudioTimeStamp *in_timestamp,
	   gcc_unused UInt32 in_bus_number,
	   UInt32 in_number_frames,
	   AudioBufferList *buffer_list)
{
	OSXOutput *od = (OSXOutput *) vdata;
	AudioStreamBasicDescription *asbd = od->asbd;
	size_t sample_size = asbd->mBytesPerFrame / asbd->mChannelsPerFrame;
	unsigned int i;
	uint8_t dest;

	assert(od->buffer != nullptr);
	assert(in_bus_number == 0);
	assert(buffer_list->mNumberBuffers == asbd->mChannelsPerFrame);
	for (i = 0 ; i < buffer_list->mNumberBuffers; ++i) {
		assert(buffer_list->mBuffers[i].mData != nullptr);
		assert(buffer_list->mBuffers[i].mDataByteSize == in_number_frames * sample_size);
	}

	// Acquire mutex when accessing od->buffer (the ring buffer)
	od->mutex.lock();

	auto src = od->buffer->Read();

	UInt32 available_frames = src.size / asbd->mBytesPerFrame;
	if (available_frames > in_number_frames)
		available_frames = in_number_frames;

	for (UInt32 current_frame = 0; current_frame < available_frames; ++current_frame) {
		// De-interleave audio
		for (i = 0 ; i < buffer_list->mNumberBuffers; ++i) {
			dest = (size_t) buffer_list->mBuffers[i].mData;
			memcpy(
				(void *) (dest + current_frame * sample_size),
				src.data + current_frame * asbd->mBytesPerFrame + i * sample_size,
				sample_size
			);
		}
	}

	od->buffer->Consume(available_frames * asbd->mBytesPerFrame);

	od->condition.signal();
	od->mutex.unlock();

	if (available_frames < in_number_frames) {
		// Play silence during a buffer underrun
		for (i = 0 ; i < buffer_list->mNumberBuffers; ++i) {
			dest = (size_t) buffer_list->mBuffers[i].mData;
			memset(
				(void *) (dest + available_frames * sample_size),
				0,
				(in_number_frames - available_frames) * sample_size
			);
		}
	}

	return 0;
}

static bool
osx_output_enable(AudioOutput *ao, Error &error)
{
	char errormsg[1024];
	OSXOutput *oo = (OSXOutput *)ao;

	AudioComponentDescription desc;
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = oo->component_subtype;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (comp == 0) {
		error.Set(osx_output_domain,
			  "Error finding OS X component");
		return false;
	}

	OSStatus status = AudioComponentInstanceNew(comp, &oo->au);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to open OS X component: %s",
			     errormsg);
		return false;
	}

	if (!osx_output_set_device(oo, error)) {
		AudioComponentInstanceDispose(oo->au);
		return false;
	}

	AURenderCallbackStruct callback;
	callback.inputProc = osx_render;
	callback.inputProcRefCon = oo;

	status =
		AudioUnitSetProperty(oo->au,
				     kAudioUnitProperty_SetRenderCallback,
				     kAudioUnitScope_Input, 0,
				     &callback, sizeof(callback));
	if (status != noErr) {
		AudioComponentInstanceDispose(oo->au);
		error.Set(osx_output_domain, status,
			  "unable to set callback for OS X audio unit");
		return false;
	}

	return true;
}

static void
osx_output_disable(AudioOutput *ao)
{
	OSXOutput *oo = (OSXOutput *)ao;

	AudioComponentInstanceDispose(oo->au);
}

static void
osx_output_cancel(AudioOutput *ao)
{
	OSXOutput *od = (OSXOutput *)ao;

	const ScopeLock protect(od->mutex);
	od->buffer->Clear();
}

static void
osx_output_close(AudioOutput *ao)
{
	OSXOutput *od = (OSXOutput *)ao;

	AudioOutputUnitStop(od->au);
	AudioUnitUninitialize(od->au);

	delete od->buffer;
}

static bool
osx_output_open(AudioOutput *ao, AudioFormat &audio_format,
		Error &error)
{
	char errormsg[1024];
	OSXOutput *od = (OSXOutput *)ao;

	AudioStreamBasicDescription stream_description;
	stream_description.mSampleRate = audio_format.sample_rate;
	stream_description.mFormatID = kAudioFormatLinearPCM;
	stream_description.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;

	switch (audio_format.format) {
	case SampleFormat::S8:
		stream_description.mBitsPerChannel = 8;
		break;

	case SampleFormat::S16:
		stream_description.mBitsPerChannel = 16;
		break;

	case SampleFormat::S32:
		stream_description.mBitsPerChannel = 32;
		break;

	default:
		audio_format.format = SampleFormat::S32;
		stream_description.mBitsPerChannel = 32;
		break;
	}

	if (IsBigEndian())
		stream_description.mFormatFlags |= kLinearPCMFormatFlagIsBigEndian;

	stream_description.mBytesPerPacket = audio_format.GetFrameSize();
	stream_description.mFramesPerPacket = 1;
	stream_description.mBytesPerFrame = stream_description.mBytesPerPacket;
	stream_description.mChannelsPerFrame = audio_format.channels;

	OSStatus status =
		AudioUnitSetProperty(od->au, kAudioUnitProperty_StreamFormat,
				     kAudioUnitScope_Input, 0,
				     &stream_description,
				     sizeof(stream_description));
	if (status != noErr) {
		error.Set(osx_output_domain, status,
			  "Unable to set format on OS X device");
		return false;
	}

	status = AudioUnitInitialize(od->au);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to initialize OS X audio unit: %s",
			     errormsg);
		return false;
	}

	FormatDebug(osx_output_domain, "%s: getting asbd of audio unit", od->device_name);
	UInt32 size = sizeof(od->asbd);
	memset(od->asbd, 0, size);
	status = AudioUnitGetProperty(od->au,
				kAudioUnitProperty_StreamFormat,
				kAudioUnitScope_Input,
				0, 
				od->asbd,
				&size);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to get AudioStreamDescription of output HAL device: %s",
			     errormsg);
		return false;
	}

	FormatDebug(osx_output_domain, "%s: creating ring buffer", od->device_name);

	/* create a buffer of 1s */
	od->buffer = new DynamicFifoBuffer<uint8_t>(audio_format.sample_rate *
						    audio_format.GetFrameSize());

	status = AudioOutputUnitStart(od->au);
	if (status != 0) {
		AudioUnitUninitialize(od->au);
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "unable to start audio output: %s",
			     errormsg);
		return false;
	}

	return true;
}

static size_t
osx_output_play(AudioOutput *ao, const void *chunk, size_t size,
		gcc_unused Error &error)
{
	OSXOutput *od = (OSXOutput *)ao;

	const ScopeLock protect(od->mutex);

	DynamicFifoBuffer<uint8_t>::Range dest;
	while (true) {
		dest = od->buffer->Write();
		if (!dest.IsEmpty())
			break;

		/* wait for some free space in the buffer */
		od->condition.wait(od->mutex);
	}

	if (size > dest.size)
		size = dest.size;

	memcpy(dest.data, chunk, size);
	od->buffer->Append(size);

	return size;
}

const struct AudioOutputPlugin osx_output_plugin = {
	"osx",
	osx_output_test_default_device,
	osx_output_init,
	osx_output_finish,
	osx_output_enable,
	osx_output_disable,
	osx_output_open,
	osx_output_close,
	nullptr,
	nullptr,
	osx_output_play,
	nullptr,
	osx_output_cancel,
	nullptr,
	nullptr,
};
