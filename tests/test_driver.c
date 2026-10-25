// Offline driver contract/timeline tests. Calls the real driver implementation.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mach/mach_time.h>
#include "../src/CamMicLinkDriver.c"
static uint64_t testNow=1000000;
uint64_t CMLTestHostTime(void) { return testNow; }
static void atFrame(int64_t n) {
    mach_timebase_info_data_t t; mach_timebase_info(&t);
    testNow=1000000+(uint64_t)((double)n*1e9*t.denom/t.numer/48000.0);
}
static float source(int64_t frame) {
    // Includes an exact digital-silence segment; silence must advance time normally.
    if (frame>=16000 && frame<19000) return 0;
    return (float)(.3*sin(2*3.141592653589793*440*(double)frame/48000));
}
static void timeline(unsigned w, unsigned r1, unsigned r2) {
    atFrame(0); CMLInitialize(); assert(CMLStartIO(4,17)==0);
    assert(CMLStartIO(4,18)!=0); // no heuristic selection or zero-block writers
    float write[CML_MAX_IO_FRAMES*2], read[CML_MAX_IO_FRAMES*2];
    int64_t nextW=0,nextR1=0,nextR2=3000;
    unsigned reads=0;
    for (;;) {
        int64_t now=nextW<nextR1?nextW:nextR1;
        if(r2 && nextR2<now)now=nextR2;
        if(now>250000)break;
        atFrame(now);
        // Read before same-time write. Input timestamps already include latency.
        for(unsigned reader=0;reader<2;++reader) {
            unsigned frames=reader?r2:r1;
            int64_t *next=reader?&nextR2:&nextR1;
            if(!frames || *next!=now)continue;
            assert(CMLStartIO(2,100+reader)==0);
            int64_t first=now-frames-CML_DELAY_FRAMES;
            CMLRead(read,frames,(double)first);
            for(unsigned i=0;i<frames;++i) {
                int64_t sample=first+i;
                if(sample<0) { assert(read[i*2]==0 && read[i*2+1]==0); }
                else {
                    if(read[i*2]!=source(sample) || read[i*2+1]!=(float)(sample+1)) {
                        fprintf(stderr,"mismatch W%u R%u at %lld expected %.6f/%.0f got %.6f/%.0f\n",w,frames,sample,source(sample),(float)(sample+1),read[i*2],read[i*2+1]);abort();
                    }
                }
            }
            CMLStopIO(2,100+reader); // other readers must not reset the ring
            *next+=frames; ++reads;
        }
        if(nextW==now) {
            for(unsigned i=0;i<w;++i){write[i*2]=source(now+i);write[i*2+1]=(float)(now+i+1);}
            CMLWrite(write,w,(double)now,17);
            nextW+=w;
        }
    }
    CMLStopIO(4,17);
    CMLRead(read,512,300000); // requests beyond last supplied sample become silence
    for(unsigned i=0;i<1024;++i)assert(read[i]==0);
    assert(CMLStartIO(4,18)==0);
    memset(write,0,sizeof(write));
    CMLWrite(write,512,300000,18);
    CMLRead(read,512,300000);
    for(unsigned i=0;i<1024;++i)assert(read[i]==0);
    CMLRead(read,512,0); // overwritten old ring time must never be replayed
    for(unsigned i=0;i<1024;++i)assert(read[i]==0);
    CMLRead(read,512,NAN);for(unsigned i=0;i<1024;++i)assert(read[i]==0);
    printf("timeline W=%u readers=%u,%u: %u blocks, exact sample match PASS\n",w,r1,r2,reads);
}
static void properties(void) {
    const UInt32 selectors[]={kAudioObjectPropertyBaseClass,kAudioObjectPropertyClass,kAudioObjectPropertyOwner,kAudioObjectPropertyName,kAudioObjectPropertyManufacturer,kAudioObjectPropertyOwnedObjects,kAudioPlugInPropertyBundleID,kAudioPlugInPropertyDeviceList,kAudioPlugInPropertyBoxList,kAudioPlugInPropertyResourceBundle,kAudioBoxPropertyBoxUID,kAudioBoxPropertyAcquired,kAudioBoxPropertyDeviceList,kAudioBoxPropertyClockDeviceList,kAudioDevicePropertyDeviceUID,kAudioDevicePropertyModelUID,kAudioDevicePropertyIsHidden,kAudioDevicePropertyStreams,kAudioDevicePropertyLatency,kAudioDevicePropertySafetyOffset,kAudioDevicePropertyNominalSampleRate,kAudioDevicePropertyAvailableNominalSampleRates,kAudioDevicePropertyPreferredChannelLayout,kAudioDevicePropertyPreferredChannelsForStereo,kAudioDevicePropertyRelatedDevices,kAudioDevicePropertyDeviceCanBeDefaultDevice,kAudioObjectPropertyControlList,kAudioStreamPropertyVirtualFormat,kAudioStreamPropertyAvailableVirtualFormats,kAudioStreamPropertyIsActive,kAudioStreamPropertyDirection};
    UInt32 scopes[]={kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput};
    unsigned checked=0;
    for(unsigned id=1;id<=6;++id)for(unsigned si=0;si<sizeof(selectors)/sizeof(*selectors);++si)for(unsigned sc=0;sc<3;++sc) {
        AudioObjectPropertyAddress a={selectors[si],scopes[sc],0};UInt32 size=0,written=0;
        OSStatus st=getSize(gDriver,id,0,&a,0,NULL,&size);
        assert(hasProperty(gDriver,id,0,&a)==(st==0));if(st)continue;
        unsigned char *storage=calloc(1,size+16);memset(storage+size,0xA7,16);
        assert(getData(gDriver,id,0,&a,0,NULL,size,&written,storage)==0);assert(written==size);
        for(unsigned i=0;i<16;++i)assert(storage[size+i]==0xA7);
        if(a.mSelector==kAudioObjectPropertyName || a.mSelector==kAudioObjectPropertyManufacturer || a.mSelector==kAudioPlugInPropertyBundleID || a.mSelector==kAudioPlugInPropertyResourceBundle || a.mSelector==kAudioBoxPropertyBoxUID || a.mSelector==kAudioDevicePropertyDeviceUID || a.mSelector==kAudioDevicePropertyModelUID)CFRelease(*(CFStringRef*)storage);
        free(storage);++checked;
    }
    AudioObjectPropertyAddress a={kAudioDevicePropertyStreams,kAudioObjectPropertyScopeOutput,0};UInt32 size;
    assert(getSize(gDriver,2,0,&a,0,NULL,&size)==0 && size==0);
    a.mScope=kAudioObjectPropertyScopeInput;
    assert(getSize(gDriver,4,0,&a,0,NULL,&size)==0 && size==0);
    Boolean will,inPlace;
    willDo(gDriver,2,1,kAudioServerPlugInIOOperationWriteMix,&will,&inPlace);assert(!will);
    willDo(gDriver,4,1,kAudioServerPlugInIOOperationReadInput,&will,&inPlace);assert(!will);
    a=(AudioObjectPropertyAddress){kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,0};
    double rate=44100;assert(setData(gDriver,2,0,&a,0,NULL,sizeof(rate),&rate)!=0);
    rate=48000;assert(setData(gDriver,2,0,&a,0,NULL,sizeof(rate),&rate)==0);
    printf("properties: %u exact-size payloads, direction and rate contracts PASS\n",checked);
}
int main(void) {
    initialize(gDriver,NULL);properties();
    unsigned cases[][3]={{1024,1024,0},{512,512,0},{1024,512,0},{1024,256,0},{512,1024,0},{1024,480,0},{1024,441,0},{1024,480,1024},{1024,480,512},{2048,128,441},{64,2048,256}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)timeline(cases[i][0],cases[i][1],cases[i][2]);
    atFrame(0);CMLInitialize();atFrame(48000*600);
    double sample;uint64_t host,seed;
    CMLGetZeroTimeStamp(&sample,&host,&seed);
    assert(fabs(sample-48000*600)<CML_PERIOD_FRAMES+1 && seed==1);
    puts("shared clock jumps forward ten minutes correctly PASS");
    puts("ALL TESTS PASSED");
}
