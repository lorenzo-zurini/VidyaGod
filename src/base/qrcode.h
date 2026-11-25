#ifndef QRCODE_H
#define QRCODE_H

#include <string>
#include <vector>

//A QR code (ISO/IEC 18004) of a short text: byte mode, error correction M, the smallest version 1–10 that holds it
//(up to 213 bytes), the mask a phone's reader likes best. What a peer code is shown as on a screen another device
//reads with its camera.
namespace QrCode {

//Its modules, row by row (true: dark), without the quiet zone; empty when the text does not fit version 10.
std::vector<std::vector<bool>> Encode(const std::string &Text);

} // namespace QrCode

#endif // QRCODE_H
