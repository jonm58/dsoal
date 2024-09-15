#include "primarybuffer.h"

#include "buffer.h"
#include "dsoal.h"
#include "dsoundoal.h"
#include "guidprinter.h"
#include "logging.h"


namespace {

using voidp = void*;
using cvoidp = const void*;

/* The primary buffer has a fixed size, apprently. */
constexpr size_t PrimaryBufSize{32768};

} // namespace

PrimaryBuffer::PrimaryBuffer(DSound8OAL &parent) : mParent{parent}, mMutex{parent.getMutex()}
{
    /* Make sure the format is valid, store 16-bit stereo 44.1khz by default. */
    mFormat.Format.wFormatTag = WAVE_FORMAT_PCM;
    mFormat.Format.nChannels = 2;
    mFormat.Format.nSamplesPerSec = 44100;
    mFormat.Format.nAvgBytesPerSec = 44100 * 4;
    mFormat.Format.nBlockAlign = 4;
    mFormat.Format.wBitsPerSample = 16;
    mFormat.Format.cbSize = 0;
}

PrimaryBuffer::~PrimaryBuffer() = default;

#define PREFIX "Primary::"
auto PrimaryBuffer::createWriteEmu(DWORD flags) noexcept -> HRESULT
{
    auto emudesc = DSBUFFERDESC{};
    emudesc.dwSize = sizeof(emudesc);
    emudesc.dwFlags = DSBCAPS_LOCHARDWARE | (flags&DSBCAPS_CTRLPAN);
    emudesc.dwBufferBytes = PrimaryBufSize - (PrimaryBufSize%mFormat.Format.nBlockAlign);
    emudesc.lpwfxFormat = &mFormat.Format;

    auto emu = ComPtr{new(std::nothrow) Buffer{mParent, false, nullptr}};
    if(!emu) return DSERR_OUTOFMEMORY;

    if(auto hr = HRESULT{emu->Initialize(mParent.as<IDirectSound*>(), &emudesc)}; FAILED(hr))
        return hr;

    return emu->QueryInterface(IID_IDirectSoundBuffer, ds::out_ptr(mWriteEmu));
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::QueryInterface(REFIID riid, void** ppvObject) noexcept
{
    DEBUG(PREFIX "QueryInterface (%p)->(%s, %p)\n", voidp{this}, IidPrinter{riid}.c_str(),
        voidp{ppvObject});

    *ppvObject = nullptr;
    if(riid == IID_IUnknown)
    {
        AddRef();
        *ppvObject = as<IUnknown*>();
        return S_OK;
    }
    if(riid == IID_IDirectSoundBuffer)
    {
        AddRef();
        *ppvObject = as<IDirectSoundBuffer*>();
        return S_OK;
    }
    if(riid == IID_IDirectSound3DListener)
    {
        mListener3D.AddRef();
        *ppvObject = mListener3D.as<IDirectSound3DListener*>();
        return S_OK;
    }

    FIXME(PREFIX "QueryInterface Unhandled GUID: %s\n", IidPrinter{riid}.c_str());
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE PrimaryBuffer::AddRef() noexcept
{
    const auto prev = mTotalRef.fetch_add(1u, std::memory_order_relaxed);
    const auto ret = mDsRef.fetch_add(1u, std::memory_order_relaxed) + 1;
    DEBUG(PREFIX "AddRef (%p) ref %lu\n", voidp{this}, ret);

    /* Clear the flags when getting the first reference, so it can be
     * reinitialized.
     */
    if(prev == 0)
        mFlags = 0;

    return ret;
}

ULONG STDMETHODCALLTYPE PrimaryBuffer::Release() noexcept
{
    /* NOTE: Some buggy apps try to release after hitting 0 references, so
     * prevent underflowing the reference counter.
     */
    ULONG ret{mDsRef.load(std::memory_order_relaxed)};
    do {
        if(ret == 0) UNLIKELY
        {
            WARN(PREFIX "Release (%p) ref already %lu\n", voidp{this}, ret);
            return ret;
        }
    } while(!mDsRef.compare_exchange_weak(ret, ret-1, std::memory_order_relaxed));
    ret -= 1;
    DEBUG(PREFIX "Release (%p) ref %lu\n", voidp{this}, ret);

    /* The primary buffer is a static object and should not be deleted. */
    mTotalRef.fetch_sub(1u, std::memory_order_relaxed);
    return ret;
}


HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetCaps(DSBCAPS *bufferCaps) noexcept
{
    DEBUG(PREFIX "GetCaps (%p)->(%p)\n", voidp{this}, voidp{bufferCaps});

    if(!bufferCaps || bufferCaps->dwSize < sizeof(*bufferCaps))
    {
        WARN(PREFIX "Invalid DSBCAPS (%p, %lu)\n", voidp{bufferCaps},
            bufferCaps ? bufferCaps->dwSize : 0lu);
        return DSERR_INVALIDPARAM;
    }

    bufferCaps->dwFlags = mFlags;
    bufferCaps->dwBufferBytes = PrimaryBufSize;
    bufferCaps->dwUnlockTransferRate = 0;
    bufferCaps->dwPlayCpuOverhead = 0;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetCurrentPosition(DWORD *playCursor, DWORD *writeCursor) noexcept
{
    DEBUG(PREFIX "GetCurrentPosition (%p)->(%p, %p)\n", voidp{this}, voidp{playCursor},
        voidp{writeCursor});

    std::lock_guard lock{mMutex};
    if(mWriteEmu)
        return mWriteEmu->GetCurrentPosition(playCursor, writeCursor);
    return DSERR_PRIOLEVELNEEDED;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetFormat(WAVEFORMATEX *wfx, DWORD sizeAllocated, DWORD *sizeWritten) noexcept
{
    DEBUG(PREFIX "GetFormat (%p)->(%p, %lu, %p)\n", voidp{this}, voidp{wfx}, sizeAllocated,
        voidp{sizeWritten});

    if(!wfx && !sizeWritten)
    {
        WARN(PREFIX "GetFormat Cannot report format or format size\n");
        return DSERR_INVALIDPARAM;
    }

    std::lock_guard lock{mMutex};
    const DWORD size{sizeof(mFormat.Format) + mFormat.Format.cbSize};
    if(sizeWritten)
        *sizeWritten = size;
    if(wfx)
    {
        if(sizeAllocated < size)
            return DSERR_INVALIDPARAM;
        std::memcpy(wfx, &mFormat.Format, size);
    }

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetVolume(LONG *volume) noexcept
{
    DEBUG(PREFIX "GetVolume (%p)->(%p)\n", voidp{this}, voidp{volume});

    if(!volume)
        return DSERR_INVALIDPARAM;

    if(!(mFlags&DSBCAPS_CTRLVOLUME))
        return DSERR_CONTROLUNAVAIL;

    std::lock_guard lock{mMutex};
    *volume = mVolume;
    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetPan(LONG *pan) noexcept
{
    DEBUG(PREFIX "GetPan (%p)->(%p)\n", voidp{this}, voidp{pan});

    if(!pan)
        return DSERR_INVALIDPARAM;

    if(!(mFlags&DSBCAPS_CTRLPAN))
        return DSERR_CONTROLUNAVAIL;

    std::lock_guard lock{mMutex};
    *pan = mPan;
    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetFrequency(DWORD *frequency) noexcept
{
    DEBUG(PREFIX "GetFrequency (%p)->(%p)\n", voidp{this}, voidp{frequency});

    if(!frequency)
        return DSERR_INVALIDPARAM;

    if(!(mFlags&DSBCAPS_CTRLFREQUENCY))
        return DSERR_CONTROLUNAVAIL;

    std::lock_guard lock{mMutex};
    *frequency = mFormat.Format.nSamplesPerSec;
    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::GetStatus(DWORD *status) noexcept
{
    DEBUG(PREFIX "GetStatus (%p)->(%p)\n", voidp{this}, voidp{status});

    if(!status)
        return DSERR_INVALIDPARAM;

    std::lock_guard lock{mMutex};
    bool playing{mPlaying};
    if(!playing && mParent.getPriorityLevel() < DSSCL_WRITEPRIMARY)
    {
        ALSection alsection{mContext};
        for(auto &group : mParent.getSecondaryBuffers())
        {
            uint64_t usemask{~group.mFreeMask};
            while(usemask)
            {
                auto idx = static_cast<unsigned int>(ds::countr_zero(usemask));
                usemask &= ~(1_u64 << idx);
                Buffer &buffer = (*group.mBuffers)[idx];

                if(const ALuint source{buffer.getSource()})
                {
                    ALint state{};
                    alGetSourcei(source, AL_SOURCE_STATE, &state);
                    playing = (state == AL_PLAYING);
                    if(playing) break;
                }
            }
            if(playing)
                break;
        }
    }

    if(playing)
    {
        *status = DSBSTATUS_PLAYING|DSBSTATUS_LOOPING;
        if((mFlags&DSBCAPS_LOCDEFER))
            *status |= DSBSTATUS_LOCHARDWARE;
    }
    else
        *status = 0;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Initialize(IDirectSound *directSound, const DSBUFFERDESC *dsBufferDesc) noexcept
{
    DEBUG(PREFIX "Initialize (%p)->(%p, %p)\n", voidp{this}, voidp{directSound},
        cvoidp{dsBufferDesc});

    if(!dsBufferDesc || dsBufferDesc->lpwfxFormat || dsBufferDesc->dwBufferBytes)
    {
        WARN(PREFIX "Initialize Bad DSBUFFERDESC\n");
        return DSERR_INVALIDPARAM;
    }

    static constexpr DWORD BadFlags{DSBCAPS_CTRLFX | DSBCAPS_CTRLPOSITIONNOTIFY
        | DSBCAPS_LOCSOFTWARE};
    if((dsBufferDesc->dwFlags&BadFlags))
    {
        WARN(PREFIX "Bad dwFlags %08lx\n", dsBufferDesc->dwFlags);
        return DSERR_INVALIDPARAM;
    }

    std::lock_guard lock{mMutex};
    if(mFlags != 0)
        return DSERR_ALREADYINITIALIZED;

    if(mParent.getPriorityLevel() == DSSCL_WRITEPRIMARY)
    {
        mWriteEmu = nullptr;
        if(auto hr = createWriteEmu(dsBufferDesc->dwFlags); FAILED(hr))
            return hr;
    }

    mFlags = dsBufferDesc->dwFlags | DSBCAPS_LOCHARDWARE;

    mImmediate.dwSize = sizeof(mImmediate);
    mImmediate.vPosition.x = 0.0f;
    mImmediate.vPosition.y = 0.0f;
    mImmediate.vPosition.z = 0.0f;
    mImmediate.vVelocity.x = 0.0f;
    mImmediate.vVelocity.y = 0.0f;
    mImmediate.vVelocity.z = 0.0f;
    mImmediate.vOrientFront.x = 0.0f;
    mImmediate.vOrientFront.y = 0.0f;
    mImmediate.vOrientFront.z = 1.0f;
    mImmediate.vOrientTop.x = 0.0f;
    mImmediate.vOrientTop.y = 1.0f;
    mImmediate.vOrientTop.z = 0.0f;
    mImmediate.flDistanceFactor = DS3D_DEFAULTDISTANCEFACTOR;
    mImmediate.flRolloffFactor = DS3D_DEFAULTROLLOFFFACTOR;
    mImmediate.flDopplerFactor = DS3D_DEFAULTDOPPLERFACTOR;
    mDeferred = mImmediate;
    mDirty.reset();

    ALSection alsection{mContext};
    setParams(mDeferred, ~0llu);

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Lock(DWORD offset, DWORD bytes, void **audioPtr1, DWORD *audioBytes1, void **audioPtr2, DWORD *audioBytes2, DWORD flags) noexcept
{
    DEBUG(PREFIX "Lock (%p)->(%lu, %lu, %p, %p, %p, %p, %lu)\n", voidp{this}, offset, bytes,
        voidp{audioPtr1}, voidp{audioBytes1}, voidp{audioPtr2}, voidp{audioBytes2}, flags);

    std::lock_guard lock{mMutex};
    if(mWriteEmu)
        return mWriteEmu->Lock(offset, bytes, audioPtr1, audioBytes1, audioPtr2, audioBytes2, flags);
    return DSERR_PRIOLEVELNEEDED;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Play(DWORD reserved1, DWORD reserved2, DWORD flags) noexcept
{
    DEBUG(PREFIX "Play (%p)->(%lu, %lu, %lu)\n", voidp{this}, reserved1, reserved2, flags);

    if(!(flags & DSBPLAY_LOOPING))
    {
        WARN(PREFIX "Play Flags (%08lx) not set to DSBPLAY_LOOPING\n", flags);
        return DSERR_INVALIDPARAM;
    }

    std::lock_guard lock{mMutex};
    auto hr = HRESULT{S_OK};
    if(mWriteEmu)
        hr = mWriteEmu->Play(reserved1, reserved2, flags);

    if(SUCCEEDED(hr))
        mPlaying = true;

    return hr;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::SetCurrentPosition(DWORD newPosition) noexcept
{
    FIXME(PREFIX "SetCurrentPosition (%p)->(%lu)\n", voidp{this}, newPosition);
    return DSERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::SetFormat(const WAVEFORMATEX *wfx) noexcept
{
    DEBUG(PREFIX "SetFormat (%p)->(%p)\n", voidp{this}, cvoidp{wfx});

    if(!wfx)
    {
        WARN(PREFIX "SetFormat Missing format\n");
        return DSERR_INVALIDPARAM;
    }

    std::lock_guard lock{mMutex};
    if(mParent.getPriorityLevel() < DSSCL_PRIORITY)
        return DSERR_PRIOLEVELNEEDED;

    static constexpr WORD ExtExtraSize{sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
    if(wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        /* Fail silently.. */
        if(wfx->cbSize < ExtExtraSize)
        {
            WARN(PREFIX "SetFormat EXTENSIBLE size too small (%u, expected %u). Ignoring...\n",
                wfx->cbSize, ExtExtraSize);
            return DS_OK;
        }

        const WAVEFORMATEXTENSIBLE *wfe{CONTAINING_RECORD(wfx, const WAVEFORMATEXTENSIBLE, Format)};
        TRACE(PREFIX "SetFormat Requested primary format:\n"
              "    FormatTag          = 0x%04x\n"
              "    Channels           = %u\n"
              "    SamplesPerSec      = %lu\n"
              "    AvgBytesPerSec     = %lu\n"
              "    BlockAlign         = %u\n"
              "    BitsPerSample      = %u\n"
              "    ValidBitsPerSample = %u\n"
              "    ChannelMask        = 0x%08lx\n"
              "    SubFormat          = %s\n",
            wfe->Format.wFormatTag, wfe->Format.nChannels, wfe->Format.nSamplesPerSec,
            wfe->Format.nAvgBytesPerSec, wfe->Format.nBlockAlign, wfe->Format.wBitsPerSample,
            wfe->Samples.wValidBitsPerSample, wfe->dwChannelMask,
            FmtidPrinter{wfe->SubFormat}.c_str());
    }
    else
    {
        TRACE(PREFIX "SetFormat Requested primary format:\n"
              "    FormatTag      = 0x%04x\n"
              "    Channels       = %u\n"
              "    SamplesPerSec  = %lu\n"
              "    AvgBytesPerSec = %lu\n"
              "    BlockAlign     = %u\n"
              "    BitsPerSample  = %u\n",
            wfx->wFormatTag, wfx->nChannels, wfx->nSamplesPerSec, wfx->nAvgBytesPerSec,
            wfx->nBlockAlign, wfx->wBitsPerSample);
    }

    auto copy_format = [wfx](WAVEFORMATEXTENSIBLE &dst) -> HRESULT
    {
        if(wfx->nChannels <= 0)
        {
            WARN("copy_format Invalid Channels %d\n", wfx->nChannels);
            return DSERR_INVALIDPARAM;
        }
        if(wfx->nSamplesPerSec < DSBFREQUENCY_MIN || wfx->nSamplesPerSec > DSBFREQUENCY_MAX)
        {
            WARN("copy_format Invalid SamplesPerSec %lu\n", wfx->nSamplesPerSec);
            return DSERR_INVALIDPARAM;
        }
        if(wfx->nBlockAlign <= 0)
        {
            WARN("copy_format Invalid BlockAlign %d\n", wfx->nBlockAlign);
            return DSERR_INVALIDPARAM;
        }
        if(wfx->wBitsPerSample == 0 || (wfx->wBitsPerSample%8) != 0)
        {
            WARN("copy_format Invalid BitsPerSample %d\n", wfx->wBitsPerSample);
            return DSERR_INVALIDPARAM;
        }
        if(wfx->nBlockAlign != wfx->nChannels*wfx->wBitsPerSample/8)
        {
            WARN("copy_format Invalid BlockAlign %d (expected %u = %u*%u/8)\n",
                 wfx->nBlockAlign, wfx->nChannels*wfx->wBitsPerSample/8,
                 wfx->nChannels, wfx->wBitsPerSample);
            return DSERR_INVALIDPARAM;
        }
        if(wfx->nAvgBytesPerSec != wfx->nBlockAlign*wfx->nSamplesPerSec)
        {
            WARN("copy_format Invalid AvgBytesPerSec %lu (expected %lu = %lu*%u)\n",
                 wfx->nAvgBytesPerSec, wfx->nSamplesPerSec*wfx->nBlockAlign,
                 wfx->nSamplesPerSec, wfx->nBlockAlign);
            return DSERR_INVALIDPARAM;
        }

        if(wfx->wFormatTag == WAVE_FORMAT_PCM)
        {
            if(wfx->wBitsPerSample > 32)
                return DSERR_INVALIDPARAM;
            dst = {};
        }
        else if(wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            if(wfx->wBitsPerSample != 32)
                return DSERR_INVALIDPARAM;
            dst = {};
        }
        else if(wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const WAVEFORMATEXTENSIBLE *fromx{CONTAINING_RECORD(wfx, const WAVEFORMATEXTENSIBLE, Format)};

            if(fromx->Samples.wValidBitsPerSample > fromx->Format.wBitsPerSample)
                return DSERR_INVALIDPARAM;

            if(fromx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            {
                if(wfx->wBitsPerSample > 32)
                    return DSERR_INVALIDPARAM;
            }
            else if(fromx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            {
                if(wfx->wBitsPerSample != 32)
                    return DSERR_INVALIDPARAM;
            }
            else
            {
                FIXME("copy_format Unhandled extensible format: %s\n",
                    GuidPrinter{fromx->SubFormat}.c_str());
                return DSERR_INVALIDPARAM;
            }

            dst = {};
            dst.Format.cbSize = ExtExtraSize;
            dst.Samples.wValidBitsPerSample = fromx->Samples.wValidBitsPerSample;
            if(!dst.Samples.wValidBitsPerSample)
                dst.Samples.wValidBitsPerSample = fromx->Format.wBitsPerSample;
            dst.dwChannelMask = fromx->dwChannelMask;
            dst.SubFormat = fromx->SubFormat;
        }
        else
        {
            FIXME("copy_format Unhandled format tag %04x\n", wfx->wFormatTag);
            return DSERR_INVALIDPARAM;
        }

        dst.Format.wFormatTag = wfx->wFormatTag;
        dst.Format.nChannels = wfx->nChannels;
        dst.Format.nSamplesPerSec = wfx->nSamplesPerSec;
        dst.Format.nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
        dst.Format.nBlockAlign = static_cast<WORD>(wfx->wBitsPerSample * wfx->nChannels / 8);
        dst.Format.wBitsPerSample = wfx->wBitsPerSample;
        return DS_OK;
    };

    auto hr = HRESULT{copy_format(mFormat)};
    if(SUCCEEDED(hr) && mWriteEmu)
    {
        mWriteEmu = nullptr;
        hr = createWriteEmu(mFlags);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::SetVolume(LONG volume) noexcept
{
    FIXME(PREFIX "SetVolume (%p)->(%ld)\n", voidp{this}, volume);

    if(volume > DSBVOLUME_MAX || volume < DSBVOLUME_MIN)
    {
        WARN(PREFIX "Invalid volume (%ld)\n", volume);
        return DSERR_INVALIDPARAM;
    }

    std::lock_guard lock{mMutex};
    if(!(mFlags&DSBCAPS_CTRLVOLUME))
        return DSERR_CONTROLUNAVAIL;

    ALSection alsection{mContext};
    mVolume = volume;
    alListenerf(AL_GAIN, mB_to_gain(static_cast<float>(volume)));

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::SetPan(LONG pan) noexcept
{
    FIXME(PREFIX "SetPan (%p)->(%ld): stub\n", voidp{this}, pan);

    if(pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT)
    {
        WARN(PREFIX "Invalid pan (%ld)\n", pan);
        return DSERR_INVALIDPARAM;
    }

    std::lock_guard lock{mMutex};
    if(!(mFlags&DSBCAPS_CTRLPAN))
        return DSERR_CONTROLUNAVAIL;

    auto hr = HRESULT{S_OK};
    if(mWriteEmu)
        hr = mWriteEmu->SetPan(pan);
    if(SUCCEEDED(hr))
        mPan = pan;

    return hr;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::SetFrequency(DWORD frequency) noexcept
{
    FIXME(PREFIX "SetFrequency (%p)->(%lu)\n", voidp{this}, frequency);
    return DSERR_CONTROLUNAVAIL;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Stop() noexcept
{
    DEBUG("PrimaryBuffer::Stop (%p)->()\n", voidp{this});

    std::lock_guard lock{mMutex};
    auto hr = HRESULT{S_OK};
    if(mWriteEmu)
        hr = mWriteEmu->Stop();
    if(SUCCEEDED(hr))
        mPlaying = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Unlock(void *audioPtr1, DWORD audioBytes1, void *audioPtr2, DWORD audioBytes2) noexcept
{
    FIXME(PREFIX "Unlock (%p)->(%p, %lu, %p, %lu)\n", voidp{this}, audioPtr1, audioBytes1,
        audioPtr2, audioBytes2);

    std::lock_guard lock{mMutex};
    if(mWriteEmu)
        return mWriteEmu->Unlock(audioPtr1, audioBytes1, audioPtr2, audioBytes2);
    return DSERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Restore() noexcept
{
    FIXME(PREFIX "Restore (%p)->()\n", voidp{this});

    std::lock_guard lock{mMutex};
    if(mWriteEmu)
        return mWriteEmu->Restore();
    return DS_OK;
}


void PrimaryBuffer::setParams(const DS3DLISTENER &params, const std::bitset<FlagCount> flags)
{
    if(flags.test(Position))
        mImmediate.vPosition = params.vPosition;
    if(flags.test(Velocity))
        mImmediate.vVelocity = params.vVelocity;
    if(flags.test(Orientation))
    {
        mImmediate.vOrientFront = params.vOrientFront;
        mImmediate.vOrientTop = params.vOrientTop;
    }
    if(flags.test(DistanceFactor))
        mImmediate.flDistanceFactor = params.flDistanceFactor;
    if(flags.test(RolloffFactor))
        mImmediate.flRolloffFactor = params.flRolloffFactor;
    if(flags.test(DopplerFactor))
        mImmediate.flDopplerFactor = params.flDopplerFactor;

    if(flags.test(Position))
        alListener3f(AL_POSITION, params.vPosition.x, params.vPosition.y, -params.vPosition.z);
    if(flags.test(Velocity))
        alListener3f(AL_VELOCITY, params.vVelocity.x, params.vVelocity.y, -params.vVelocity.z);
    if(flags.test(Orientation))
    {
        const std::array<ALfloat,6> ori{{
            params.vOrientFront.x, params.vOrientFront.y, -params.vOrientFront.z,
            params.vOrientTop.x, params.vOrientTop.y, -params.vOrientTop.z}};
        alListenerfv(AL_ORIENTATION, ori.data());
    }
    if(flags.test(DistanceFactor))
    {
        alSpeedOfSound(343.3f / params.flDistanceFactor);
        if(mParent.haveExtension(EXT_EFX))
            alListenerf(AL_METERS_PER_UNIT, params.flDistanceFactor);
    }
    if(flags.test(RolloffFactor))
    {
        for(Buffer *buffer : mParent.get3dBuffers())
        {
            if(buffer->getCurrentMode() != DS3DMODE_DISABLE)
            {
                if(ALuint source{buffer->getSource()})
                    alSourcef(source, AL_ROLLOFF_FACTOR, params.flRolloffFactor);
            }
        }
    }
    if(flags.test(DopplerFactor))
        alDopplerFactor(params.flDopplerFactor);
}

void PrimaryBuffer::commit() noexcept
{
    if(auto flags = std::exchange(mDirty, 0); flags.any())
    {
        setParams(mDeferred, flags);
        alGetError();
    }

    for(Buffer *buffer : mParent.get3dBuffers())
        buffer->commit();
    alGetError();
}
#undef PREFIX

#define PREFIX "Primary::Listener3D::"
HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::QueryInterface(REFIID riid, void** ppvObject) noexcept
{ return impl_from_base()->QueryInterface(riid, ppvObject); }

ULONG STDMETHODCALLTYPE PrimaryBuffer::Listener3D::AddRef() noexcept
{
    auto self = impl_from_base();
    self->mTotalRef.fetch_add(1u, std::memory_order_relaxed);
    const auto ret = self->mDs3dRef.fetch_add(1u, std::memory_order_relaxed) + 1;
    DEBUG(PREFIX "AddRef (%p) ref %lu\n", voidp{this}, ret);
    return ret;
}

ULONG STDMETHODCALLTYPE PrimaryBuffer::Listener3D::Release() noexcept
{
    auto self = impl_from_base();
    const auto ret = self->mDs3dRef.fetch_sub(1u, std::memory_order_relaxed) - 1;
    DEBUG(PREFIX "Release (%p) ref %lu\n", voidp{this}, ret);
    self->mTotalRef.fetch_sub(1u, std::memory_order_relaxed);
    return ret;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetAllParameters(DS3DLISTENER *listener) noexcept
{
    DEBUG(PREFIX "GetAllParameters (%p)->(%p)\n", voidp{this}, voidp{listener});

    if(!listener || listener->dwSize < sizeof(*listener))
    {
        WARN(PREFIX "Invalid DS3DLISTENER (%p %lu)\n", voidp{listener},
            listener ? listener->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    listener->vPosition = self->mImmediate.vPosition;
    listener->vVelocity = self->mImmediate.vVelocity;
    listener->vOrientFront = self->mImmediate.vOrientFront;
    listener->vOrientTop = self->mImmediate.vOrientTop;
    listener->flDistanceFactor = self->mImmediate.flDistanceFactor;
    listener->flRolloffFactor = self->mImmediate.flRolloffFactor;
    listener->flDopplerFactor = self->mImmediate.flDopplerFactor;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetDistanceFactor(D3DVALUE *distanceFactor) noexcept
{
    DEBUG(PREFIX "GetDistanceFactor (%p)->(%p)\n", voidp{this}, voidp{distanceFactor});

    if(!distanceFactor)
        return DSERR_INVALIDPARAM;

    auto self = impl_from_base();
    *distanceFactor = self->mImmediate.flDistanceFactor;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetDopplerFactor(D3DVALUE *dopplerFactor) noexcept
{
    DEBUG(PREFIX "GetDoppleFactor (%p)->(%p)\n", voidp{this}, voidp{dopplerFactor});

    if(!dopplerFactor)
        return DSERR_INVALIDPARAM;

    auto self = impl_from_base();
    *dopplerFactor = self->mImmediate.flDopplerFactor;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetOrientation(D3DVECTOR *orientFront, D3DVECTOR *orientTop) noexcept
{
    DEBUG(PREFIX "GetOrientation (%p)->(%p, %p)\n", voidp{this}, voidp{orientFront},
        voidp{orientTop});

    if(!orientFront || !orientTop)
        return DSERR_INVALIDPARAM;

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    *orientFront = self->mImmediate.vOrientFront;
    *orientTop = self->mImmediate.vOrientTop;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetPosition(D3DVECTOR *position) noexcept
{
    DEBUG(PREFIX "GetPosition (%p)->(%p)\n", voidp{this}, voidp{position});

    if(!position)
        return DSERR_INVALIDPARAM;

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    *position = self->mImmediate.vPosition;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetRolloffFactor(D3DVALUE *rolloffFactor) noexcept
{
    DEBUG(PREFIX "GetRolloffFactor (%p)->(%p)\n", voidp{this}, voidp{rolloffFactor});

    if(!rolloffFactor)
        return DSERR_INVALIDPARAM;

    auto self = impl_from_base();
    *rolloffFactor = self->mImmediate.flRolloffFactor;

    return DS_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::GetVelocity(D3DVECTOR *velocity) noexcept
{
    DEBUG(PREFIX "GetVelocity (%p)->(%p)\n", voidp{this}, voidp{velocity});

    if(!velocity)
        return DSERR_INVALIDPARAM;

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    *velocity = self->mImmediate.vVelocity;

    return DS_OK;
}


HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetAllParameters(const DS3DLISTENER *listener, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetAllParameters (%p)->(%p, %lu)\n", voidp{this}, cvoidp{listener}, apply);

    if(!listener || listener->dwSize < sizeof(*listener))
    {
        WARN(PREFIX "Invalid parameter (%p %lu)\n", cvoidp{listener},
            listener ? listener->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    if(listener->flDistanceFactor > DS3D_MAXDISTANCEFACTOR
        || listener->flDistanceFactor < DS3D_MINDISTANCEFACTOR)
    {
        WARN(PREFIX "Invalid distance factor (%f)\n", listener->flDistanceFactor);
        return DSERR_INVALIDPARAM;
    }

    if(listener->flDopplerFactor > DS3D_MAXDOPPLERFACTOR
        || listener->flDopplerFactor < DS3D_MINDOPPLERFACTOR)
    {
        WARN(PREFIX "Invalid doppler factor (%f)\n", listener->flDopplerFactor);
        return DSERR_INVALIDPARAM;
    }

    if(listener->flRolloffFactor < DS3D_MINROLLOFFFACTOR
        || listener->flRolloffFactor > DS3D_MAXROLLOFFFACTOR)
    {
        WARN(PREFIX "Invalid rolloff factor (%f)\n", listener->flRolloffFactor);
        return DSERR_INVALIDPARAM;
    }

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred = *listener;
        self->mDeferred.dwSize = sizeof(self->mDeferred);
        self->mDirty.set();
    }
    else
    {
        ALSection alsection{self->mContext};
        alcSuspendContext(self->mContext);
        self->setParams(*listener, ~0ull);
        alcProcessContext(self->mContext);
    }

    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetDistanceFactor(D3DVALUE distanceFactor, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetDistanceFactor (%p)->(%f, %lu)\n", voidp{this}, distanceFactor, apply);

    if(distanceFactor < DS3D_MINDISTANCEFACTOR || distanceFactor > DS3D_MAXDISTANCEFACTOR)
    {
        WARN(PREFIX "Invalid parameter %f\n", distanceFactor);
        return DSERR_INVALIDPARAM;
    }

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred.flDistanceFactor = distanceFactor;
        self->mDirty.set(DistanceFactor);
    }
    else
    {
        ALSection alsection{self->mContext};
        alcSuspendContext(self->mContext);
        self->mImmediate.flDistanceFactor = distanceFactor;
        alSpeedOfSound(343.3f / distanceFactor);
        if(self->mParent.haveExtension(EXT_EFX))
            alListenerf(AL_METERS_PER_UNIT, distanceFactor);
        alGetError();
        alcProcessContext(self->mContext);
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetDopplerFactor(D3DVALUE dopplerFactor, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetDopplerFactor (%p)->(%f, %lu)\n", voidp{this}, dopplerFactor, apply);

    if(dopplerFactor < DS3D_MINDOPPLERFACTOR || dopplerFactor > DS3D_MAXDOPPLERFACTOR)
    {
        WARN(PREFIX "Invalid parameter %f\n", dopplerFactor);
        return DSERR_INVALIDPARAM;
    }

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred.flDopplerFactor = dopplerFactor;
        self->mDirty.set(DopplerFactor);
    }
    else
    {
        ALSection alsection{self->mContext};
        self->mImmediate.flDopplerFactor = dopplerFactor;
        alDopplerFactor(dopplerFactor);
        alGetError();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetOrientation(D3DVALUE xFront, D3DVALUE yFront, D3DVALUE zFront, D3DVALUE xTop, D3DVALUE yTop, D3DVALUE zTop, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetOrientation (%p)->(%f, %f, %f, %f, %f, %f, %lu)\n", voidp{this}, xFront, yFront, zFront, xTop, yTop, zTop, apply);

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred.vOrientFront.x = xFront;
        self->mDeferred.vOrientFront.y = yFront;
        self->mDeferred.vOrientFront.z = zFront;
        self->mDeferred.vOrientTop.x = xTop;
        self->mDeferred.vOrientTop.y = yTop;
        self->mDeferred.vOrientTop.z = zTop;
        self->mDirty.set(Orientation);
    }
    else
    {
        ALSection alsection{self->mContext};
        self->mImmediate.vOrientFront.x = xFront;
        self->mImmediate.vOrientFront.y = yFront;
        self->mImmediate.vOrientFront.z = zFront;
        self->mImmediate.vOrientTop.x = xTop;
        self->mImmediate.vOrientTop.y = yTop;
        self->mImmediate.vOrientTop.z = zTop;

        const std::array<ALfloat,6> ori{{xFront, yFront, -zFront, xTop, yTop, -zTop}};
        alListenerfv(AL_ORIENTATION, ori.data());
        alGetError();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetPosition (%p)->(%f, %f, %f, %lu)\n", voidp{this}, x, y, z, apply);

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred.vPosition.x = x;
        self->mDeferred.vPosition.y = y;
        self->mDeferred.vPosition.z = z;
        self->mDirty.set(Position);
    }
    else
    {
        ALSection alsection{self->mContext};
        self->mImmediate.vPosition.x = x;
        self->mImmediate.vPosition.y = y;
        self->mImmediate.vPosition.z = z;

        alListener3f(AL_POSITION, x, y, -z);
        alGetError();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetRolloffFactor(D3DVALUE rolloffFactor, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetRolloffFactor (%p)->(%f, %lu)\n", voidp{this}, rolloffFactor, apply);

    if(rolloffFactor < DS3D_MINROLLOFFFACTOR || rolloffFactor > DS3D_MAXROLLOFFFACTOR)
    {
        WARN(PREFIX "Invalid parameter %f\n", rolloffFactor);
        return DSERR_INVALIDPARAM;
    }

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred.flRolloffFactor = rolloffFactor;
        self->mDirty.set(RolloffFactor);
    }
    else
    {
        ALSection alsection{self->mContext};
        alcSuspendContext(self->mContext);
        self->mImmediate.flRolloffFactor = rolloffFactor;

        for(Buffer *buffer : self->mParent.get3dBuffers())
        {
            if(buffer->getCurrentMode() != DS3DMODE_DISABLE)
            {
                if(ALuint source{buffer->getSource()})
                    alSourcef(source, AL_ROLLOFF_FACTOR, rolloffFactor);
            }
        }
        alGetError();
        alcProcessContext(self->mContext);
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) noexcept
{
    DEBUG(PREFIX "SetVelocity (%p)->(%f, %f, %f, %lu)\n", voidp{this}, x, y, z, apply);

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    if(apply == DS3D_DEFERRED)
    {
        self->mDeferred.vVelocity.x = x;
        self->mDeferred.vVelocity.y = y;
        self->mDeferred.vVelocity.z = z;
        self->mDirty.set(Velocity);
    }
    else
    {
        ALSection alsection{self->mContext};
        self->mImmediate.vVelocity.x = x;
        self->mImmediate.vVelocity.y = y;
        self->mImmediate.vVelocity.z = z;

        alListener3f(AL_VELOCITY, x, y, -z);
        alGetError();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE PrimaryBuffer::Listener3D::CommitDeferredSettings() noexcept
{
    DEBUG(PREFIX "CommitDeferredSettings (%p)->()\n", voidp{this});

    auto self = impl_from_base();
    std::lock_guard lock{self->mMutex};
    ALSection alsection{self->mContext};
    alcSuspendContext(self->mContext);

    self->commit();

    alcProcessContext(self->mContext);
    return DS_OK;
}
#undef PREFIX
