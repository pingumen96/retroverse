// SDL_main.h provides the real entry point (main/WinMain), converts the Windows command line to UTF-8 and calls the
// main() below, which it renames to SDL_main.
#include <SDL3/SDL_main.h>

#include <rv/app/app.hpp>

// Hybrid-GPU laptops: ask the NVIDIA Optimus and AMD PowerXpress drivers to run Retroverse on the discrete GPU. The
// drivers only look for these symbols in the executable's export table.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char* argv[]) {
    return rv::app::run(argc, argv);
}
