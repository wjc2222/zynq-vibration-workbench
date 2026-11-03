#include "vibration/sha256.hpp"
#include <iostream>
#include <stdexcept>

void check(const std::string& text, const std::string& expected) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    if (vibration::sha256_hex(bytes, text.size()) != expected) throw std::runtime_error("one-shot SHA mismatch");
    vibration::Sha256 incremental;
    for (std::size_t i = 0; i < text.size(); ++i) incremental.update(bytes+i, 1);
    if (incremental.hex_digest() != expected || incremental.hex_digest() != expected)
        throw std::runtime_error("incremental or repeated SHA digest mismatch");
}
int main() {
    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check(std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    vibration::Sha256 state; state.update(reinterpret_cast<const std::uint8_t*>("a"),1);
    (void)state.hex_digest(); state.update(reinterpret_cast<const std::uint8_t*>("bc"),2);
    if (state.hex_digest() != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        throw std::runtime_error("digest changed incremental state");
    std::cout << "SHA256 known vectors, incremental, repeated digest: PASS\n";
}
