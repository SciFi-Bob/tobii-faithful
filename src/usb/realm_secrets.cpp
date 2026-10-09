#include "tobii/usb/realm_secrets.hpp"

#include <cstring>

namespace tobii::usb {

// The vendor handshake constants are not included in this tree.
void CopyRealm2712Key(std::uint8_t out[16]) {
    std::memset(out, 0, 16);
}

void CopyRealmResponseTrailer(std::uint8_t out[4]) {
    std::memset(out, 0, 4);
}

} // namespace tobii::usb
