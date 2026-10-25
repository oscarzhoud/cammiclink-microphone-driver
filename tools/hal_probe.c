/*
 * CamMicLink 0.3.0 independent HAL integration probe.
 * Build: clang -std=c11 -O2 -Wall -Wextra tools/hal_probe.c \
 *          -framework CoreAudio -framework CoreFoundation -framework AudioToolbox \
 *          -o /tmp/cml-hal-probe
 *
 * Inspect topology: /tmp/cml-hal-probe inspect
 * Terminal A:       /tmp/cml-hal-probe tone 15 1024
 * Terminal B:       /tmp/cml-hal-probe record 10 512
 * Terminal C:       /tmp/cml-hal-probe record 8 256
 * Converted input:  /tmp/cml-hal-probe record-au 10 16000
 *                   /tmp/cml-hal-probe record-au 10 44100
 *
 * Run the writer first, then readers while it remains alive. Use separate processes.
 * The requested device buffer size is a preference, not a promise; actual callback
 * sizes are reported. A tone is generated directly from the device sample time,
 * with no producer thread, network, or application FIFO that could underrun.
 * Recording reads only the explicitly named virtual input UID, never the physical mic.
 * Record exit 0 means the steady-state tone met the stated numeric criteria;
 * it does not assert that third-party call processing is transparent.
 * Inspect exit 0 requires input-only visible mic and a hidden output-only writer.
 */
#define _DARWIN_C_SOURCE 1
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>

#define MIC_UID CFSTR("com.oscarzhou.cammiclink.microphone.v3")
#define WRITER_UID CFSTR("com.oscarzhou.cammiclink.writer.v3")
#define TONE_HZ 440.0
#define TONE_AMP 0.3
#define PI 3.14159265358979323846
#define MAX_HIST 32

