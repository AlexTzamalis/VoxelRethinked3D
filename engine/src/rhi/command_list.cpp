#include "vxe/rhi/command_list.hpp"

namespace vxe::rhi {

void CommandList::appendHeader(CommandType type, usize payloadSize) {
    const Header header{.type = type, .reserved = 0, .size = static_cast<u32>(payloadSize)};
    append(&header, sizeof(Header));
    ++m_count;
}

void CommandList::append(const void* data, usize size) {
    const usize offset = m_bytes.size();
    m_bytes.resize(offset + size);
    std::memcpy(m_bytes.data() + offset, data, size);
}

void CommandList::beginDebugGroup(std::string_view name) {
    const cmd::BeginDebugGroup header{.length = static_cast<u32>(name.size())};
    appendHeader(CommandType::BeginDebugGroup, sizeof(header) + name.size());
    append(&header, sizeof(header));
    append(name.data(), name.size());
}

CommandView CommandList::Iterator::operator*() const noexcept {
    Header header{};
    std::memcpy(&header, m_bytes.data() + m_offset, sizeof(Header));
    return CommandView{.type = header.type,
                       .payload = m_bytes.subspan(m_offset + sizeof(Header), header.size)};
}

CommandList::Iterator& CommandList::Iterator::operator++() noexcept {
    Header header{};
    std::memcpy(&header, m_bytes.data() + m_offset, sizeof(Header));
    m_offset += sizeof(Header) + header.size;
    return *this;
}

} // namespace vxe::rhi
