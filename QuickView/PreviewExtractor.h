#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <map>

// PreviewExtractor: Static helper to extract embedded thumbnails
class PreviewExtractor {
public:
    struct ExtractedData {
        const uint8_t* pData = nullptr; // Pointer to data within source buffer
        size_t size = 0;
        bool isCopy = false; // If true, caller owns pData (rare)
        std::vector<uint8_t> buffer; // Storage if copy needed
        
        bool IsValid() const { return pData != nullptr && size > 512; } // Min valid size
    };

    // RAW (ARW, CR2, NEF, DNG, RAF, ORF)
    // All usually Tiff-based.
    static bool ExtractFromRAW(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // TIFF / TIF
    // Standard TIFF may embed a JPEG thumbnail in IFD0/IFD1.
    static bool ExtractFromTIFF(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // HEIC / HEIF / AVIF
    // ISOBMFF based.
    static bool ExtractFromHEIC(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // PSD / PSB
    // Resource block based.
    static bool ExtractFromPSD(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // JPEG (Extract EXIF thumbnail from APP1 segment)
    static bool ExtractFromJPEG(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // CDR / CMX (CorelDRAW)
    // ZIP (X4+): scan central directory for an embedded preview entry
    //   (names containing "thumb"/"preview"/"page", prefer PNG, else BMP/JPG).
    // RIFF (older): locate the DISP chunk (or PRE*/PRV*/THMB/PTI_ for other
    //   versions) and extract the DIB bitmap inside, prefixed with a BMP file
    //   header so WIC can decode it.
    static bool ExtractFromCDR(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

private:
    // RAW/RIFF helpers
    static bool InflateRaw(const uint8_t* src, size_t srcSize,
                           std::vector<uint8_t>& outBuf, size_t hint);
    static bool FindDibInRange(const uint8_t* data, size_t start, size_t end,
                               size_t& dibOff, size_t& pixOffInDib, size_t& dibSize);
    static void WalkRiffCollect(const uint8_t* data, size_t size, size_t off,
                                size_t end, bool dispOnly,
                                std::vector<std::pair<size_t, size_t>>& ranges);
    // TIFF Parsing Helpers
    static bool ParseTiffIFD(const uint8_t* start, size_t size, size_t offset, bool isLittleEndian, uint64_t& jpegOffset, uint64_t& jpegSize);
    
    // ISOBMFF Parsing Helpers
    static uint32_t ReadU32BE(const uint8_t* p);
    static uint64_t ReadU64BE(const uint8_t* p);
};
