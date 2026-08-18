#pragma once
// Ungepufferte Ablaufverfolgung fuer die Fehlersuche. Jede Zeile wird sofort
// geschrieben und geflusht -- stuerzt der Prozess ab, steht die letzte erreichte
// Stelle in der Datei. Aktiv nur, wenn PS_TRACE_FILE gesetzt ist.
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace PS {
    inline std::FILE* TraceFile()
    {
        static std::FILE* File = []() -> std::FILE* {
            const char* Path = std::getenv("PS_TRACE_FILE");
            return Path ? std::fopen(Path, "w") : nullptr;
        }();
        return File;
    }

    inline void Trace(const char* Format, ...)
    {
        std::FILE* File = TraceFile();
        if (!File) return;
        va_list Args;
        va_start(Args, Format);
        std::vfprintf(File, Format, Args);
        va_end(Args);
        std::fputc(10, File);
        std::fflush(File);
    }
}
