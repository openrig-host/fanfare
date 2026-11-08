#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <array>

namespace OpenRig {

/**
 * ISO/IEC 18004 Model 2 QR Code Generator (Byte Mode, ECC Level M, Versions 1..10).
 * Implements full GF(256) Reed-Solomon error correction, multi-block interleaving,
 * 8-mask penalty evaluation, BCH(15,5) format bits, and Golay/BCH(18,6) version bits.
 */
class QrCodeGenerator {
public:
    struct QrMatrix {
        int size = 0;
        std::vector<bool> modules; // size * size (true = dark, false = light)

        bool get(int x, int y) const {
            if (x < 0 || x >= size || y < 0 || y >= size) return false;
            return modules[(size_t)(y * size + x)];
        }

        void set(int x, int y, bool val) {
            if (x >= 0 && x < size && y >= 0 && y < size)
                modules[(size_t)(y * size + x)] = val;
        }
    };

    static QrMatrix generate(const juce::String& text) {
        std::string utf8 = text.toStdString();
        const int len = (int)utf8.size();

        // Capacity in bytes for Byte Mode at ECC Level M (Versions 1..10)
        static const int byteCapM[11]         = { 0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213 };
        static const int eccPerBlockM[11]     = { 0, 10, 16, 26, 18, 24,  16,  18,  22,  22,  26 };
        static const int numErrorBlocksM[11]  = { 0,  1,  1,  1,  2,  2,   4,   4,   4,   5,   5 };

        int version = 0;
        for (int v = 1; v <= 10; ++v) {
            if (len <= byteCapM[v]) {
                version = v;
                break;
            }
        }
        if (version == 0) {
            version = 10;
            utf8.resize((size_t)byteCapM[10]);
        }

        const int actualLen = (int)utf8.size();
        const int rawDataModules = getNumRawDataModules(version);
        const int totalCodewords = rawDataModules / 8;
        const int numBlocks      = numErrorBlocksM[version];
        const int blockEccLen    = eccPerBlockM[version];
        const int dataCapacity   = totalCodewords - blockEccLen * numBlocks;

        // 1. Encode Byte Mode bitstream (mode 0100 + char count + bytes + terminator + pad)
        std::vector<bool> bitBuf;
        bitBuf.reserve((size_t)(dataCapacity * 8));

        appendBits(0x4, 4, bitBuf); // Byte mode indicator (0100)
        appendBits((uint32_t)actualLen, (version <= 9) ? 8 : 16, bitBuf);
        for (int i = 0; i < actualLen; ++i)
            appendBits((uint8_t)utf8[(size_t)i], 8, bitBuf);

        const int capacityBits = dataCapacity * 8;
        const int termLen = std::min(4, capacityBits - (int)bitBuf.size());
        appendBits(0, termLen, bitBuf);

        const int padZeroBits = (8 - ((int)bitBuf.size() % 8)) % 8;
        appendBits(0, padZeroBits, bitBuf);

        std::vector<uint8_t> dataCodewords;
        dataCodewords.reserve((size_t)dataCapacity);
        for (size_t i = 0; i < bitBuf.size(); i += 8) {
            uint8_t b = 0;
            for (size_t j = 0; j < 8; ++j)
                b = (uint8_t)((b << 1) | (bitBuf[i + j] ? 1 : 0));
            dataCodewords.push_back(b);
        }

        for (uint8_t padByte = 0xEC; (int)dataCodewords.size() < dataCapacity; padByte ^= (0xEC ^ 0x11))
            dataCodewords.push_back(padByte);

        // 2. Reed-Solomon ECC per block + Interleaving (ISO/IEC 18004 Section 7.5 / 7.6)
        const std::vector<uint8_t> rsDivisor = rsComputeDivisor(blockEccLen);
        const int numShortBlocks = numBlocks - (totalCodewords % numBlocks);
        const int shortBlockTotalLen = totalCodewords / numBlocks;
        const int shortDataLen = shortBlockTotalLen - blockEccLen;

        std::vector<std::vector<uint8_t>> blocks((size_t)numBlocks);
        for (int i = 0, k = 0; i < numBlocks; ++i) {
            const int datLen = shortDataLen + (i < numShortBlocks ? 0 : 1);
            std::vector<uint8_t> dat(dataCodewords.begin() + k, dataCodewords.begin() + k + datLen);
            k += datLen;

            const std::vector<uint8_t> ecc = rsComputeRemainder(dat, rsDivisor);
            if (i < numShortBlocks)
                dat.push_back(0); // Dummy slot skipped during interleave step
            dat.insert(dat.end(), ecc.begin(), ecc.end());
            blocks[(size_t)i] = std::move(dat);
        }

        std::vector<uint8_t> allCodewords;
        allCodewords.reserve((size_t)totalCodewords);
        const size_t fullBlockSize = blocks[0].size();
        for (size_t i = 0; i < fullBlockSize; ++i) {
            for (int j = 0; j < numBlocks; ++j) {
                if ((int)i == shortDataLen && j < numShortBlocks)
                    continue;
                allCodewords.push_back(blocks[(size_t)j][i]);
            }
        }

        // 3. Construct Matrix & Function Patterns
        const int size = version * 4 + 17;
        QrMatrix baseMat;
        baseMat.size = size;
        baseMat.modules.assign((size_t)(size * size), false);
        std::vector<bool> isFunction((size_t)(size * size), false);

        auto setFunc = [&](int x, int y, bool dark) {
            if (x >= 0 && x < size && y >= 0 && y < size) {
                baseMat.set(x, y, dark);
                isFunction[(size_t)(y * size + x)] = true;
            }
        };

        // Timing patterns
        for (int i = 0; i < size; ++i) {
            setFunc(6, i, (i % 2 == 0));
            setFunc(i, 6, (i % 2 == 0));
        }

        // Finder patterns + 1-module white separators
        auto drawFinder = [&](int cx, int cy) {
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const int xx = cx + dx;
                    const int yy = cy + dy;
                    if (xx >= 0 && xx < size && yy >= 0 && yy < size) {
                        const int dist = std::max(std::abs(dx), std::abs(dy));
                        setFunc(xx, yy, dist != 2 && dist != 4);
                    }
                }
            }
        };
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);

        // Alignment patterns (Versions 2..10)
        const std::vector<int> alignPos = getAlignmentPatternPositions(version, size);
        const int numAlign = (int)alignPos.size();
        for (int i = 0; i < numAlign; ++i) {
            for (int j = 0; j < numAlign; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0))
                    continue; // Skip 3 finder corners
                const int cx = alignPos[(size_t)i];
                const int cy = alignPos[(size_t)j];
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx)
                        setFunc(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
                }
            }
        }

        // Reserve Format Info & Dark Module
        drawFormatBits(baseMat, isFunction, 0);

        // Version Info (Versions 7..10)
        if (version >= 7) {
            int rem = version;
            for (int i = 0; i < 12; ++i)
                rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            const uint32_t verBits = ((uint32_t)version << 12) | (uint32_t)rem;
            for (int i = 0; i < 18; ++i) {
                const bool dark = ((verBits >> i) & 1U) != 0;
                const int a = size - 11 + (i % 3);
                const int b = i / 3;
                setFunc(a, b, dark);
                setFunc(b, a, dark);
            }
        }

        // 4. Zigzag Codeword Placement
        size_t bitIdx = 0;
        const size_t totalDataBits = allCodewords.size() * 8;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = (((right + 1) & 2) == 0);
                    const int y = upward ? (size - 1 - vert) : vert;
                    if (!isFunction[(size_t)(y * size + x)]) {
                        bool dark = false;
                        if (bitIdx < totalDataBits) {
                            dark = ((allCodewords[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1) != 0;
                            ++bitIdx;
                        }
                        baseMat.set(x, y, dark); // Remainder bits default to false (0)
                    }
                }
            }
        }

        // 5. Evaluate All 8 Masks and Pick Lowest ISO Penalty Score
        int bestMask = 0;
        long bestPenalty = LONG_MAX;
        QrMatrix bestMat = baseMat;

        for (int mask = 0; mask < 8; ++mask) {
            QrMatrix candidate = baseMat;
            applyMask(candidate, isFunction, mask);
            drawFormatBits(candidate, isFunction, mask);
            const long penalty = computePenaltyScore(candidate);
            if (penalty < bestPenalty) {
                bestPenalty = penalty;
                bestMask = mask;
                bestMat = std::move(candidate);
            }
        }
        (void)bestMask;
        return bestMat;
    }

    /**
     * Draw the QR code on crisp integer pixel boundaries with a standard >= 4-module quiet zone.
     */
    static void draw(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& payload,
                     juce::Colour fg = juce::Colours::black, juce::Colour bg = juce::Colours::white) {
        if (payload.isEmpty() || area.isEmpty()) return;

        const QrMatrix mat = generate(payload);
        if (mat.size <= 0) return;

        g.setColour(bg);
        g.fillRect(area);

        constexpr int quietZone = 4;
        const int totalModules = mat.size + quietZone * 2;
        const int minSide = std::min(area.getWidth(), area.getHeight());
        const int modulePx = std::max(1, minSide / totalModules);

        const int qrSpan = mat.size * modulePx;
        const int startX = area.getX() + (area.getWidth() - qrSpan) / 2;
        const int startY = area.getY() + (area.getHeight() - qrSpan) / 2;

        g.setColour(fg);
        for (int y = 0; y < mat.size; ++y) {
            const int py = startY + y * modulePx;
            int x = 0;
            while (x < mat.size) {
                if (!mat.get(x, y)) {
                    ++x;
                    continue;
                }
                int runEnd = x + 1;
                while (runEnd < mat.size && mat.get(runEnd, y))
                    ++runEnd;
                g.fillRect(startX + x * modulePx, py, (runEnd - x) * modulePx, modulePx);
                x = runEnd;
            }
        }
    }

    /**
     * Build standard Wi-Fi configuration payload with proper escaping (iOS / Android Camera compatible).
     */
    static juce::String makeWifiPayload(const juce::String& ssid, const juce::String& password) {
        auto escapeWifiField = [](const juce::String& s) {
            juce::String out;
            for (int i = 0; i < s.length(); ++i) {
                const juce::juce_wchar c = s[i];
                if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':')
                    out << '\\';
                out << c;
            }
            return out;
        };
        if (password.isEmpty())
            return "WIFI:T:nopass;S:" + escapeWifiField(ssid) + ";;";
        return "WIFI:T:WPA;S:" + escapeWifiField(ssid) + ";P:" + escapeWifiField(password) + ";;";
    }

