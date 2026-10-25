// CamMicLink 0.3.0: shared device clock and timestamp-addressed PCM storage.
#ifndef CML_AUDIO_CORE_H
#define CML_AUDIO_CORE_H
#include <stdint.h>
#define CML_SAMPLE_RATE 48000.0
#define CML_CHANNELS 2u
#define CML_PERIOD_FRAMES 512u
#define CML_DELAY_FRAMES 2048u
#define CML_RING_FRAMES 65536u
#define CML_MAX_IO_FRAMES 8192u
#define CML_INPUT_DEVICE 2u
#define CML_WRITER_DEVICE 4u
void CMLInitialize(void);
int CMLStartIO(uint32_t device, uint32_t client);
void CMLStopIO(uint32_t device, uint32_t client);
void CMLGetZeroTimeStamp(double *sample, uint64_t *host, uint64_t *seed);
void CMLRead(float *dst, uint32_t frames, double sampleTime);
void CMLWrite(const float *src, uint32_t frames, double sampleTime, uint32_t client);
#endif
