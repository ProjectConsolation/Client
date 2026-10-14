#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <cstring>

namespace network_security
{
    // Partial node_t, attributed to KisakCOD qcommon/huffman.cpp and verified
    // against QoS PC 1.1 103ECBA0: children +0/+4, symbol +16 (257 internal).
    struct huffman_node
    {
        const huffman_node* left;
        const huffman_node* right;
        std::uint32_t unused[2];
        int symbol;
    };
    static_assert(sizeof(void*) != 4 || offsetof(huffman_node, symbol) == 16);

    inline int decompress(const huffman_node* root, std::span<const std::uint8_t> input,
                          std::span<std::uint8_t> output)
    {
        if (!root || input.size() > 0x20000) return -1;
        std::size_t bit = 0, count = 0;
        while (bit < input.size() * 8)
        {
            const auto previous = bit;
            auto* node = root;
            unsigned depth = 0;
            while (node && node->symbol == 257)
            {
                // Like KisakCOD's bounded receive: a padded incomplete final
                // code is not a symbol. Never read beyond the compressed input.
                if (bit == input.size() * 8) return static_cast<int>(count);
                if (++depth > 256) return -1;
                const bool right = (input[bit / 8] >> (bit % 8)) & 1;
                ++bit;
                node = right ? node->right : node->left;
            }
            if (!node || node->symbol < 0 || node->symbol > 255 || bit == previous
                || count == output.size()) return -1;
            output[count++] = static_cast<std::uint8_t>(node->symbol);
        }
        return static_cast<int>(count);
    }

    inline bool message_valid(const std::uint32_t* msg, std::uint32_t limit)
    {
        return msg && !msg[0] && msg[2] && msg[4] <= limit && msg[5] <= msg[4]
            && msg[7] <= msg[5] && !msg[6];
    }

    using netchan_process = int(__cdecl*)(std::uint32_t*, std::uint32_t*);
    inline int fragment_guard(netchan_process native, std::uint32_t* channel, std::uint32_t* msg)
    {
        if (!channel || !message_valid(msg, 0x20000) || msg[5] < 4 || msg[4] < 4) return 0;
        std::uint32_t sequence = 0;
        std::memcpy(&sequence, reinterpret_cast<const void*>(msg[2]), 4);
        const bool fragmented = (sequence & 0x80000000u) != 0;
        if (fragmented && (msg[5] < 10 || channel[8] > channel[10] || channel[10] > 0x20000 || !channel[9])) return 0;
        const auto capacity = msg[4];
        if (fragmented) msg[4] = capacity - 4;
        const auto result = native(channel, msg);
        msg[4] = capacity;
        return result;
    }

    // QoS 103047A8 pushes six DWORDs and cleans 0x18 bytes. IDA's phantom
    // char@dil parameter is NOT a stack argument.
    using member_join = int(__cdecl*)(std::uint32_t*, int, std::int64_t, int, std::uint32_t*);
    inline int join_guard(member_join native, std::uint32_t* party, int controller,
                          std::int64_t address, int address_tail, std::uint32_t* msg)
    {
        if (!party || !message_valid(msg, 0x20000) || msg[5] - msg[7] > 0x4000) return 0;
        return native(party, controller, address, address_tail, msg);
    }
}
