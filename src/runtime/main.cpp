#include "runtime/RuntimeApp.hpp"

// SDK-provided entry point. A consumer's controller is linked in via its CIF
// symbols (CIF_* buffers, applicatieprogramma, getCifItemName); the container
// runtime drives it. External controllers need no pre-init hook — their
// object mapping comes from getCifItemName(), not a config file.
int main(int argc, char* argv[]) {
    return tlcfi::runtime::runContainer(argc, argv);
}
