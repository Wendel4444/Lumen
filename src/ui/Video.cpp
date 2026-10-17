#include "Video.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace lumen::ui {

void VideoPlayer::Startup() { MFStartup(MF_VERSION, MFSTARTUP_LITE); }
void VideoPlayer::Shutdown() { MFShutdown(); }

bool VideoPlayer::Open(const std::wstring& path) {
    Close();
    path_ = path;
    stop_ = false;
    failed_ = false;
    hasFrame_ = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        fresh_ = false;
        w_ = h_ = 0;
    }
    thread_ = std::thread([this] { Decode(); });
    return true;
}

void VideoPlayer::Close() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    hasFrame_ = false;
    path_.clear();
}

ID2D1Bitmap1* VideoPlayer::Frame(Gfx& g) {
    if (g.generation() != gen_) {  // new GPU device
        bmp_.Reset();
        gen_ = g.generation();
        hasFrame_ = false;
    }
    std::lock_guard<std::mutex> lk(m_);
    if (fresh_ && w_ && h_) {
        if (!bmp_ || bmpW_ != w_ || bmpH_ != h_) {
            bmp_.Reset();
            bmp_.Attach(g.CreateOpaqueBitmap(w_, h_));
            bmpW_ = w_;
            bmpH_ = h_;
        }
        if (bmp_ && SUCCEEDED(bmp_->CopyFromMemory(nullptr, pixels_.data(), w_ * 4))) hasFrame_ = true;
        fresh_ = false;
    }
    return hasFrame_ ? bmp_.Get() : nullptr;
}

void VideoPlayer::Decode() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 2);
        // Lets the reader insert the decoder plus a colour converter so we receive plain RGB32.
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(path_.c_str(), attrs.Get(), &reader))) {
            failed_ = true;
            CoUninitialize();
            return;
        }
        const DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(video, TRUE);
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(reader->SetCurrentMediaType(video, nullptr, type.Get()))) {
            failed_ = true;
            CoUninitialize();
            return;
        }
        ComPtr<IMFMediaType> current;
        reader->GetCurrentMediaType(video, &current);
        UINT32 fw = 0, fh = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &fw, &fh);
        // Decoders pad heights to multiples of 16 (720 -> 736): show only the real picture.
        UINT32 vw = fw, vh = fh;
        MFVideoArea area{};
        if (SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                       nullptr))) {
            if (area.Area.cx > 0 && area.Area.cy > 0) {
                vw = std::min<UINT32>(fw, area.Area.cx);
                vh = std::min<UINT32>(fh, area.Area.cy);
            }
        }
        if (!fw || !fh) {
            failed_ = true;
            CoUninitialize();
            return;
        }

        using clock = std::chrono::steady_clock;
        auto base = clock::now();
        LONGLONG firstTs = -1;
        std::vector<uint8_t> local(static_cast<size_t>(vw) * vh * 4);
        while (!stop_) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(video, 0, &stream, &flags, &ts, &sample))) {
                failed_ = true;
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // loop
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pos);
                base = clock::now();
                firstTs = -1;
                continue;
            }
            if (!sample) continue;
            if (firstTs < 0) firstTs = ts;
            // Present at the sample's timestamp (100 ns units), relative to when the loop started.
            auto due = base + std::chrono::microseconds((ts - firstTs) / 10);
            while (!stop_ && clock::now() < due) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (stop_) break;

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
            BYTE* src = nullptr;
            LONG pitch = 0;
            ComPtr<IMF2DBuffer> buf2d;
            bool locked2d = SUCCEEDED(buf.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&src, &pitch));
            DWORD len = 0;
            if (!locked2d) {
                if (FAILED(buf->Lock(&src, nullptr, &len))) continue;
                pitch = static_cast<LONG>(fw * 4);
            }
            for (UINT32 y = 0; y < vh; ++y) std::memcpy(&local[static_cast<size_t>(y) * vw * 4], src + y * pitch, vw * 4);
            if (locked2d) buf2d->Unlock2D();
            else buf->Unlock();
            {
                std::lock_guard<std::mutex> lk(m_);
                pixels_.swap(local);
                if (local.size() != pixels_.size()) local.resize(pixels_.size());
                w_ = vw;
                h_ = vh;
                fresh_ = true;
            }
        }
    }
    CoUninitialize();
}

}  // namespace lumen::ui
