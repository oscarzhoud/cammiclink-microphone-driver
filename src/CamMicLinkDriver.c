/*
 * CamMicLink Microphone 0.3.0 — AudioServerPlugIn interface.
 * Copyright (c) 2026 Oscar Zhou. MIT license; see ../LICENSE.
 * Based on the interface/property conventions of Apple's NullAudio sample.
 * See ../reference/LICENSE.txt (Copyright 2024 Apple Inc., MIT).
 *
 * Public device: input only. Private writer: hidden, output only.
 * There are no gain, mute, data-source or DSP controls in this driver.
 * ioMainBuffer is raw stereo interleaved Float32 PCM, never AudioBufferList.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "CMLAudioCore.h"

/* CFPlugInFactories: 65D2A6AE-3499-4B09-BB23-5E967D35CC30 */
enum { kPlugin = kAudioObjectPlugInObject, kInputDevice = 2,
       kInputStream = 3, kWriterDevice = 4, kWriterStream = 5, kBox = 6 };
#define INPUT_UID "com.oscarzhou.cammiclink.microphone.v3"
#define WRITER_UID "com.oscarzhou.cammiclink.writer.v3"
#define BOX_UID "com.oscarzhou.cammiclink.box.v3"
#define BUNDLE_ID "com.oscarzhou.cammiclink.audiodriver"

static AudioServerPlugInHostRef gHost;
static _Atomic UInt32 gRefs = 1;
static _Atomic UInt32 gRunning[2];
static _Atomic bool gActive[2] = { true, true };
static AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface *gInterfacePointer = &gInterface;
static AudioServerPlugInDriverRef gDriver = &gInterfacePointer;

static bool isDevice(AudioObjectID id) { return id == kInputDevice || id == kWriterDevice; }
static bool isStream(AudioObjectID id) { return id == kInputStream || id == kWriterStream; }
static bool isObject(AudioObjectID id) { return id >= kPlugin && id <= kBox; }
static unsigned indexFor(AudioObjectID id) { return id == kWriterDevice || id == kWriterStream; }
static AudioObjectID deviceFor(AudioObjectID id) { return id == kInputStream ? kInputDevice : kWriterDevice; }
static AudioObjectID streamFor(AudioObjectID id) { return id == kInputDevice ? kInputStream : kWriterStream; }
static bool validDriver(AudioServerPlugInDriverRef driver) { return driver == gDriver; }
static bool ioScope(AudioObjectID id, AudioObjectPropertyScope scope) {
    (void)id;
    return scope == kAudioObjectPropertyScopeInput || scope == kAudioObjectPropertyScopeOutput;
}
static bool populatedScope(AudioObjectID id, AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal ||
        (id == kInputDevice && scope == kAudioObjectPropertyScopeInput) ||
        (id == kWriterDevice && scope == kAudioObjectPropertyScopeOutput);
}
static AudioStreamBasicDescription format(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = CML_SAMPLE_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = CML_CHANNELS * sizeof(Float32);
    f.mFramesPerPacket = 1; f.mChannelsPerFrame = CML_CHANNELS; f.mBitsPerChannel = 32;
    return f;
}
static bool supportedFormat(const AudioStreamBasicDescription *f) {
    AudioStreamBasicDescription wanted = format();
    return f->mSampleRate == wanted.mSampleRate && f->mFormatID == wanted.mFormatID &&
        f->mFormatFlags == wanted.mFormatFlags && f->mBytesPerPacket == wanted.mBytesPerPacket &&
        f->mFramesPerPacket == 1 && f->mBytesPerFrame == wanted.mBytesPerFrame &&
        f->mChannelsPerFrame == CML_CHANNELS && f->mBitsPerChannel == 32;
}
static OSStatus copyValue(UInt32 capacity, UInt32 *written, void *out, const void *value, UInt32 bytes) {
    *written = 0;
    if (capacity < bytes || (bytes && !out)) return kAudioHardwareBadPropertySizeError;
    if (bytes) memcpy(out, value, bytes);
    *written = bytes;
    return noErr;
}
static OSStatus copyArray(UInt32 capacity, UInt32 *written, void *out, const void *values, UInt32 count, UInt32 stride) {
    UInt32 n = capacity / stride;
    if (n > count) n = count;
    return copyValue(capacity, written, out, values, n * stride);
}
static OSStatus copyString(UInt32 capacity, UInt32 *written, void *out, CFStringRef value) {
    if (capacity < sizeof(value) || !out) return kAudioHardwareBadPropertySizeError;
    CFRetain(value);
    return copyValue(capacity, written, out, &value, sizeof(value));
}
static CFStringRef objectName(AudioObjectID id) {
    switch (id) {
        case kPlugin: return CFSTR("CamMicLink Audio Driver");
        case kInputDevice: return CFSTR("CamMicLink Microphone");
        case kWriterDevice: return CFSTR("CamMicLink Private Writer");
        case kInputStream: return CFSTR("Microphone Input");
        case kWriterStream: return CFSTR("Private Audio Feed");
        default: return CFSTR("CamMicLink Audio");
    }
}
static UInt32 ownedObjects(AudioObjectID id, AudioObjectPropertyScope scope, AudioObjectID out[3]) {
    if (id == kPlugin) { out[0] = kBox; out[1] = kInputDevice; out[2] = kWriterDevice; return 3; }
    if (id == kBox) return 0;
    if (isDevice(id) && populatedScope(id, scope)) { out[0] = streamFor(id); return 1; }
    return 0;
}