private:
    static void appendBits(uint32_t val, int len, std::vector<bool>& out) {
        for (int i = len - 1; i >= 0; --i)
            out.push_back(((val >> i) & 1U) != 0);
    }

    static int getNumRawDataModules(int ver) {
        const int size = ver * 4 + 17;
        int result = size * size;
        result -= 8 * 8 * 3;          // 3 finder patterns + separators
        result -= 15 * 2 + 1;         // 2 format copies + dark module
        result -= (size - 16) * 2;    // timing patterns
        if (ver >= 2) {
            const int numAlign = ver / 7 + 2;
            result -= (numAlign - 1) * (numAlign - 1) * 25;
            result -= (numAlign - 2) * 2 * 20;
            if (ver >= 7)
                result -= 6 * 3 * 2;  // 2 version info blocks
        }
        return result;
    }

    static std::vector<int> getAlignmentPatternPositions(int ver, int size) {
        if (ver == 1)
            return {};
        const int numAlign = ver / 7 + 2;
        int step = (ver == 32) ? 26 : (((ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2)) * 2);
        std::vector<int> result((size_t)numAlign, 6);
        for (int i = numAlign - 1, pos = size - 7; i >= 1; --i, pos -= step)
            result[(size_t)i] = pos;
        return result;
    }

    static uint8_t gfMultiply(uint8_t x, uint8_t y) {
        int z = 0;
        for (int i = 7; i >= 0; --i) {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return (uint8_t)z;
    }

    static std::vector<uint8_t> rsComputeDivisor(int degree) {
        std::vector<uint8_t> result((size_t)degree, 0);
        result[(size_t)(degree - 1)] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i) {
            for (int j = 0; j < degree; ++j) {
                result[(size_t)j] = gfMultiply(result[(size_t)j], root);
                if (j + 1 < degree)
                    result[(size_t)j] ^= result[(size_t)(j + 1)];
            }
            root = gfMultiply(root, 0x02);
        }
        return result;
    }

    static std::vector<uint8_t> rsComputeRemainder(const std::vector<uint8_t>& data,
                                                   const std::vector<uint8_t>& divisor) {
        std::vector<uint8_t> result(divisor.size(), 0);
        for (uint8_t b : data) {
            const uint8_t factor = b ^ result.front();
            result.erase(result.begin());
            result.push_back(0);
            for (size_t i = 0; i < divisor.size(); ++i)
                result[i] ^= gfMultiply(divisor[i], factor);
        }
        return result;
    }

    static void drawFormatBits(QrMatrix& mat, std::vector<bool>& isFunction, int mask) {
        const int size = mat.size;
        auto setFunc = [&](int x, int y, bool dark) {
            mat.set(x, y, dark);
            isFunction[(size_t)(y * size + x)] = true;
        };

        // ECC Level M format indicator bits = 00 (0)
        const int data = (0 << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = ((data << 10) | rem) ^ 0x5412;

        // First copy (around top-left finder)
        for (int i = 0; i <= 5; ++i)
            setFunc(8, i, ((bits >> i) & 1) != 0);
        setFunc(8, 7, ((bits >> 6) & 1) != 0);
        setFunc(8, 8, ((bits >> 7) & 1) != 0);
        setFunc(7, 8, ((bits >> 8) & 1) != 0);
        for (int i = 9; i < 15; ++i)
            setFunc(14 - i, 8, ((bits >> i) & 1) != 0);

        // Second copy (top-right + bottom-left) + Dark Module at (8, size - 8)
        for (int i = 0; i < 8; ++i)
            setFunc(size - 1 - i, 8, ((bits >> i) & 1) != 0);
        for (int i = 8; i < 15; ++i)
            setFunc(8, size - 15 + i, ((bits >> i) & 1) != 0);
        setFunc(8, size - 8, true);
    }

    static void applyMask(QrMatrix& mat, const std::vector<bool>& isFunction, int mask) {
        const int size = mat.size;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (isFunction[(size_t)(y * size + x)])
                    continue;
                bool invert = false;
                switch (mask) {
                    case 0: invert = ((x + y) % 2 == 0); break;
                    case 1: invert = (y % 2 == 0); break;
                    case 2: invert = (x % 3 == 0); break;
                    case 3: invert = ((x + y) % 3 == 0); break;
                    case 4: invert = ((x / 3 + y / 2) % 2 == 0); break;
                    case 5: invert = ((x * y) % 2 + (x * y) % 3 == 0); break;
                    case 6: invert = (((x * y) % 2 + (x * y) % 3) % 2 == 0); break;
                    case 7: invert = (((x + y) % 2 + (x * y) % 3) % 2 == 0); break;
                    default: break;
                }
                if (invert)
                    mat.set(x, y, !mat.get(x, y));
            }
        }
    }

    static long computePenaltyScore(const QrMatrix& mat) {
        const int size = mat.size;
        long result = 0;

        auto finderPenaltyAddHistory = [](int currentRunLength, std::array<int, 7>& runHistory) {
            std::copy_backward(runHistory.begin(), runHistory.end() - 1, runHistory.end());
            runHistory[0] = currentRunLength;
        };

        auto finderPenaltyCountPatterns = [&](const std::array<int, 7>& runHistory) -> int {
            const int n = runHistory[1];
            const bool core = n > 0 && runHistory[2] == n && runHistory[3] == n * 3
                              && runHistory[4] == n && runHistory[5] == n;
            return (core && runHistory[0] >= n * 4 && runHistory[6] >= n ? 1 : 0)
                 + (core && runHistory[6] >= n * 4 && runHistory[0] >= n ? 1 : 0);
        };

        auto finderPenaltyTerminateAndCount = [&](bool currentRunColor, int currentRunLength,
                                                  std::array<int, 7>& runHistory) -> int {
            if (currentRunColor) {
                finderPenaltyAddHistory(currentRunLength, runHistory);
                currentRunLength = 0;
            }
            currentRunLength += size;
            finderPenaltyAddHistory(currentRunLength, runHistory);
            return finderPenaltyCountPatterns(runHistory);
        };

        // Adjacent modules in row having same color, and finder-like patterns
        for (int y = 0; y < size; ++y) {
            bool runColor = false;
            int runX = 0;
            std::array<int, 7> runHistory = {};
            for (int x = 0; x < size; ++x) {
                if (mat.get(x, y) == runColor) {
                    ++runX;
                    if (runX == 5) result += 3;
                    else if (runX > 5) ++result;
                } else {
                    finderPenaltyAddHistory(runX, runHistory);
                    if (!runColor)
                        result += finderPenaltyCountPatterns(runHistory) * 40;
                    runColor = mat.get(x, y);
                    runX = 1;
                }
            }
            result += finderPenaltyTerminateAndCount(runColor, runX, runHistory) * 40;
        }

        // Adjacent modules in column having same color, and finder-like patterns
        for (int x = 0; x < size; ++x) {
            bool runColor = false;
            int runY = 0;
            std::array<int, 7> runHistory = {};
            for (int y = 0; y < size; ++y) {
                if (mat.get(x, y) == runColor) {
                    ++runY;
                    if (runY == 5) result += 3;
                    else if (runY > 5) ++result;
                } else {
                    finderPenaltyAddHistory(runY, runHistory);
                    if (!runColor)
                        result += finderPenaltyCountPatterns(runHistory) * 40;
                    runColor = mat.get(x, y);
                    runY = 1;
                }
            }
            result += finderPenaltyTerminateAndCount(runColor, runY, runHistory) * 40;
        }

        // 2x2 blocks of modules having same color
        for (int y = 0; y < size - 1; ++y) {
            for (int x = 0; x < size - 1; ++x) {
                const bool color = mat.get(x, y);
                if (color == mat.get(x + 1, y) && color == mat.get(x, y + 1) && color == mat.get(x + 1, y + 1))
                    result += 3;
            }
        }

        // Balance of dark and light modules
        int dark = 0;
        for (bool b : mat.modules)
            if (b) ++dark;
        const int total = size * size;
        const int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
        if (k > 0)
            result += k * 10;

        return result;
    }
};

} // namespace OpenRig