typedef struct {
    int writer;
    double rate;
    float *capture;
    size_t capacity, samples;
    uint64_t calls, overflow, badBuffers, invalidTimes, timeBreaks;
    uint32_t sizes[MAX_HIST];
    uint64_t sizeCounts[MAX_HIST];
    unsigned sizeUsed;
    double firstSample, lastSample, previousEnd;
    int haveTime;
} Probe;

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector selector,
                                        AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = { selector, scope, kAudioObjectPropertyElementMain };
    return a;
}
static OSStatus get(AudioObjectID object, AudioObjectPropertySelector selector,
                    AudioObjectPropertyScope scope, UInt32 *size, void *data) {
    AudioObjectPropertyAddress a = addr(selector, scope);
    return AudioObjectGetPropertyData(object, &a, 0, NULL, size, data);
}
static AudioDeviceID lookup(CFStringRef uid) {
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal);
    AudioDeviceID id = kAudioObjectUnknown;
    UInt32 size = sizeof(id);
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a,
                                            sizeof(uid), &uid, &size, &id);
    if (st) fprintf(stderr, "UID lookup status=%d\n", (int)st);
    return id;
}
static uint32_t streamCount(AudioDeviceID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreams, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &a, 0, NULL, &size)) return 0;
    return size / sizeof(AudioStreamID);
}
static int deviceFormat(AudioDeviceID dev, AudioObjectPropertyScope scope,
                         AudioStreamBasicDescription *format) {
    AudioStreamID streams[16];
    UInt32 size = sizeof(streams);
    if (get(dev, kAudioDevicePropertyStreams, scope, &size, streams) || size == 0) return 0;
    size = sizeof(*format);
    return get(streams[0], kAudioStreamPropertyVirtualFormat,
               kAudioObjectPropertyScopeGlobal, &size, format) == noErr;
}
static void nameOf(AudioObjectID id, char *name, size_t capacity) {
    CFStringRef s = NULL;
    UInt32 size = sizeof(s);
    snprintf(name, capacity, "<unnamed>");
    if (!get(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &size, &s) && s) {
        CFStringGetCString(s, name, (CFIndex)capacity, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}
static int inspect(void) {
    AudioDeviceID mic = lookup(MIC_UID), writer = lookup(WRITER_UID);
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    if (st) { fprintf(stderr, "enumeration status=%d\n", (int)st); return 1; }
    AudioDeviceID *ids = calloc(1, size ? size : sizeof(AudioDeviceID));
    if (!ids) return 1;
    st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
    if (st) { free(ids); return 1; }
    int micListed = 0, writerListed = 0;
    unsigned micOccurrences = 0;
    printf("Normal device enumeration:\n");
    for (UInt32 i = 0; i < size / sizeof(*ids); ++i) {
        char name[256]; nameOf(ids[i], name, sizeof(name));
        uint32_t ins = streamCount(ids[i], kAudioObjectPropertyScopeInput);
        uint32_t outs = streamCount(ids[i], kAudioObjectPropertyScopeOutput);
        printf("  id=%u inputStreams=%u outputStreams=%u name=%s\n", ids[i], ins, outs, name);
        if (ids[i] == mic) micListed = 1;
        if (ids[i] == writer) writerListed = 1;
        if (strstr(name, "CamMicLink") && ins) ++micOccurrences;
    }
    free(ids);
    if (!mic || !writer) {
        fprintf(stderr, "FAIL: public UID=%u, hidden writer UID=%u; install/load 0.3.0 first.\n", mic, writer);
        return 1;
    }
    uint32_t hidden = 0; size = sizeof(hidden);
    st = get(writer, kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal, &size, &hidden);
    AudioStreamBasicDescription inf = {0}, outf = {0};
    int formats = deviceFormat(mic, kAudioObjectPropertyScopeInput, &inf) &&
                  deviceFormat(writer, kAudioObjectPropertyScopeOutput, &outf);
    printf("Public mic id=%u listed=%d in=%u out=%u rate=%.0f channels=%u\n", mic, micListed,
           streamCount(mic, kAudioObjectPropertyScopeInput), streamCount(mic, kAudioObjectPropertyScopeOutput),
           inf.mSampleRate, inf.mChannelsPerFrame);
    printf("Private writer id=%u listed=%d hidden=%u in=%u out=%u rate=%.0f channels=%u\n",
           writer, writerListed, hidden, streamCount(writer, kAudioObjectPropertyScopeInput),
           streamCount(writer, kAudioObjectPropertyScopeOutput), outf.mSampleRate, outf.mChannelsPerFrame);
    int good = micListed && !writerListed && !st && hidden && micOccurrences == 1 &&
        streamCount(mic, kAudioObjectPropertyScopeInput) == 1 &&
        streamCount(mic, kAudioObjectPropertyScopeOutput) == 0 &&
        streamCount(writer, kAudioObjectPropertyScopeInput) == 0 &&
        streamCount(writer, kAudioObjectPropertyScopeOutput) == 1 && formats &&
        inf.mSampleRate == 48000 && outf.mSampleRate == 48000 &&
        inf.mChannelsPerFrame == 2 && outf.mChannelsPerFrame == 2;
    printf("Topology: %s\n", good ? "PASS" : "FAIL");
    return good ? 0 : 1;
}
static void noteFrames(Probe *p, uint32_t frames, const AudioTimeStamp *time) {
    ++p->calls;
    unsigned i;
    for (i=0; i<p->sizeUsed; ++i) if (p->sizes[i] == frames) break;
    if (i == p->sizeUsed && i < MAX_HIST) { p->sizes[i] = frames; ++p->sizeUsed; }
    if (i < MAX_HIST) ++p->sizeCounts[i];
    if (time && (time->mFlags & kAudioTimeStampSampleTimeValid)) {
        if (!p->haveTime) { p->firstSample = time->mSampleTime; p->haveTime = 1; }
        else if (fabs(time->mSampleTime - p->previousEnd) > .01) ++p->timeBreaks;
        p->lastSample = time->mSampleTime;
        p->previousEnd = time->mSampleTime + frames;
    } else ++p->invalidTimes;
}
static OSStatus callback(AudioObjectID dev, const AudioTimeStamp *now,
                         const AudioBufferList *input, const AudioTimeStamp *inputTime,
                         AudioBufferList *output, const AudioTimeStamp *outputTime, void *context) {
    (void)dev; (void)now;
    Probe *p = context;
    const AudioBufferList *list = p->writer ? output : input;
    if (!list || !list->mNumberBuffers) { ++p->badBuffers; return noErr; }
    const AudioBuffer *first = &list->mBuffers[0];
    if (!first->mData || !first->mNumberChannels ||
        first->mDataByteSize % (sizeof(float) * first->mNumberChannels)) {
        ++p->badBuffers; return noErr;
    }
    uint32_t frames = first->mDataByteSize / (sizeof(float) * first->mNumberChannels);
    const AudioTimeStamp *time = p->writer ? outputTime : inputTime;
    noteFrames(p, frames, time);
    if (p->writer) {
        const double base = time && (time->mFlags & kAudioTimeStampSampleTimeValid)
            ? time->mSampleTime : (double)p->samples;
        for (UInt32 b=0; b<output->mNumberBuffers; ++b) {
            AudioBuffer *buf = &output->mBuffers[b];
            if (!buf->mData || !buf->mNumberChannels) continue;
            uint32_t bf = buf->mDataByteSize / (sizeof(float)*buf->mNumberChannels);
            float *dst = buf->mData;
            for (uint32_t i=0; i<bf; ++i) {
                float v = (float)(TONE_AMP * sin(2*PI*TONE_HZ*(base+i)/p->rate));
                for (uint32_t c=0; c<buf->mNumberChannels; ++c) dst[i*buf->mNumberChannels+c] = v;
            }
        }
        p->samples += frames;
    } else {
        const float *src = first->mData;
        size_t available = p->capacity - p->samples;
        size_t n = frames < available ? frames : available;
        for (size_t i=0; i<n; ++i) p->capture[p->samples+i] = src[i*first->mNumberChannels];
        p->samples += n;
        p->overflow += frames-n;
    }
    return noErr;
}
static void setUsage(AudioDeviceID dev, AudioDeviceIOProcID proc,
                     AudioObjectPropertyScope scope, int on) {
    UInt32 n = streamCount(dev, scope);
    if (!n) return;
    UInt32 bytes = (UInt32)(offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn)+n*sizeof(UInt32));
    AudioHardwareIOProcStreamUsage *u = calloc(1, bytes);
    if (!u) return;
    u->mIOProc = (void *)proc; u->mNumberStreams = n;
    for (UInt32 i=0; i<n; ++i) u->mStreamIsOn[i] = on;
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyIOProcStreamUsage, scope);
    OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, NULL, bytes, u);
    if (st) fprintf(stderr, "stream usage scope=%08x status=%d\n", (unsigned)scope, (int)st);
    free(u);
}
static void waitSeconds(double seconds) {
    struct timespec req = { (time_t)seconds, (long)((seconds-floor(seconds))*1e9) };
    while (nanosleep(&req, &req) != 0) {}
}
static int report(Probe *p, OSStatus stop, OSStatus destroy) {
    printf("callbacks=%llu frames=%zu firstSample=%.0f lastSample=%.0f timeBreaks=%llu invalidTimes=%llu badBuffers=%llu overflow=%llu\n",
           (unsigned long long)p->calls, p->samples, p->firstSample, p->lastSample,
           (unsigned long long)p->timeBreaks, (unsigned long long)p->invalidTimes,
           (unsigned long long)p->badBuffers, (unsigned long long)p->overflow);
    printf("Callback frames:");
    for (unsigned i=0;i<p->sizeUsed;++i) printf(" %u:%llu",p->sizes[i],(unsigned long long)p->sizeCounts[i]);
    printf("\n");
    int good = !stop && !destroy && p->calls && !p->badBuffers && !p->overflow && !p->invalidTimes;
    if (!p->writer) {
        size_t start = (size_t)(.5*p->rate);
        size_t count = p->samples > start ? p->samples-start : 0;
        double peak=0, maxJump=0, sumSquares=0;
        double idealJump=2*TONE_AMP*sin(PI*TONE_HZ/p->rate);
        size_t silent=0, crossings=0, discontinuities=0, nonfinite=0;
        for(size_t i=start;i<p->samples;++i) {
            double v=p->capture[i];
            if(!isfinite(v)) { ++nonfinite; continue; }
            if(fabs(v)>peak) peak=fabs(v);
            sumSquares+=v*v;
            if(fabs(v)<.00001) ++silent;
            if(i>start) {
                double diff=fabs(v-p->capture[i-1]);
                if(diff>maxJump) maxJump=diff;
                if(diff>idealJump*1.1) ++discontinuities;
                if(p->capture[i-1]<0 && v>=0) ++crossings;
            }
        }
        double hz=count ? crossings*p->rate/count : 0;
        double silence=count ? (double)silent/count : 1;
        double rms=count ? sqrt(sumSquares/count) : 0;
        /* A perfect 440Hz tone at 8kHz has exactly 1% zero-valued samples. */
        double silenceLimit = p->rate == 8000 ? .015 : .01;
        good = good && count>p->rate && !nonfinite && silence<silenceLimit &&
               fabs(hz-TONE_HZ)<1.0 && peak>.29 && peak<.31 && maxJump<idealJump*1.1 &&
               fabs(rms-TONE_AMP/sqrt(2))<.005;
        printf("Steady-state (first 0.5s ignored): frames=%zu Hz=%.4f peak=%.6f RMS=%.6f silence=%.4f%% maxJump=%.6f idealJump=%.6f abnormalJumps=%zu nonfinite=%zu\n",
               count,hz,peak,rms,100*silence,maxJump,idealJump,discontinuities,nonfinite);
    }
    printf("%s: %s\n", p->writer ? "Writer" : "Capture", good ? "PASS" : "FAIL");
    return good ? 0 : 1;
}
static int run(int writer, double seconds, UInt32 requested) {
    AudioDeviceID dev = lookup(writer ? WRITER_UID : MIC_UID);
    if (!dev) { fprintf(stderr, "Required 0.3.0 %s device missing.\n", writer ? "writer" : "microphone"); return 1; }
    AudioStreamBasicDescription fmt = {0};
    if (!deviceFormat(dev, writer ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput, &fmt) ||
        fmt.mFormatID != kAudioFormatLinearPCM || !(fmt.mFormatFlags & kAudioFormatFlagIsFloat) ||
        fmt.mBitsPerChannel != 32 || fmt.mSampleRate <= 0) {
        fprintf(stderr, "Unsupported device format (requires Float32 PCM).\n"); return 1;
    }
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    Boolean settable = false;
    if (!AudioObjectIsPropertySettable(dev, &a, &settable) && settable) {
        OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(requested), &requested);
        if (st) fprintf(stderr, "Buffer-size request %u rejected: %d (actual sizes will be reported)\n", requested, (int)st);
    }
    UInt32 actual = 0, bytes = sizeof(actual);
    get(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &bytes, &actual);
    Probe p = { .writer=writer, .rate=fmt.mSampleRate };
    if (!writer) {
        p.capacity = (size_t)(fmt.mSampleRate*(seconds+4));
        p.capture = calloc(p.capacity, sizeof(float));
        if (!p.capture) return 1;
    }
    AudioDeviceIOProcID proc = NULL;
    OSStatus st = AudioDeviceCreateIOProcID(dev, callback, &p, &proc);
    if (st) { fprintf(stderr, "CreateIOProc status=%d\n", (int)st); free(p.capture); return 1; }
    setUsage(dev, proc, kAudioObjectPropertyScopeInput, !writer);
    setUsage(dev, proc, kAudioObjectPropertyScopeOutput, writer);
    printf("%s device=%u rate=%.0f channels=%u requested=%u deviceBuffer=%u seconds=%.3f\n",
           writer ? "tone" : "record", dev, p.rate, fmt.mChannelsPerFrame, requested, actual, seconds);
    fflush(stdout);
    st = AudioDeviceStart(dev, proc);
    if (st) { fprintf(stderr, "Start status=%d\n", (int)st); AudioDeviceDestroyIOProcID(dev,proc); free(p.capture); return 1; }
    waitSeconds(seconds);
    OSStatus stop = AudioDeviceStop(dev, proc);
    OSStatus destroy = AudioDeviceDestroyIOProcID(dev, proc);
    int result = report(&p, stop, destroy);
    free(p.capture);
    return result;
}

