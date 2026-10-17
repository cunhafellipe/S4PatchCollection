#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstring>
#include <algorithm>

// ----------------------------------------------------------------------------
// Arabic Letter Data & Shaping Tables
// ----------------------------------------------------------------------------
struct ArabicGlyph {
    wchar_t unicode;    // Base codepoint (0x0600 - 0x06FF)
    wchar_t isolated;   // Presentation Form-B
    wchar_t finalForm;
    wchar_t initialForm;
    wchar_t medialForm;
    bool rightJoin;     // Can join with previous letter (from right)
    bool leftJoin;      // Can join with next letter (to left)
};

static const ArabicGlyph g_ArabicTable[] = {
    { 0x0621, 0xFE80, 0xFE80, 0xFE80, 0xFE80, false, false }, // Hamza
    { 0x0622, 0xFE81, 0xFE82, 0xFE81, 0xFE82, true,  false }, // Alef Madda
    { 0x0623, 0xFE83, 0xFE84, 0xFE83, 0xFE84, true,  false }, // Alef Hamza Above
    { 0x0624, 0xFE85, 0xFE86, 0xFE85, 0xFE86, true,  false }, // Waw Hamza
    { 0x0625, 0xFE87, 0xFE88, 0xFE87, 0xFE88, true,  false }, // Alef Hamza Below
    { 0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, true,  true  }, // Yeh Hamza
    { 0x0627, 0xFE8D, 0xFE8E, 0xFE8D, 0xFE8E, true,  false }, // Alef
    { 0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, true,  true  }, // Beh
    { 0x0629, 0xFE93, 0xFE94, 0xFE93, 0xFE94, true,  false }, // Teh Marbuta
    { 0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, true,  true  }, // Teh
    { 0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, true,  true  }, // Theh
    { 0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, true,  true  }, // Jeem
    { 0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, true,  true  }, // Hah
    { 0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, true,  true  }, // Khah
    { 0x062F, 0xFEA9, 0xFEAA, 0xFEA9, 0xFEAA, true,  false }, // Dal
    { 0x0630, 0xFEAB, 0xFEAC, 0xFEAB, 0xFEAC, true,  false }, // Thal
    { 0x0631, 0xFEAD, 0xFEAE, 0xFEAD, 0xFEAE, true,  false }, // Reh
    { 0x0632, 0xFEAF, 0xFEB0, 0xFEAF, 0xFEB0, true,  false }, // Zain
    { 0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, true,  true  }, // Seen
    { 0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, true,  true  }, // Sheen
    { 0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, true,  true  }, // Sad
    { 0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, true,  true  }, // Dad
    { 0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, true,  true  }, // Tah
    { 0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, true,  true  }, // Zah
    { 0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, true,  true  }, // Ain
    { 0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, true,  true  }, // Ghain
    { 0x0640, 0x0640, 0x0640, 0x0640, 0x0640, true,  true  }, // Tatweel
    { 0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, true,  true  }, // Feh
    { 0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, true,  true  }, // Qaf
    { 0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, true,  true  }, // Kaf
    { 0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, true,  true  }, // Lam
    { 0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, true,  true  }, // Meem
    { 0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, true,  true  }, // Noon
    { 0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, true,  true  }, // Heh
    { 0x0648, 0xFEED, 0xFEEE, 0xFEED, 0xFEEE, true,  false }, // Waw
    { 0x0649, 0xFEEF, 0xFEF0, 0xFBE8, 0xFBE9, true,  true  }, // Alef Maksura
    { 0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, true,  true  }  // Yeh
};

static const ArabicGlyph* FindArabicGlyph(wchar_t c) {
    for (size_t i = 0; i < sizeof(g_ArabicTable) / sizeof(g_ArabicTable[0]); ++i) {
        if (g_ArabicTable[i].unicode == c) return &g_ArabicTable[i];
    }
    return nullptr;
}

static inline bool IsArabicChar(wchar_t c) {
    return (c >= 0x0600 && c <= 0x06FF);
}

