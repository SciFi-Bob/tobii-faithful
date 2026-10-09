#pragma once

#include <cstdint>

namespace tobii::usb {

void CopyRealm2712Key(std::uint8_t out[16]);

void CopyRealmResponseTrailer(std::uint8_t out[4]);

}
