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

#define CHANNEL_MAP "channel_map"

struct OSXOutput {
	AudioOutput base;

	/* configuration settings */
	OSType component_subtype;
	/* only applicable with kAudioUnitSubType_HALOutput */
	const char *device_name;
	const char *channel_map;

	AudioComponentInstance au;
	AudioStreamBasicDescription asbd;

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

	oo->channel_map = block.GetBlockValue(CHANNEL_MAP);
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
osx_output_set_channel_map(OSXOutput *oo, Error &error)
{
	AudioStreamBasicDescription desc;
	SInt32 *channelmap = nullptr;
	UInt32 size, numchannels;
	char errormsg[1024];
	const char *remaining;
	bool ret = true;
	bool wantnumber;
	unsigned int j;
	OSStatus status;

	size = sizeof(desc);
	memset(&desc, 0, size);
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
	channelmap = new SInt32[numchannels];

	remaining = oo->channel_map;
	char *endptr;
	j = 0;
	wantnumber = true;

	while (*remaining) {
		FormatDebug(osx_output_domain, "%s %s remaining: %s j: %u wantnumber: %u", oo->device_name, CHANNEL_MAP, remaining, j, wantnumber);

		if (j >= numchannels) {
			error.Format(osx_output_domain,
			     "%s: %s contains more than %u entries", oo->device_name, CHANNEL_MAP, numchannels);
			ret = false;
			goto done;
		}

		if (*remaining == ':' && !wantnumber) {
			++remaining;
			wantnumber = true;
		} else if ((isdigit(*remaining) || *remaining == '-') && wantnumber) {
			channelmap[j] = strtol(remaining, &endptr, 10);
			if (channelmap[j] < -1) {
				error.Format(osx_output_domain,
				     "%s: %s value %d not allowed (must be -1 or greater)", oo->device_name, CHANNEL_MAP, channelmap[j]);
				ret = false;
				goto done;
			}
			remaining = endptr;
			wantnumber = false;
			FormatDebug(osx_output_domain, "%s channelmap[%u] = %d", oo->device_name, j, channelmap[j]);
			++j;
		} else {
			error.Format(osx_output_domain,
			     "%s: invalid character %c in %s", oo->device_name, *remaining, CHANNEL_MAP);
			ret = false;
			goto done;
		}
	}

	if (j + 1 < numchannels) {
		error.Format(osx_output_domain,
		     "%s: %s contains less than %u entries", oo->device_name, CHANNEL_MAP, numchannels);
		ret = false;
		goto done;
	}

	size = numchannels * sizeof(SInt32);
	status = AudioUnitSetProperty(oo->au, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Input, 0, channelmap, size);
	if (status != noErr) {
		osx_os_status_to_cstring(status, errormsg, sizeof(errormsg));
		error.Format(osx_output_domain, status,
			     "Unable to set OS X audio output channel map: %s",
			     errormsg);
		ret = false;
		goto done;
	}

done:
	delete[] channelmap;
	return ret;
}

static bool
osx_output_set_device(OSXOutput *oo, Error &error)
{
	bool ret = true;
	OSStatus status;
	UInt32 size, numdevices;
	AudioDeviceID *deviceids = nullptr;
	AudioObjectPropertyAddress propaddr;
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

	if (oo->channel_map && !osx_output_set_channel_map(oo, error))
		ret = false;
		goto done;

done:
	delete[] deviceids;
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
	DynamicFifoBuffer<uint8_t> *ring_buffer = od->buffer;
	AudioStreamBasicDescription asbd = od->asbd;
	AudioBuffer *audio_buffer = nullptr;
	size_t sample_size = asbd.mBytesPerFrame / asbd.mChannelsPerFrame;
	size_t audio_buffer_frame_size, sample_bytes_consumed, dest;
	unsigned int i, channel_count;

	assert(ring_buffer != nullptr);
	assert(in_bus_number == 0);	// '0' means we use the audio hardware to _output_ sound (instead of input)
	channel_count = 0;
	for (i = 0 ; i < buffer_list->mNumberBuffers; ++i) {
		audio_buffer = &buffer_list->mBuffers[i];
		assert(audio_buffer->mData != nullptr);
		channel_count += audio_buffer->mNumberChannels;
	}
	assert(channel_count == asbd.mChannelsPerFrame);

	// Acquire mutex when accessing ring_buffer
	od->mutex.lock();

	auto src = ring_buffer->Read();

	UInt32 available_frames = src.size / asbd.mBytesPerFrame;
	// Never write more frames than we were asked
	if (available_frames > in_number_frames)
		available_frames = in_number_frames;

	// Loop over ring buffer frames
	for (UInt32 current_frame = 0; current_frame < available_frames; ++current_frame) {
		// De-interleave the current ring buffer frame onto available audio buffers
		sample_bytes_consumed = 0;
		for (i = 0 ; i < buffer_list->mNumberBuffers; ++i) {
			// Copy sub-frame of the current ring buffer frame into its respective audio buffer
			audio_buffer = &buffer_list->mBuffers[i];
			audio_buffer_frame_size = audio_buffer->mNumberChannels * sample_size;
			dest = (size_t) audio_buffer->mData + current_frame * audio_buffer_frame_size;
			memcpy(
				(void *) dest,
				src.data + current_frame * asbd.mBytesPerFrame + sample_bytes_consumed,
				audio_buffer_frame_size
			);

			sample_bytes_consumed += audio_buffer_frame_size;
		}
	}

	ring_buffer->Consume(available_frames * asbd.mBytesPerFrame);

	od->condition.signal();
	od->mutex.unlock();

	// Play silence during a buffer underrun
	if (available_frames < in_number_frames) {
		// Fill the remaining space in each audio buffer with '0'
		for (i = 0 ; i < buffer_list->mNumberBuffers; ++i) {
			audio_buffer = &buffer_list->mBuffers[i];
			audio_buffer_frame_size = audio_buffer->mNumberChannels * sample_size;
			dest = (size_t) audio_buffer->mData + available_frames * audio_buffer_frame_size;
			memset(
				(void *) dest,
				0,
				(in_number_frames - available_frames) * audio_buffer_frame_size
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

	od->asbd.mSampleRate = audio_format.sample_rate;
	od->asbd.mFormatID = kAudioFormatLinearPCM;
	od->asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;

	switch (audio_format.format) {
	case SampleFormat::S8:
		od->asbd.mBitsPerChannel = 8;
		break;

	case SampleFormat::S16:
		od->asbd.mBitsPerChannel = 16;
		break;

	case SampleFormat::S32:
		od->asbd.mBitsPerChannel = 32;
		break;

	default:
		audio_format.format = SampleFormat::S32;
		od->asbd.mBitsPerChannel = 32;
		break;
	}

	if (IsBigEndian())
		od->asbd.mFormatFlags |= kLinearPCMFormatFlagIsBigEndian;

	od->asbd.mBytesPerPacket = audio_format.GetFrameSize();
	od->asbd.mFramesPerPacket = 1;
	od->asbd.mBytesPerFrame = od->asbd.mBytesPerPacket;
	od->asbd.mChannelsPerFrame = audio_format.channels;

	OSStatus status =
		AudioUnitSetProperty(od->au, kAudioUnitProperty_StreamFormat,
				     kAudioUnitScope_Input, 0,
				     &od->asbd,
				     sizeof(od->asbd));
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

	/* create a buffer of 1s */
	od->buffer = new DynamicFifoBuffer<uint8_t>(audio_format.sample_rate *
						    audio_format.GetFrameSize());

	status = AudioOutputUnitStart(od->au);
	if (status != noErr) {
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