#define AU_MAX_FRAMES 8192
typedef struct {
    Probe probe;
    AudioUnit unit;
    AudioBufferList list;
    float *renderBuffer;
    OSStatus renderError;
} AUProbe;
static OSStatus inputCallback(void *context, AudioUnitRenderActionFlags *flags,
                              const AudioTimeStamp *time, UInt32 bus, UInt32 frames,
                              AudioBufferList *unused) {
    (void)bus; (void)unused;
    AUProbe *a=context;
    if(frames>AU_MAX_FRAMES) { ++a->probe.badBuffers; return kAudioUnitErr_TooManyFramesToProcess; }
    a->list.mBuffers[0].mDataByteSize=frames*sizeof(float);
    OSStatus st=AudioUnitRender(a->unit,flags,time,1,frames,&a->list);
    if(st) { a->renderError=st; ++a->probe.badBuffers; return st; }
    noteFrames(&a->probe,frames,time);
    size_t available=a->probe.capacity-a->probe.samples;
    size_t n=frames<available?frames:available;
    memcpy(a->probe.capture+a->probe.samples,a->renderBuffer,n*sizeof(float));
    a->probe.samples+=n;
    a->probe.overflow+=frames-n;
    return noErr;
}
static int runAudioUnit(double seconds,double rate) {
    AudioDeviceID dev=lookup(MIC_UID);
    if(!dev) { fprintf(stderr,"Required virtual microphone UID missing.\n");return 1; }
    AUProbe a={.probe={.rate=rate}};
    a.probe.capacity=(size_t)(rate*(seconds+4));
    a.probe.capture=calloc(a.probe.capacity,sizeof(float));
    a.renderBuffer=calloc(AU_MAX_FRAMES,sizeof(float));
    if(!a.probe.capture||!a.renderBuffer) { free(a.probe.capture);free(a.renderBuffer);return 1; }
    AudioComponentDescription d={.componentType=kAudioUnitType_Output,.componentSubType=kAudioUnitSubType_HALOutput,.componentManufacturer=kAudioUnitManufacturer_Apple};
    AudioComponent c=AudioComponentFindNext(NULL,&d);
    OSStatus st=c?AudioComponentInstanceNew(c,&a.unit):-1;
    const char *stage="new AUHAL";
    int initialized=0,started=0,result=1;
    if(st) goto done;
    UInt32 on=1,off=0,maxFrames=AU_MAX_FRAMES;
#define AU_SET(label,prop,scope,element,value) do { stage=label; st=AudioUnitSetProperty(a.unit,prop,scope,element,&value,sizeof(value));if(st)goto done; } while(0)
    AU_SET("enable input",kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,on);
    AU_SET("disable output",kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,off);
    AU_SET("select device",kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,dev);
    AudioStreamBasicDescription format={.mSampleRate=rate,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,
        .mBytesPerPacket=4,.mFramesPerPacket=1,.mBytesPerFrame=4,.mChannelsPerFrame=1,.mBitsPerChannel=32};
    AU_SET("client format",kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,1,format);
    SInt32 channel=0;
    AU_SET("channel map",kAudioOutputUnitProperty_ChannelMap,kAudioUnitScope_Output,1,channel);
    AU_SET("max render frames",kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,maxFrames);
    AURenderCallbackStruct callbackInfo={.inputProc=inputCallback,.inputProcRefCon=&a};
    AU_SET("input callback",kAudioOutputUnitProperty_SetInputCallback,kAudioUnitScope_Global,1,callbackInfo);
#undef AU_SET
    a.list.mNumberBuffers=1;
    a.list.mBuffers[0]=(AudioBuffer){.mNumberChannels=1,.mDataByteSize=AU_MAX_FRAMES*sizeof(float),.mData=a.renderBuffer};
    stage="initialize";st=AudioUnitInitialize(a.unit);if(st)goto done;initialized=1;
    printf("record-au device=%u clientRate=%.0f clientChannels=1 seconds=%.3f\n",dev,rate,seconds);fflush(stdout);
    stage="start";st=AudioOutputUnitStart(a.unit);if(st)goto done;started=1;
    waitSeconds(seconds);
    st=AudioOutputUnitStop(a.unit);started=0;
    if(a.renderError)fprintf(stderr,"AU render error=%d\n",(int)a.renderError);
    result=report(&a.probe,st,a.renderError);
done:
    if(st)fprintf(stderr,"AUHAL stage=%s status=%d\n",stage,(int)st);
    if(started)AudioOutputUnitStop(a.unit);
    if(initialized)AudioUnitUninitialize(a.unit);
    if(a.unit)AudioComponentInstanceDispose(a.unit);
    free(a.probe.capture);free(a.renderBuffer);
    return result;
}


