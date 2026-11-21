/*
 * plugin_smoke - loads the built HAL and MIDI bundles in-process (nothing is
 * installed, Core Audio is not touched) and exercises the plug-in entry
 * points with a stub host. Works without the hardware attached.
 *   make smoke
 * SPDX-License-Identifier: MIT
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/MIDIDriver.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } else printf("ok   %s\n", #c); } while (0)

static CFMutableDictionaryRef gStorage;
static AudioServerPlugInDriverRef gDrv;

static OSStatus H_PropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a)
{ (void)h; (void)a; printf("     host: PropertiesChanged obj %u (%u)\n", o, n); return 0; }
static OSStatus H_Copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *out)
{ (void)h; CFPropertyListRef v = CFDictionaryGetValue(gStorage, k); *out = v ? CFRetain(v) : NULL; return v ? 0 : kAudioHardwareUnknownPropertyError; }
static OSStatus H_Write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v)
{ (void)h; CFDictionarySetValue(gStorage, k, v); return 0; }
static OSStatus H_Delete(AudioServerPlugInHostRef h, CFStringRef k)
{ (void)h; CFDictionaryRemoveValue(gStorage, k); return 0; }
/* Like the real host, perform configuration changes later (not inside SetPropertyData). */
static AudioObjectID gReqDev; static UInt64 gReqAction; static void *gReqInfo; static int gReqPending;
static OSStatus H_Request(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void *info)
{ (void)h; gReqDev = dev; gReqAction = action; gReqInfo = info; gReqPending = 1; return 0; }
static void flush_requests(void)
{ if (gReqPending) { gReqPending = 0; (*gDrv)->PerformDeviceConfigurationChange(gDrv, gReqDev, gReqAction, gReqInfo); } }

static AudioServerPlugInHostInterface gHost = {
    .PropertiesChanged = H_PropertiesChanged, .CopyFromStorage = H_Copy, .WriteToStorage = H_Write,
    .DeleteFromStorage = H_Delete, .RequestDeviceConfigurationChange = H_Request,
};

static void *load(const char *path, CFUUIDRef type, CFUUIDRef iface)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
    CFPlugInRef plug = CFPlugInCreate(NULL, url);
    CFRelease(url);
    if (!plug) return NULL;
    CFArrayRef f = CFPlugInFindFactoriesForPlugInTypeInPlugIn(type, plug);
    if (!f || CFArrayGetCount(f) == 0) return NULL;
    IUnknownVTbl **unk = CFPlugInInstanceCreate(NULL, (CFUUIDRef)CFArrayGetValueAtIndex(f, 0), type);
    if (!unk) return NULL;
    void *out = NULL;
    (*unk)->QueryInterface(unk, CFUUIDGetUUIDBytes(iface), &out);
    (*unk)->Release(unk);
    return out;
}

