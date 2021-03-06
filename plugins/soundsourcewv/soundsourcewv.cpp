#include <QFile>

#include "soundsourcewv.h"

#include "util/logger.h"

namespace mixxx {

namespace {

const Logger kLogger("SoundSourceWV");

} // anonymous namespace

//static
WavpackStreamReader SoundSourceWV::s_streamReader = {
    SoundSourceWV::ReadBytesCallback,
    SoundSourceWV::GetPosCallback,
    SoundSourceWV::SetPosAbsCallback,
    SoundSourceWV::SetPosRelCallback,
    SoundSourceWV::PushBackByteCallback,
    SoundSourceWV::GetlengthCallback,
    SoundSourceWV::CanSeekCallback,
    SoundSourceWV::WriteBytesCallback
};

SoundSourceWV::SoundSourceWV(const QUrl& url)
        : SoundSourcePlugin(url, "wv"),
          m_wpc(nullptr),
          m_sampleScaleFactor(CSAMPLE_ZERO), 
          m_pWVFile(nullptr),
          m_pWVCFile(nullptr),
          m_curFrameIndex(0) {
}

SoundSourceWV::~SoundSourceWV() {
    close();
}

SoundSource::OpenResult SoundSourceWV::tryOpen(
        OpenMode /*mode*/,
        const OpenParams& params) {
    DEBUG_ASSERT(!m_wpc);
    char msg[80]; // hold possible error message
    int openFlags = OPEN_WVC | OPEN_NORMALIZE;
    if ((params.channelCount() == 1) ||
            (params.channelCount() == 2)) {
        openFlags |= OPEN_2CH_MAX;
    }

    // We use WavpackOpenFileInputEx to support Unicode paths on windows
    // http://www.wavpack.com/lib_use.txt
    QString wavPackFileName = getLocalFileName();
    m_pWVFile = new QFile(wavPackFileName);
    m_pWVFile->open(QFile::ReadOnly);
    QString correctionFileName(wavPackFileName + "c");
    if (QFile::exists(correctionFileName)) {
        // If there is a correction file, open it as well
        m_pWVCFile = new QFile(correctionFileName);
        m_pWVCFile->open(QFile::ReadOnly);
    }
    m_wpc = WavpackOpenFileInputEx(&s_streamReader, m_pWVFile, m_pWVCFile,
            msg, openFlags, 0);
    if (!m_wpc) {
        kLogger.debug() << "failed to open file : " << msg;
        return OpenResult::Failed;
    }

    setChannelCount(WavpackGetReducedChannels(m_wpc));
    setSampleRate(WavpackGetSampleRate(m_wpc));
    initFrameIndexRangeOnce(
            mixxx::IndexRange::forward(
                    0,
                    WavpackGetNumSamples(m_wpc)));

    if (WavpackGetMode(m_wpc) & MODE_FLOAT) {
        m_sampleScaleFactor = CSAMPLE_PEAK;
    } else {
        const int bitsPerSample = WavpackGetBitsPerSample(m_wpc);
        const uint32_t wavpackPeakSampleValue = 1u
                << (bitsPerSample - 1);
        m_sampleScaleFactor = CSAMPLE_PEAK / wavpackPeakSampleValue;
    }

    m_curFrameIndex = frameIndexMin();

    return OpenResult::Succeeded;
}

void SoundSourceWV::close() {
    if (m_wpc) {
        WavpackCloseFile(m_wpc);
        m_wpc = nullptr;
    }
    if (m_pWVFile) {
        m_pWVFile->close();
        delete m_pWVFile;
        m_pWVFile = nullptr;
    }
    if (m_pWVCFile) {
        m_pWVCFile->close();
        delete m_pWVCFile;
        m_pWVCFile = nullptr;
    }
}

ReadableSampleFrames SoundSourceWV::readSampleFramesClamped(
        WritableSampleFrames writableSampleFrames) {

    const SINT firstFrameIndex = writableSampleFrames.frameIndexRange().start();

    if (m_curFrameIndex != firstFrameIndex) {
        if (WavpackSeekSample(m_wpc, firstFrameIndex)) {
            m_curFrameIndex = firstFrameIndex;
        } else {
            kLogger.warning()
                    << "Could not seek to first frame index"
                    << firstFrameIndex;
            m_curFrameIndex = WavpackGetSampleIndex(m_wpc);
            return ReadableSampleFrames(IndexRange::between(m_curFrameIndex, m_curFrameIndex));
        }
    }
    DEBUG_ASSERT(m_curFrameIndex == firstFrameIndex);

    const SINT numberOfFramesTotal = writableSampleFrames.frameLength();

    static_assert(sizeof(CSAMPLE) == sizeof(int32_t),
            "CSAMPLE and int32_t must have the same size");
    CSAMPLE* pOutputBuffer = writableSampleFrames.writableData();
    SINT unpackCount = WavpackUnpackSamples(m_wpc,
            reinterpret_cast<int32_t*>(pOutputBuffer), numberOfFramesTotal);
    DEBUG_ASSERT(unpackCount >= 0);
    DEBUG_ASSERT(unpackCount <= numberOfFramesTotal);
    if (!(WavpackGetMode(m_wpc) & MODE_FLOAT)) {
        // signed integer -> float
        const SINT sampleCount = frames2samples(unpackCount);
        for (SINT i = 0; i < sampleCount; ++i) {
            const int32_t sampleValue =
                    *reinterpret_cast<int32_t*>(pOutputBuffer);
            *pOutputBuffer++ = CSAMPLE(sampleValue) * m_sampleScaleFactor;
        }
    }
    const auto resultRange = IndexRange::forward(m_curFrameIndex, unpackCount);
    m_curFrameIndex += unpackCount;
    return ReadableSampleFrames(
            resultRange,
            SampleBuffer::ReadableSlice(
                    writableSampleFrames.writableData(),
                    frames2samples(unpackCount)));
}

QString SoundSourceProviderWV::getName() const {
    return "WavPack";
}

QStringList SoundSourceProviderWV::getSupportedFileExtensions() const {
    QStringList supportedFileExtensions;
    supportedFileExtensions.append("wv");
    return supportedFileExtensions;
}

SoundSourcePointer SoundSourceProviderWV::newSoundSource(const QUrl& url) {
    return newSoundSourcePluginFromUrl<SoundSourceWV>(url);
}

//static
int32_t SoundSourceWV::ReadBytesCallback(void* id, void* data, int bcount)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    return pFile->read((char*)data, bcount);
}