/* Single property-size definition is also used by HasProperty and GetPropertyData.
 * Unsupported selectors/scopes return unknown, never fabricated success. */
static OSStatus propertySize(AudioObjectID id, const AudioObjectPropertyAddress *a, UInt32 *size) {
    if (!isObject(id)) return kAudioHardwareBadObjectError;
    if (!a || !size) return kAudioHardwareIllegalOperationError;
    if (a->mElement != kAudioObjectPropertyElementMain) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = a->mSelector;
    const AudioObjectPropertyScope scope = a->mScope;
    const bool global = scope == kAudioObjectPropertyScopeGlobal;
    if (!global && !(isDevice(id) && ioScope(id, scope))) return kAudioHardwareUnknownPropertyError;
    switch (s) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
            if (!global) break;
            *size = sizeof(UInt32); return noErr;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
            if (!global) break;
            *size = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID values[3];
            *size = ownedObjects(id, scope, values) * sizeof(AudioObjectID); return noErr;
        }
    }
    if (id == kPlugin && global) switch (s) {
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyResourceBundle:
            *size = sizeof(CFStringRef); return noErr;
        case kAudioPlugInPropertyDeviceList:
            *size = 2 * sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *size = sizeof(AudioObjectID); return noErr;
    }
    if (id == kBox && global) switch (s) {
        case kAudioBoxPropertyBoxUID:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertySerialNumber:
        case kAudioObjectPropertyFirmwareVersion:
            *size = sizeof(CFStringRef); return noErr;
        case kAudioBoxPropertyTransportType:
        case kAudioBoxPropertyHasAudio:
        case kAudioBoxPropertyHasVideo:
        case kAudioBoxPropertyHasMIDI:
        case kAudioBoxPropertyIsProtected:
        case kAudioBoxPropertyAcquired:
        case kAudioBoxPropertyAcquisitionFailed:
        case kAudioObjectPropertyIdentify:
            *size = sizeof(UInt32); return noErr;
        case kAudioBoxPropertyDeviceList:
            *size = 2 * sizeof(AudioObjectID); return noErr;
        case kAudioBoxPropertyClockDeviceList:
            *size = 0; return noErr;
    }
    if (isDevice(id)) switch (s) {
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            if (!global) break;
            *size = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:
            if (!global) break;
            *size = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
            if (!ioScope(id, scope)) break;
            *size = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyStreams:
            *size = populatedScope(id, scope) ? sizeof(AudioObjectID) : 0; return noErr;
        case kAudioObjectPropertyControlList:
            *size = 0; return noErr;
        case kAudioDevicePropertyNominalSampleRate:
            if (!global) break;
            *size = sizeof(Float64); return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            if (!global) break;
            *size = sizeof(AudioValueRange); return noErr;
        case kAudioDevicePropertyRelatedDevices:
            if (!global) break;
            *size = 2 * sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (!ioScope(id, scope) || !populatedScope(id, scope)) break;
            *size = 2 * sizeof(UInt32); return noErr;
        case kAudioDevicePropertyPreferredChannelLayout:
            if (!ioScope(id, scope) || !populatedScope(id, scope)) break;
            *size = offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription); return noErr;
    }
    if (isStream(id) && global) switch (s) {
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
            *size = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *size = sizeof(AudioStreamBasicDescription); return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *size = sizeof(AudioStreamRangedDescription); return noErr;
    }
    return kAudioHardwareUnknownPropertyError;
}
static Boolean hasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)pid; UInt32 size;
    return validDriver(d) && propertySize(id, a, &size) == noErr;
}
static OSStatus settable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)pid;
    if (!validDriver(d)) return kAudioHardwareBadObjectError;
    if (!out) return kAudioHardwareIllegalOperationError;
    UInt32 size; OSStatus err = propertySize(id, a, &size);
    if (err) return err;
    *out = (isDevice(id) && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
        (isStream(id) && (a->mSelector == kAudioStreamPropertyIsActive ||
                         a->mSelector == kAudioStreamPropertyVirtualFormat ||
                         a->mSelector == kAudioStreamPropertyPhysicalFormat));
    return noErr;
}
static OSStatus getSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 *out) {
    (void)pid; (void)qsize; (void)qdata;
    return validDriver(d) ? propertySize(id, a, out) : kAudioHardwareBadObjectError;
}
static OSStatus getData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 capacity, UInt32 *written, void *out) {
    (void)pid;
    if (!validDriver(d)) return kAudioHardwareBadObjectError;
    if (!written) return kAudioHardwareIllegalOperationError;
    *written = 0;
    UInt32 size; OSStatus err = propertySize(id, a, &size); if (err) return err;
    const UInt32 s = a->mSelector;
    UInt32 u = 0;
    AudioObjectID ids[3];
#define RETURN_U(v) do { u = (v); return copyValue(capacity, written, out, &u, sizeof(u)); } while (0)
#define RETURN_IDS(n) return copyArray(capacity, written, out, ids, (n), sizeof(AudioObjectID))
    switch (s) {
        case kAudioObjectPropertyBaseClass: RETURN_U(kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            RETURN_U(id == kPlugin ? kAudioPlugInClassID : id == kBox ? kAudioBoxClassID : isDevice(id) ? kAudioDeviceClassID : kAudioStreamClassID);
        case kAudioObjectPropertyOwner:
            RETURN_U(id == kPlugin ? kAudioObjectUnknown : isStream(id) ? deviceFor(id) : kPlugin);
        case kAudioObjectPropertyName: return copyString(capacity, written, out, objectName(id));
        case kAudioObjectPropertyManufacturer: return copyString(capacity, written, out, CFSTR("Oscar Zhou"));
        case kAudioObjectPropertyOwnedObjects: {
            UInt32 n = ownedObjects(id, a->mScope, ids); RETURN_IDS(n);
        }
    }
    if (id == kPlugin) switch (s) {
        case kAudioPlugInPropertyBundleID: return copyString(capacity, written, out, CFSTR(BUNDLE_ID));
        case kAudioPlugInPropertyResourceBundle: return copyString(capacity, written, out, CFSTR(""));
        case kAudioPlugInPropertyDeviceList: ids[0] = kInputDevice; ids[1] = kWriterDevice; RETURN_IDS(2);
        case kAudioPlugInPropertyBoxList: ids[0] = kBox; RETURN_IDS(1);
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (qsize != sizeof(CFStringRef) || !qdata) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef *)qdata;
            if (!uid || CFGetTypeID(uid) != CFStringGetTypeID()) return kAudioHardwareIllegalOperationError;
            if (s == kAudioPlugInPropertyTranslateUIDToBox) u = CFEqual(uid, CFSTR(BOX_UID)) ? kBox : kAudioObjectUnknown;
            else if (CFEqual(uid, CFSTR(INPUT_UID))) u = kInputDevice;
            else if (CFEqual(uid, CFSTR(WRITER_UID))) u = kWriterDevice;
            else u = kAudioObjectUnknown;
            return copyValue(capacity, written, out, &u, sizeof(u));
        }
    }
    if (id == kBox) switch (s) {
        case kAudioBoxPropertyBoxUID: return copyString(capacity, written, out, CFSTR(BOX_UID));
        case kAudioObjectPropertyModelName: return copyString(capacity, written, out, CFSTR("CamMicLink Microphone"));
        case kAudioObjectPropertySerialNumber: return copyString(capacity, written, out, CFSTR("CamMicLink-030"));
        case kAudioObjectPropertyFirmwareVersion: return copyString(capacity, written, out, CFSTR("0.3.0"));
        case kAudioBoxPropertyTransportType: RETURN_U(kAudioDeviceTransportTypeVirtual);
        case kAudioBoxPropertyHasAudio:
        case kAudioBoxPropertyAcquired: RETURN_U(1);
        case kAudioBoxPropertyHasVideo:
        case kAudioBoxPropertyHasMIDI:
        case kAudioBoxPropertyIsProtected:
        case kAudioBoxPropertyAcquisitionFailed:
        case kAudioObjectPropertyIdentify: RETURN_U(0);
        case kAudioBoxPropertyDeviceList: ids[0] = kInputDevice; ids[1] = kWriterDevice; RETURN_IDS(2);
        case kAudioBoxPropertyClockDeviceList: RETURN_IDS(0);
    }
    if (isDevice(id)) switch (s) {
        case kAudioDevicePropertyDeviceUID: return copyString(capacity, written, out, id == kInputDevice ? CFSTR(INPUT_UID) : CFSTR(WRITER_UID));
        case kAudioDevicePropertyModelUID: return copyString(capacity, written, out, CFSTR("com.oscarzhou.cammiclink.model.v3"));
        case kAudioDevicePropertyTransportType: RETURN_U(kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyClockDomain: RETURN_U(0x434D4C33);
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyClockIsStable: RETURN_U(1);
        case kAudioDevicePropertyDeviceIsRunning: RETURN_U(atomic_load_explicit(&gRunning[indexFor(id)], memory_order_relaxed) != 0);
        case kAudioDevicePropertyIsHidden: RETURN_U(id == kWriterDevice);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: RETURN_U(id == kInputDevice && a->mScope == kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_U(0);
        case kAudioDevicePropertyLatency: RETURN_U(id == kInputDevice && a->mScope == kAudioObjectPropertyScopeInput ? CML_DELAY_FRAMES : 0);
        case kAudioDevicePropertySafetyOffset: RETURN_U(0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_U(CML_PERIOD_FRAMES);
        case kAudioDevicePropertyStreams: ids[0] = streamFor(id); RETURN_IDS(populatedScope(id, a->mScope) ? 1 : 0);
        case kAudioObjectPropertyControlList: RETURN_IDS(0);
        case kAudioDevicePropertyRelatedDevices: ids[0] = kInputDevice; ids[1] = kWriterDevice; RETURN_IDS(2);
        case kAudioDevicePropertyNominalSampleRate: {
            Float64 rate = CML_SAMPLE_RATE; return copyValue(capacity, written, out, &rate, sizeof(rate));
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange range = { CML_SAMPLE_RATE, CML_SAMPLE_RATE };
            return copyArray(capacity, written, out, &range, 1, sizeof(range));
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: ids[0] = 1; ids[1] = 2; RETURN_IDS(2);
        case kAudioDevicePropertyPreferredChannelLayout: {
            struct { AudioChannelLayout layout; AudioChannelDescription extra; } storage = {0};
            storage.layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            storage.layout.mNumberChannelDescriptions = 2;
            storage.layout.mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            storage.extra.mChannelLabel = kAudioChannelLabel_Right;
            return copyValue(capacity, written, out, &storage, size);
        }
    }
    if (isStream(id)) switch (s) {
        case kAudioStreamPropertyIsActive: RETURN_U(atomic_load_explicit(&gActive[indexFor(id)], memory_order_relaxed));
        case kAudioStreamPropertyDirection: RETURN_U(id == kInputStream);
        case kAudioStreamPropertyTerminalType: RETURN_U(id == kInputStream ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: RETURN_U(1);
        case kAudioStreamPropertyLatency: RETURN_U(0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            AudioStreamBasicDescription f = format(); return copyValue(capacity, written, out, &f, sizeof(f));
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = { .mFormat = format(), .mSampleRateRange = { CML_SAMPLE_RATE, CML_SAMPLE_RATE } };
            return copyArray(capacity, written, out, &r, 1, sizeof(r));
        }
    }
    return kAudioHardwareUnknownPropertyError;
#undef RETURN_U
#undef RETURN_IDS
}
static OSStatus setData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 size, const void *data) {
    (void)qsize; (void)qdata;
    Boolean canSet; OSStatus err = settable(d, id, pid, a, &canSet);
    if (err) return err;
    if (!canSet) return kAudioHardwareUnsupportedOperationError;
    if (!data) return kAudioHardwareIllegalOperationError;
    if (isDevice(id)) {
        if (size != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)data == CML_SAMPLE_RATE ? noErr : kAudioHardwareIllegalOperationError;
    }
    if (a->mSelector == kAudioStreamPropertyIsActive) {
        if (size != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        bool active = *(const UInt32 *)data != 0;
        if (atomic_exchange_explicit(&gActive[indexFor(id)], active, memory_order_relaxed) != active && gHost)
            gHost->PropertiesChanged(gHost, id, 1, a);
        return noErr;
    }
    if (size != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
    return supportedFormat(data) ? noErr : kAudioDeviceUnsupportedFormatError;
}

static ULONG addRef(void *d) {
    if (d != gDriver) return 0;
    return atomic_fetch_add_explicit(&gRefs, 1, memory_order_relaxed) + 1;
}
static ULONG release(void *d) {
    if (d != gDriver) return 0;
    UInt32 n = atomic_load_explicit(&gRefs, memory_order_relaxed);
    while (n && !atomic_compare_exchange_weak_explicit(&gRefs, &n, n - 1, memory_order_relaxed, memory_order_relaxed)) {}
    return n ? n - 1 : 0;
}
static HRESULT query(void *d, REFIID iid, LPVOID *out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (d != gDriver) return E_NOINTERFACE;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, iid);
    bool match = CFEqual(uuid, IUnknownUUID) || CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(uuid);
    if (!match) return E_NOINTERFACE;
    addRef(d); *out = gDriver; return S_OK;
}
__attribute__((visibility("default")))
void *CMLDriver_Create(CFAllocatorRef allocator, CFUUIDRef requestedType) {
    (void)allocator;
    if (!requestedType || !CFEqual(requestedType, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriver;
}
static OSStatus initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    if (!validDriver(d)) return kAudioHardwareBadObjectError;
    gHost = host; CMLInitialize(); return noErr;
}
static OSStatus createDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *client, AudioObjectID *out) {
    (void)desc; (void)client; if (out) *out = kAudioObjectUnknown;
    return validDriver(d) ? kAudioHardwareUnsupportedOperationError : kAudioHardwareBadObjectError;
}
static OSStatus destroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) {
    return validDriver(d) && isDevice(id) ? kAudioHardwareUnsupportedOperationError : kAudioHardwareBadObjectError;
}
static OSStatus clientChange(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *client) {
    if (!validDriver(d) || !isDevice(id)) return kAudioHardwareBadObjectError;
    return client ? noErr : kAudioHardwareIllegalOperationError;
}
static OSStatus performChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) {
    (void)action; (void)info;
    return validDriver(d) && isDevice(id) ? kAudioHardwareUnsupportedOperationError : kAudioHardwareBadObjectError;
}
static OSStatus abortChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) {
    (void)action; (void)info;
    return validDriver(d) && isDevice(id) ? noErr : kAudioHardwareBadObjectError;
}
static OSStatus startIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    if (!validDriver(d) || !isDevice(id)) return kAudioHardwareBadObjectError;
    if (CMLStartIO(id, client)) return kAudioHardwareIllegalOperationError;
    atomic_fetch_add_explicit(&gRunning[indexFor(id)], 1, memory_order_relaxed);
    return noErr;
}
static OSStatus stopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    if (!validDriver(d) || !isDevice(id)) return kAudioHardwareBadObjectError;
    CMLStopIO(id, client);
    UInt32 n = atomic_load_explicit(&gRunning[indexFor(id)], memory_order_relaxed);
    while (n && !atomic_compare_exchange_weak_explicit(&gRunning[indexFor(id)], &n, n - 1, memory_order_relaxed, memory_order_relaxed)) {}
    return noErr;
}
static OSStatus zeroTime(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *sample, UInt64 *host, UInt64 *seed) {
    (void)client;
    if (!validDriver(d) || !isDevice(id)) return kAudioHardwareBadObjectError;
    if (!sample || !host || !seed) return kAudioHardwareIllegalOperationError;
    CMLGetZeroTimeStamp(sample, host, seed); return noErr;
}
static OSStatus willDo(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)client;
    if (!validDriver(d) || !isDevice(id)) return kAudioHardwareBadObjectError;
    if (will) *will = (id == kInputDevice && op == kAudioServerPlugInIOOperationReadInput) ||
                      (id == kWriterDevice && op == kAudioServerPlugInIOOperationWriteMix);
    if (inPlace) *inPlace = true;
    return noErr;
}
static OSStatus beginEnd(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) {
    (void)client; (void)op; (void)frames; (void)cycle;
    return validDriver(d) && isDevice(id) ? noErr : kAudioHardwareBadObjectError;
}
static OSStatus doIO(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary) {
    (void)secondary;
    if (!validDriver(d) || !isDevice(id) || stream != streamFor(id)) return kAudioHardwareBadObjectError;
    if (!frames) return noErr;
    if (!main || !cycle) return kAudioHardwareIllegalOperationError;
    if (id == kInputDevice && op == kAudioServerPlugInIOOperationReadInput) {
        if (!atomic_load_explicit(&gActive[0], memory_order_relaxed) ||
            !(cycle->mInputTime.mFlags & kAudioTimeStampSampleTimeValid) || !isfinite(cycle->mInputTime.mSampleTime)) {
            memset(main, 0, (size_t)frames * CML_CHANNELS * sizeof(Float32));
        } else CMLRead(main, frames, cycle->mInputTime.mSampleTime);
        return noErr;
    }
    if (id == kWriterDevice && op == kAudioServerPlugInIOOperationWriteMix) {
        if (atomic_load_explicit(&gActive[1], memory_order_relaxed) &&
            (cycle->mOutputTime.mFlags & kAudioTimeStampSampleTimeValid) && isfinite(cycle->mOutputTime.mSampleTime))
            CMLWrite(main, frames, cycle->mOutputTime.mSampleTime, client);
        return noErr;
    }
    return kAudioHardwareUnsupportedOperationError;
}
static AudioServerPlugInDriverInterface gInterface = {
    NULL, query, addRef, release, initialize, createDevice, destroyDevice,
    clientChange, clientChange, performChange, abortChange,
    hasProperty, settable, getSize, getData, setData,
    startIO, stopIO, zeroTime, willDo, beginEnd, doIO, beginEnd
};