static std::wstring ShapeArabicAndBidi(const std::wstring& input) {
    if (input.empty()) return input;

    bool hasArabic = false;
    for (wchar_t c : input) {
        if (IsArabicChar(c)) { hasArabic = true; break; }
    }
    if (!hasArabic) return input;

    size_t len = input.length();
    std::wstring shaped = input;

    // 1. Process Lam-Alef ligatures & cursive connections
    size_t w = 0;
    for (size_t r = 0; r < len; ++r) {
        wchar_t curr = input[r];

        if (curr == 0x0644 && (r + 1 < len)) {
            wchar_t next = input[r + 1];
            wchar_t lig = 0;
            if (next == 0x0622) lig = 0xFEF5; // Madda
            else if (next == 0x0623) lig = 0xFEF7; // Hamza above
            else if (next == 0x0625) lig = 0xFEF9; // Hamza below
            else if (next == 0x0627) lig = 0xFEFB; // Plain Alef

            if (lig != 0) {
                bool prevJoins = false;
                if (r > 0) {
                    const ArabicGlyph* pg = FindArabicGlyph(input[r - 1]);
                    if (pg && pg->leftJoin) prevJoins = true;
                }
                shaped[w++] = prevJoins ? (lig + 1) : lig;
                r++;
                continue;
            }
        }

        const ArabicGlyph* g = FindArabicGlyph(curr);
        if (!g) {
            shaped[w++] = curr;
            continue;
        }

        bool joinRight = false;
        if (r > 0) {
            const ArabicGlyph* pg = FindArabicGlyph(input[r - 1]);
            if (pg && pg->leftJoin && g->rightJoin) joinRight = true;
        }

        bool joinLeft = false;
        if (r + 1 < len) {
            const ArabicGlyph* ng = FindArabicGlyph(input[r + 1]);
            if (ng && ng->rightJoin && g->leftJoin) joinLeft = true;
        }

        if (joinRight && joinLeft) shaped[w++] = g->medialForm;
        else if (joinLeft)         shaped[w++] = g->initialForm;
        else if (joinRight)        shaped[w++] = g->finalForm;
        else                       shaped[w++] = g->isolated;
    }
    shaped.resize(w);

    // 2. Reverse contiguous Arabic segments (RTL display)
    size_t i = 0;
    while (i < shaped.length()) {
        if (shaped[i] >= 0x0600 || (shaped[i] >= 0xFB50 && shaped[i] <= 0xFEFC)) {
            size_t start = i;
            while (i < shaped.length() && 
                  ((shaped[i] >= 0x0600 || (shaped[i] >= 0xFB50 && shaped[i] <= 0xFEFC)) ||
                   shaped[i] == L' ' || shaped[i] == L'_' || shaped[i] == L'-')) {
                i++;
            }
            size_t end = i;
            while (end > start && shaped[end - 1] == L' ') end--;
            std::reverse(shaped.begin() + start, shaped.begin() + end);
        } else {
            i++;
        }
    }

    return shaped;
}

static wchar_t DecodeCP1256Byte(unsigned char b) {
    if (b < 0x80) return (wchar_t)b;
    switch (b) {
        case 0xC1: return 0x0621; // Hamza
        case 0xC2: return 0x0622; // Alef Madda
        case 0xC3: return 0x0623; // Alef Hamza Above
        case 0xC4: return 0x0624; // Waw Hamza
        case 0xC5: return 0x0625; // Alef Hamza Below
        case 0xC6: return 0x0626; // Yeh Hamza
        case 0xC7: return 0x0627; // Alef ('Ç' in 1252)
        case 0xC8: return 0x0628; // Beh
        case 0xC9: return 0x0629; // Teh Marbuta
        case 0xCA: return 0x062A; // Teh
        case 0xCB: return 0x062B; // Theh
        case 0xCC: return 0x062C; // Jeem
        case 0xCD: return 0x062D; // Hah
        case 0xCE: return 0x062E; // Khah ('Î' in 1252)
        case 0xCF: return 0x062F; // Dal
        case 0xD0: return 0x0630; // Thal
        case 0xD1: return 0x0631; // Reh
        case 0xD2: return 0x0632; // Zain
        case 0xD3: return 0x0633; // Seen
        case 0xD4: return 0x0634; // Sheen ('Ô' in 1252)
        case 0xD5: return 0x0635; // Sad
        case 0xD6: return 0x0636; // Dad
        case 0xD8: return 0x0637; // Tah
        case 0xD9: return 0x0638; // Zah
        case 0xDA: return 0x0639; // Ain
        case 0xDB: return 0x063A; // Ghain
        case 0xDC: return 0x0640; // Tatweel
        case 0xDD: return 0x0641; // Feh
        case 0xDE: return 0x0642; // Qaf
        case 0xDF: return 0x0643; // Kaf
        case 0xE1: return 0x0644; // Lam
        case 0xE3: return 0x0645; // Meem ('ã' in 1252)
        case 0xE4: return 0x0646; // Noon
        case 0xE5: return 0x0647; // Heh
        case 0xE6: return 0x0648; // Waw
        case 0xEC: return 0x0649; // Alef Maksura
        case 0xED: return 0x064A; // Yeh
        default: return 0;
    }
}