// AudioQueue supplies a higher-level capture/conversion path. AUHAL itself does
// not sample-rate-convert (Apple TN2091); its client rate must match the device.
typedef struct { Probe probe; OSStatus enqueueError; _Atomic bool stopping; } QueueProbe;
static void queueInput(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer,
                       const AudioTimeStamp *time, UInt32 packets,
                       const AudioStreamPacketDescription *descriptions) {
    (void)packets; (void)descriptions;
    QueueProbe *q=context;
    UInt32 frames=buffer->mAudioDataByteSize/sizeof(float);
    if(!frames)return; // AudioQueue stop/drain notifications may carry no PCM or timestamp.
    noteFrames(&q->probe,frames,time);
    size_t n=frames;
    if(n>q->probe.capacity-q->probe.samples)n=q->probe.capacity-q->probe.samples;
    memcpy(q->probe.capture+q->probe.samples,buffer->mAudioData,n*sizeof(float));
    q->probe.samples+=n;q->probe.overflow+=frames-n;
    if(!atomic_load(&q->stopping)) {
        OSStatus status=AudioQueueEnqueueBuffer(queue,buffer,0,NULL);
        if(status && !atomic_load(&q->stopping))q->enqueueError=status;
    }
}
static int runQueue(double seconds,double rate) {
    QueueProbe q={.probe={.rate=rate}};
    q.probe.capacity=(size_t)(rate*(seconds+4));
    q.probe.capture=calloc(q.probe.capacity,sizeof(float));
    if(!q.probe.capture)return 1;
    AudioStreamBasicDescription f={.mSampleRate=rate,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,
        .mBytesPerPacket=4,.mFramesPerPacket=1,.mBytesPerFrame=4,.mChannelsPerFrame=1,.mBitsPerChannel=32};
    AudioQueueRef queue=NULL;int result=1;const char *stage="create";
    OSStatus st=AudioQueueNewInput(&f,queueInput,&q,NULL,NULL,0,&queue);if(st)goto done;
    CFStringRef uid=MIC_UID;
    stage="select virtual device";st=AudioQueueSetProperty(queue,kAudioQueueProperty_CurrentDevice,&uid,sizeof(uid));if(st)goto done;
    for(unsigned i=0;i<4;++i){
        AudioQueueBufferRef b=NULL;stage="allocate";
        st=AudioQueueAllocateBuffer(queue,(UInt32)(rate*.02)*sizeof(float),&b);if(st)goto done;
        stage="enqueue";st=AudioQueueEnqueueBuffer(queue,b,0,NULL);if(st)goto done;
    }
    printf("record-queue clientRate=%.0f channels=1 requestedBuffer=20ms seconds=%.3f\n",rate,seconds);fflush(stdout);
    stage="start";st=AudioQueueStart(queue,NULL);if(st)goto done;
    waitSeconds(seconds);stage="stop";atomic_store(&q.stopping,true);st=AudioQueueStop(queue,true);
    result=report(&q.probe,st,q.enqueueError);
done:
    if(st)fprintf(stderr,"AudioQueue stage=%s status=%d\n",stage,(int)st);
    if(queue)AudioQueueDispose(queue,true);
    free(q.probe.capture);return result;
}

