
// Copyright (c) 2013 Nick Porcino, All rights reserved.
// License is MIT: http://opensource.org/licenses/MIT

#include "SpectralMonitorNode.h"
#include "LabSound.h"

#include "RecorderNode.h"

#include "AudioBus.h"
#include "AudioNodeInput.h"
#include "AudioNodeOutput.h"
#include "ExceptionCode.h"
#include "WindowFunctions.h"
#include "../ooura/fftsg.h"

namespace LabSound {

    using namespace WebCore;
    using namespace cinder::audio2::dsp;

    // FFT class directly inspired by that in Cinder Audio 2.
    class FFT {
    public:
        FFT(int size)
        : size(size) {
            oouraIp = (int *)calloc( 2 + (int)sqrt( size/2 ), sizeof( int ) );
            oouraW = (float *)calloc( size/2, sizeof( float ) );
        }
        ~FFT() {
            free(oouraIp);
            free(oouraW);
        }

        // does an in place transform of waveform to real and imag.
        // real values are on even, imag on odd
        void forward( std::vector<float>& waveform) {
            assert(waveform.size() == size);
            ooura::rdft( size, 1, &waveform[0], oouraIp, oouraW );
        }

#if 0
        void inverse( const BufferSpectral *spectral, Buffer *waveform )
        {
            CI_ASSERT( waveform->getNumFrames() == mSize );
            CI_ASSERT( spectral->getNumFrames() == mSizeOverTwo );

            mBufferCopy.copyFrom( *spectral );

            float *real = mBufferCopy.getData();
            float *imag = &mBufferCopy.getData()[mSizeOverTwo];
            float *a = waveform->getData();
            
            a[0] = real[0];
            a[1] = imag[0];
            
            for( size_t k = 1; k < mSizeOverTwo; k++ ) {
                a[k * 2] = real[k];
                a[k * 2 + 1] = imag[k];
            }
            
            ooura::rdft( (int)mSize, -1, a, mOouraIp, mOouraW );
            dsp::mul( a, 2.0f / (float)mSize, a, mSize );
        }
#endif

        int size;
        int* oouraIp;
        float* oouraW;
    };

    class SpectralMonitorNode::Detail {
    public:
        Detail()
        : fft(0) {
            setWindowSize(512);
        }
        ~Detail() {
            delete fft;
        }

        void setWindowSize(int s) {
            cursor = 0;
            windowSize = s;
            buffer.resize(windowSize);
            for (int i = 0; i < windowSize; ++i) {
                buffer[i] = 0;
            }
            delete fft;
            fft = new FFT(s);
        }
        
        float _db;
        size_t windowSize;
        int cursor;
        std::vector<float> buffer;
        std::mutex magMutex;
        FFT* fft;
    };

    SpectralMonitorNode::SpectralMonitorNode(AudioContext* context, float sampleRate)
    : AudioBasicInspectorNode(context, sampleRate)
    , detail(new Detail())
    {
        addInput(adoptPtr(new AudioNodeInput(this)));
        setNodeType((AudioNode::NodeType) NodeTypeSpectralMonitor);
        initialize();
    }

    SpectralMonitorNode::~SpectralMonitorNode()
    {
        uninitialize();
        delete detail;
    }

    void SpectralMonitorNode::process(size_t framesToProcess)
    {
        // deal with the output in case the power monitor node is embedded in a signal chain for some reason.
        // It's merely a pass through though.

        AudioBus* outputBus = output(0)->bus();

        if (!isInitialized() || !input(0)->isConnected()) {
            if (outputBus)
                outputBus->zero();
            return;
        }

        AudioBus* bus = input(0)->bus();
        bool isBusGood = bus && bus->numberOfChannels() > 0 && bus->channel(0)->length() >= framesToProcess;
        if (!isBusGood) {
            outputBus->zero();
            return;
        }

        // specific to this node
        {
            std::vector<const float*> channels;
            unsigned numberOfChannels = bus->numberOfChannels();
            for (int c = 0; c < numberOfChannels; ++ c)
                channels.push_back(bus->channel(c)->data());

            // if the fft is smaller than the quantum, just grab a chunk
            if (detail->windowSize < framesToProcess) {
                detail->cursor = 0;
                framesToProcess = detail->windowSize;
            }

            // if the quantum overlaps the end of the window, just fill up the buffer
            if (detail->cursor + framesToProcess > detail->windowSize)
                framesToProcess = detail->windowSize - detail->cursor;

            {
                std::lock_guard<std::mutex> lock(detail->magMutex);

                detail->buffer.resize(detail->windowSize);
                for (int i = 0; i < framesToProcess; ++i) {
                    detail->buffer[i + detail->cursor] = 0;
                }
                for (int c = 0; c < numberOfChannels; ++c)
                    for (int i = 0; i < framesToProcess; ++i) {
                        float p = channels[c][i];
                        detail->buffer[i + detail->cursor] += p;
                    }
            }

            // advance the cursor
            detail->cursor += framesToProcess;
            if (detail->cursor >= detail->windowSize)
                detail->cursor = 0;
        }
        // to here

        // For in-place processing, our override of pullInputs() will just pass the audio data
        // through unchanged if the channel count matches from input to output
        // (resulting in inputBus == outputBus). Otherwise, do an up-mix to stereo.
        //
        if (bus != outputBus)
            outputBus->copyFrom(*bus);
    }

    void SpectralMonitorNode::reset()
    {
        detail->setWindowSize(detail->windowSize);
    }

    void SpectralMonitorNode::spectralMag(std::vector<float>& result) {
        std::vector<float> window;
        {
            std::lock_guard<std::mutex> lock(detail->magMutex);
            window.swap(detail->buffer);
            detail->setWindowSize(detail->windowSize);
        }
        // http://www.ni.com/white-paper/4844/en/
        applyWindow(LabSound::window_blackman, window);
        detail->fft->forward(window);

        // similar to cinder audio2 Scope object, although Scope smooths spectral samples frame by frame
        // remove nyquist component - the first imaginary component
        window[1] = 0.0f;

        // compute normalized magnitude spectrum
        // TODO: break this into vector cartisian -> polar and then vector lowpass. skip lowpass if smoothing factor is very small
        const float kMagScale = 1.0f ;/// detail->windowSize;
        for(size_t i = 0; i < window.size(); i += 2) {
            float re = window[i];
            float im = window[i+1];
            window[i/2] = sqrt(re * re + im * im) * kMagScale;
        }

        result.swap(window);
    }

    void SpectralMonitorNode::windowSize(size_t ws) {
        detail->setWindowSize(ws);
    }

    size_t SpectralMonitorNode::windowSize() const {
        return detail->windowSize;
    }


} // namespace LabSound