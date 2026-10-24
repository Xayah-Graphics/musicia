#include <windows.h>
#include <bcrypt.h>

import std;

int wmain(int count, wchar_t** arguments) {
    SetConsoleOutputCP(CP_UTF8);
    bool failed{};
    for (int argument = 1; argument < count; ++argument) {
        const std::filesystem::path source{arguments[argument]};
        try {
            std::ifstream input;
            input.exceptions(std::ios::badbit | std::ios::failbit);
            input.open(source, std::ios::binary);
            input.seekg(10);
            std::uint32_t key_size{};
            input.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
            std::vector<unsigned char> encrypted_key(key_size), key_data(key_size);
            input.read(reinterpret_cast<char*>(encrypted_key.data()), key_size);
            for (auto& byte : encrypted_key) byte ^= 0x64;

            std::array<unsigned char, 16> core_key{0x68, 0x7A, 0x48, 0x52, 0x41, 0x6D, 0x73, 0x6F, 0x35, 0x6B, 0x49, 0x6E, 0x62, 0x61, 0x78, 0x57};
            BCRYPT_KEY_HANDLE aes_key{};
            auto status = BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &aes_key, nullptr, 0, core_key.data(), static_cast<ULONG>(core_key.size()), 0);
            if (status < 0) throw std::runtime_error(std::format("Cannot initialize AES: 0x{:08X}", static_cast<unsigned long>(status)));
            const std::unique_ptr<void, decltype(&BCryptDestroyKey)> key_handle(aes_key, BCryptDestroyKey);
            ULONG decrypted_size{};
            status = BCryptDecrypt(aes_key, encrypted_key.data(), key_size, nullptr, nullptr, 0, key_data.data(), key_size, &decrypted_size, BCRYPT_BLOCK_PADDING);
            if (status < 0) throw std::runtime_error(std::format("Cannot decrypt the NCM key: 0x{:08X}", static_cast<unsigned long>(status)));
            const auto stream_key = std::span(key_data).subspan(17, decrypted_size - 17);
            std::array<unsigned char, 256> key_box{}, key_stream{};
            std::iota(key_box.begin(), key_box.end(), static_cast<unsigned char>(0));
            unsigned slot{};
            for (unsigned index = 0; index < key_box.size(); ++index) {
                slot = (slot + key_box[index] + stream_key[index % stream_key.size()]) & 255;
                std::swap(key_box[index], key_box[slot]);
            }
            for (unsigned index = 0; index < key_stream.size(); ++index) {
                const unsigned next = (index + 1) & 255;
                key_stream[index] = key_box[(key_box[next] + key_box[(key_box[next] + next) & 255]) & 255];
            }

            std::uint32_t metadata_size{}, cover_frame_size{};
            input.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
            input.seekg(static_cast<std::streamoff>(metadata_size) + 5, std::ios::cur);
            input.read(reinterpret_cast<char*>(&cover_frame_size), sizeof(cover_frame_size));
            input.seekg(static_cast<std::streamoff>(cover_frame_size) + 4, std::ios::cur);
            const auto audio_start = static_cast<std::uintmax_t>(input.tellg());
            std::array<unsigned char, 4> header{};
            input.read(reinterpret_cast<char*>(header.data()), header.size());
            for (std::size_t index = 0; index < header.size(); ++index) header[index] ^= key_stream[index];
            auto destination = source;
            if (std::ranges::equal(header, std::array<unsigned char, 4>{'f', 'L', 'a', 'C'})) destination.replace_extension(L".flac");
            else if ((header[0] == 'I' && header[1] == 'D' && header[2] == '3') || (header[0] == 0xFF && (header[1] & 0xE0) == 0xE0 && (header[1] & 0x18) != 0x08 && (header[1] & 0x06) == 0x02)) destination.replace_extension(L".mp3");
            else throw std::runtime_error("The NCM audio payload is neither FLAC nor MP3.");

            std::ofstream output;
            output.exceptions(std::ios::badbit | std::ios::failbit);
            try {
                output.open(destination, std::ios::binary | std::ios::noreplace);
            } catch (const std::ios_base::failure&) {
                throw std::runtime_error("Cannot create the output file. An existing file will not be overwritten.");
            }
            input.seekg(static_cast<std::streamoff>(audio_start));
            const auto audio_size = std::filesystem::file_size(source) - audio_start;
            std::vector<unsigned char> buffer(256 * 1024);
            for (std::uintmax_t offset = 0; offset < audio_size;) {
                const auto bytes = static_cast<std::size_t>(std::min<std::uintmax_t>(buffer.size(), audio_size - offset));
                input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytes));
                for (std::size_t index = 0; index < bytes; ++index) buffer[index] ^= key_stream[(offset + index) & 255];
                output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(bytes));
                offset += bytes;
            }
            output.close();
        } catch (const std::exception& exception) {
            const auto path = source.u8string();
            std::println(std::cerr, "{}: {}", std::string(path.begin(), path.end()), exception.what());
            failed = true;
        }
    }
    DWORD process{};
    if (failed && GetConsoleProcessList(&process, 1) == 1) {
        std::println(std::cerr, "Press Enter to close.");
        std::cin.get();
    }
    return failed ? 1 : 0;
}