static bool ContainsCP1256Arabic(const char* str, size_t len) {
    if (!str || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char b = (unsigned char)str[i];
        if (DecodeCP1256Byte(b) >= 0x0621) return true;
    }
    return false;
}

static bool IsValidUTF8(const char* str, size_t len) {
    if (!str || len == 0) return false;
    size_t i = 0;
    bool hasMultibyte = false;
    while (i < len) {
        unsigned char b = (unsigned char)str[i];
        if (b <= 0x7F) {
            i++;
        } else if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= len || ((unsigned char)str[i + 1] & 0xC0) != 0x80) return false;
            hasMultibyte = true;
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= len || ((unsigned char)str[i + 1] & 0xC0) != 0x80 || ((unsigned char)str[i + 2] & 0xC0) != 0x80) return false;
            hasMultibyte = true;
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            if (i + 3 >= len || ((unsigned char)str[i + 1] & 0xC0) != 0x80 || ((unsigned char)str[i + 2] & 0xC0) != 0x80 || ((unsigned char)str[i + 3] & 0xC0) != 0x80) return false;
            hasMultibyte = true;
            i += 4;
        } else {
            return false;
        }
    }
    return hasMultibyte;
}

std::wstring ConvertToUnicodeAndShape(const char* str, int len) {
    if (!str) return L"";
    if (len < 0) len = (int)strlen(str);
    if (len == 0) return L"";

    std::wstring result;

    if (IsValidUTF8(str, len)) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, str, len, nullptr, 0);
        if (wlen > 0) {
            result.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, str, len, &result[0], wlen);
        }
    } else if (ContainsCP1256Arabic(str, len)) {
        result.reserve(len);
        for (int i = 0; i < len; ++i) {
            unsigned char b = (unsigned char)str[i];
            wchar_t c = DecodeCP1256Byte(b);
            if (c != 0) {
                result.push_back(c);
            } else {
                char temp[2] = { (char)b, 0 };
                wchar_t wtemp[2] = { 0 };
                MultiByteToWideChar(CP_ACP, 0, temp, 1, wtemp, 1);
                result.push_back(wtemp[0]);
            }
        }
    } else {
        int wlen = MultiByteToWideChar(CP_ACP, 0, str, len, nullptr, 0);
        if (wlen > 0) {
            result.resize(wlen);
            MultiByteToWideChar(CP_ACP, 0, str, len, &result[0], wlen);
        }
    }

    return ShapeArabicAndBidi(result);
}

const char* FuenteParaIdioma(const std::wstring& texto)
{
    for(wchar_t letra : texto){
        const bool esArabe = (letra >= 0x0600 && letra <= 0x06FF) || (letra >= 0xFB50 && letra <= 0xFEFC);
        const bool esAsiatico = (letra >= 0x4E00 && letra <= 0x9FFF)
            || (letra >= 0x3400 && letra <= 0x4DBF)
            || (letra >= 0x3040 && letra <= 0x30FF)
            || (letra >= 0xAC00 && letra <= 0xD7AF);
        const bool esIndio = letra >= 0x0900 && letra <= 0x0D7F;

        if(esArabe){
            return "Tahoma";
        }

        if(esAsiatico){
            return "Microsoft YaHei";
        }

        if(esIndio){
            return "Nirmala UI";
        }
    }

    return nullptr;
}
