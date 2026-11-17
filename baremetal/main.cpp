// ベアメタル版のエントリポイント (Circle の標準的な形)
#include "kernel.h"

#include <circle/startup.h>

int main(void) {
    CKernel kernel;
    if (!kernel.Initialize()) {
        halt();
        return EXIT_HALT;
    }
    TShutdownMode mode = kernel.Run();
    switch (mode) {
    case ShutdownReboot:
        reboot();
        return EXIT_REBOOT;
    case ShutdownHalt:
    default:
        halt();
        return EXIT_HALT;
    }
}