int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"inspect")) return inspect();
    if (argc==4 && (!strcmp(argv[1],"record-au") || !strcmp(argv[1],"record-queue"))) {
        double seconds=strtod(argv[2],NULL), rate=strtod(argv[3],NULL);
        if(!isfinite(seconds) || seconds<2 || seconds>3600 || !(rate==8000 || rate==16000 || rate==32000 || rate==44100 || rate==48000)) return 2;
        if (!strcmp(argv[1],"record-queue")) return runQueue(seconds,rate);
        if(rate!=48000){fprintf(stderr,"AUHAL requires the 48k device rate; use record-queue for SRC (Apple TN2091).\n");return 2;}
        return runAudioUnit(seconds,rate);
    }
    if (argc>=3 && argc<=4 && (!strcmp(argv[1],"tone") || !strcmp(argv[1],"record"))) {
        double seconds=strtod(argv[2],NULL);
        unsigned long frames=argc==4 ? strtoul(argv[3],NULL,10) : 512;
        if(!isfinite(seconds) || seconds<2 || seconds>3600 || frames<32 || frames>8192) {
            fprintf(stderr,"seconds must be 2..3600; frames 32..8192\n");return 2;
        }
        return run(!strcmp(argv[1],"tone"),seconds,(UInt32)frames);
    }
    fprintf(stderr,"Usage: %s inspect | tone seconds [bufferFrames] | record seconds [bufferFrames] | record-au seconds 48000 | record-queue seconds clientRate\n",argv[0]);
    return 2;
}