// static
uint32_t SoundSourceWV::GetPosCallback(void *id)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    return pFile->pos();
}

//static
int SoundSourceWV::SetPosAbsCallback(void* id, unsigned int pos)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    return pFile->seek(pos) ? 0 : -1;
}

//static
int SoundSourceWV::SetPosRelCallback(void *id, int delta, int mode)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }

    switch(mode) {
    case SEEK_SET:
        return pFile->seek(delta) ? 0 : -1;
    case SEEK_CUR:
        return pFile->seek(pFile->pos() + delta) ? 0 : -1;
    case SEEK_END:
        return pFile->seek(pFile->size() + delta) ? 0 : -1;
    default:
        return -1;
    }
}

//static
int SoundSourceWV::PushBackByteCallback(void* id, int c)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    pFile->ungetChar((char)c);
    return 1;
}

//static
uint32_t SoundSourceWV::GetlengthCallback(void* id)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    return pFile->size();
}

//static
int SoundSourceWV::CanSeekCallback(void *id)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    return pFile->isSequential() ? 0 : 1;
}

//static
int32_t SoundSourceWV::WriteBytesCallback(void* id, void* data, int32_t bcount)
{
    QFile* pFile = static_cast<QFile*>(id);
    if (!pFile) {
        return 0;
    }
    return (int32_t)pFile->write((char*)data, bcount);
}

} // namespace mixxx

extern "C" MIXXX_SOUNDSOURCEPLUGINAPI_EXPORT
mixxx::SoundSourceProvider* Mixxx_SoundSourcePluginAPI_createSoundSourceProvider() {
    // SoundSourceProviderWV is stateless and a single instance
    // can safely be shared
    static mixxx::SoundSourceProviderWV singleton;
    return &singleton;
}

extern "C" MIXXX_SOUNDSOURCEPLUGINAPI_EXPORT
void Mixxx_SoundSourcePluginAPI_destroySoundSourceProvider(mixxx::SoundSourceProvider*) {
    // The statically allocated instance must not be deleted!
}