int main(int argc, char **argv)
{
    const char *hal = argc > 1 ? argv[1] : "build/AudiophileUSB.driver";
    const char *midi = argc > 2 ? argv[2] : "build/AudiophileUSBMIDI.plugin";
    gStorage = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    gDrv = load(hal, kAudioServerPlugInTypeUUID, kAudioServerPlugInDriverInterfaceUUID);
    CHECK(gDrv != NULL);
    if (!gDrv) return 1;
    AudioServerPlugInHostInterface *hostPtr = &gHost;
    CHECK((*gDrv)->Initialize(gDrv, hostPtr) == 0);

    AudioObjectPropertyAddress a = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    CHECK((*gDrv)->GetPropertyDataSize(gDrv, kAudioObjectPlugInObject, 0, &a, 0, NULL, &size) == 0);
    printf("     device list size %u (0 = hardware not connected)\n", size);

    a.mSelector = kAudioObjectPropertyName;
    CFStringRef name = NULL; size = sizeof name;
    CHECK((*gDrv)->GetPropertyData(gDrv, 2, 0, &a, 0, NULL, size, &size, &name) == 0 && name);
    if (name) { CFShow(name); CFRelease(name); }

    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    Float64 rate = 0; size = sizeof rate;
    CHECK((*gDrv)->GetPropertyData(gDrv, 2, 0, &a, 0, NULL, size, &size, &rate) == 0 && rate == 48000.0);
    rate = 44100.0;
    CHECK((*gDrv)->SetPropertyData(gDrv, 2, 0, &a, 0, NULL, sizeof rate, &rate) == 0);
    flush_requests();
    size = sizeof rate;
    (*gDrv)->GetPropertyData(gDrv, 2, 0, &a, 0, NULL, size, &size, &rate);
    CHECK(rate == 44100.0);
    rate = 96000.0; /* switches to the half-duplex 24bit-96k-play mode */
    CHECK((*gDrv)->SetPropertyData(gDrv, 2, 0, &a, 0, NULL, sizeof rate, &rate) == 0);
    flush_requests();
    size = sizeof rate;
    (*gDrv)->GetPropertyData(gDrv, 2, 0, &a, 0, NULL, size, &size, &rate);
    CHECK(rate == 96000.0);
    rate = 48000.0; /* back to full duplex */
    CHECK((*gDrv)->SetPropertyData(gDrv, 2, 0, &a, 0, NULL, sizeof rate, &rate) == 0);
    flush_requests();
    /* physical format 16-bit -> 16bit-48k mode */
    {
        AudioObjectPropertyAddress pf = { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioStreamBasicDescription f = { 0 }; size = sizeof f;
        (*gDrv)->GetPropertyData(gDrv, 4, 0, &pf, 0, NULL, size, &size, &f);
        CHECK(f.mBitsPerChannel == 24 && (f.mFormatFlags & kAudioFormatFlagIsBigEndian));
        f.mBitsPerChannel = 16; f.mSampleRate = 44100.0;
        CHECK((*gDrv)->SetPropertyData(gDrv, 4, 0, &pf, 0, NULL, sizeof f, &f) == 0);
        flush_requests();
        size = sizeof f;
        (*gDrv)->GetPropertyData(gDrv, 4, 0, &pf, 0, NULL, size, &size, &f);
        CHECK(f.mBitsPerChannel == 16 && f.mSampleRate == 44100.0 && !(f.mFormatFlags & kAudioFormatFlagIsBigEndian));
    }

    a.mSelector = 'apmd';
    CFStringRef mode = CFSTR("24bit-96k-play");
    CHECK((*gDrv)->SetPropertyData(gDrv, 2, 0, &a, 0, NULL, sizeof mode, &mode) == 0);
    flush_requests();
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    size = sizeof rate;
    (*gDrv)->GetPropertyData(gDrv, 2, 0, &a, 0, NULL, size, &size, &rate);
    CHECK(rate == 96000.0);
    a.mSelector = kAudioStreamPropertyIsActive;
    UInt32 active = 9; size = sizeof active;
    (*gDrv)->GetPropertyData(gDrv, 3, 0, &a, 0, NULL, size, &size, &active);
    CHECK(active == 0); /* input inactive in play-only 96k mode */
    a.mSelector = kAudioStreamPropertyVirtualFormat;
    AudioStreamBasicDescription f; size = sizeof f;
    CHECK((*gDrv)->GetPropertyData(gDrv, 4, 0, &a, 0, NULL, size, &size, &f) == 0 && f.mChannelsPerFrame == 2 && f.mSampleRate == 96000.0);
    CHECK(CFDictionaryGetValue(gStorage, CFSTR("Mode")) != NULL);

    /* Without the hardware StartIO must fail cleanly. */
    OSStatus st = (*gDrv)->StartIO(gDrv, 2, 1);
    printf("     StartIO without hardware -> %d\n", (int)st);
    if (st == 0) (*gDrv)->StopIO(gDrv, 2, 1);

    MIDIDriverInterface **m = load(midi, kMIDIDriverTypeID, kMIDIDriverInterfaceID);
    CHECK(m != NULL);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("plug-in smoke test passed\n");
    return 0;
}
