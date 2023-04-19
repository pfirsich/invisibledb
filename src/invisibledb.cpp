#include "invisibledb.hpp"

#include <cassert>
#include <cstring>

namespace {
template <typename T, typename U>
T alignOffset(T v, U alignment)
{
    const auto misalignment = v % alignment;
    return misalignment > 0 ? alignment - misalignment : 0;
}

size_t nonNullLength(const std::byte* buffer, size_t size)
{
    size_t lastNonNull = 0;
    for (size_t i = 0; i < size; ++i) {
        if (static_cast<uint8_t>(buffer[i]) != 0) {
            lastNonNull = i;
        }
    }
    auto numNonNull = lastNonNull + 1;
    numNonNull += alignOffset(numNonNull, std::alignment_of_v<max_align_t>);
    return numNonNull;
}
}

namespace util {
std::string toHex(const std::byte* buffer, size_t size)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string ret;
    for (size_t i = 0; i < size; ++i) {
        const auto v = static_cast<uint8_t>(buffer[i]);
        ret.push_back(digits[(v & 0xF0) >> 4]);
        ret.push_back(digits[(v & 0x0F) >> 0]);
    }
    return ret;
}

std::string toStr(const std::byte* buffer, size_t size)
{
    const auto ptr = reinterpret_cast<const char*>(buffer);
    std::string ret;
    for (size_t i = 0; i < size; ++i) {
        if (std::isprint(ptr[i])) {
            ret.push_back(ptr[i]);
        } else {
            ret.push_back('.');
        }
    }
    return ret;
}

std::string toHexNonNull(const std::byte* buffer, size_t size)
{
    return toHex(buffer, nonNullLength(buffer, size));
}

std::string toStrNonNull(const std::byte* buffer, size_t size)
{
    return toStr(buffer, nonNullLength(buffer, size));
}
}

namespace invisibledb {
using namespace util;

bool Server::connect(const std::string&)
{
    return true;
}

void Server::allocateRegion(const std::string& id, size_t numPages, size_t pageSize)
{
    std::lock_guard g(mapMutex_);
    const auto it = regions_.find(id);
    if (it == regions_.end()) {
        regions_.emplace(id,
            Region {
                id,
                std::vector<std::byte>(pageSize * numPages),
                pageSize,
                std::vector<PageVersion>(numPages, 0),
                std::vector<std::vector<uintptr_t>>(numPages),
                std::make_unique<std::mutex>(),
            });
    } else {
        assert(it->second.data.size() == pageSize * numPages);
        assert(it->second.pageSize == pageSize);
    }
}

void Server::destroyRegion(const std::string& id)
{
    std::lock_guard g(mapMutex_);
    regions_.erase(id);
}

void Server::lockRegion(const std::string& id)
{
    auto& region = getRegion(id);
    region.mutex->lock();
    assert(!region.locked);
    region.locked = true;
}

void Server::unlockRegion(const std::string& id)
{
    auto& region = getRegion(id);
    assert(region.locked);
    region.locked = false;
    region.mutex->unlock();
}

const std::vector<PageVersion>& Server::getPageVersions(const std::string& id) const
{
    const auto& region = getRegion(id);
    return region.pageVersions;
}

PageVersion Server::sendPage(const std::string& id, size_t pageIdx, const std::byte* buffer,
    const std::vector<uintptr_t>& addressOffsets)
{
    auto& region = getRegion(id);
    assert(region.locked);
    const auto offset = pageIdx * region.pageSize;
    std::memcpy(region.data.data() + offset, buffer, region.pageSize);
    region.addressOffsets[pageIdx] = addressOffsets;
    region.pageVersions[pageIdx]++;
    return region.pageVersions[pageIdx];
}

void Server::receivePage(const std::string& id, std::byte* buffer,
    std::vector<uintptr_t>& addressOffsets, size_t pageIdx)
{
    auto& region = getRegion(id);
    assert(region.locked);
    const auto offset = pageIdx * region.pageSize;
    std::memcpy(buffer, region.data.data() + offset, region.pageSize);
    addressOffsets = region.addressOffsets[pageIdx];
}

void Server::addObject(const std::string& regionId, const std::string& objectId, uintptr_t offset)
{
    auto& region = getRegion(regionId);
    assert(region.objects.count(objectId) == 0);
    region.objects.emplace(objectId, offset);
}

std::optional<uintptr_t> Server::getObject(
    const std::string& regionId, const std::string& objectId) const
{
    const auto& region = getRegion(regionId);
    const auto it = region.objects.find(objectId);
    if (it == region.objects.end()) {
        return std::nullopt;
    }
    return it->second;
}

Server::Region& Server::getRegion(const std::string& id)
{
    std::lock_guard g(mapMutex_);
    const auto it = regions_.find(id);
    assert(it != regions_.end());
    return it->second;
}

const Server::Region& Server::getRegion(const std::string& id) const
{
    std::lock_guard g(mapMutex_);
    const auto it = regions_.find(id);
    assert(it != regions_.end());
    return it->second;
}

TrackedMemoryRegion::TrackedMemoryRegion(size_t numPages)
    : data_(::getpagesize() * numPages)
    , preTrackingData_(::getpagesize() * numPages)
    , isPageDirty_(numPages, 0)
{
}

std::byte* TrackedMemoryRegion::ptr()
{
    return data_.data();
}

size_t TrackedMemoryRegion::size() const
{
    return data_.size();
}

void TrackedMemoryRegion::startTracking()
{
    std::memcpy(preTrackingData_.data(), data_.data(), data_.size());
}

void TrackedMemoryRegion::stopTracking()
{
    for (size_t pageIdx = 0; pageIdx < isPageDirty_.size(); ++pageIdx) {
        const auto offset = pageIdx * ::getpagesize();
        const auto cmp
            = std::memcmp(preTrackingData_.data() + offset, data_.data() + offset, ::getpagesize());
        isPageDirty_[pageIdx] = cmp != 0;
    }
}

bool TrackedMemoryRegion::isModified(size_t pageIndex) const
{
    assert(pageIndex < isPageDirty_.size());
    return isPageDirty_[pageIndex];
}

std::optional<size_t> TrackedMemoryRegion::getNextModifiedPageIndex(size_t idx) const
{
    while (!isModified(idx)) {
        idx++;
        if (idx >= isPageDirty_.size()) {
            return std::nullopt;
        }
    }
    return idx;
}

Allocator::Allocator(std::byte* buffer, size_t size, size_t offset)
    : buffer_(buffer)
    , size_(size)
    , offset_(offset)
{
}

void* Allocator::do_allocate(size_t size, size_t alignment)
{
    offset_ += alignOffset(reinterpret_cast<uintptr_t>(buffer_ + offset_), alignment);

    if (offset_ + size > size_) {
        throw std::bad_alloc();
    }
    offset_ += size;
    return buffer_ + offset_;
}

void Allocator::do_deallocate(void*, size_t, size_t)
{
    // pass
}

bool Allocator::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    const auto otherAllocator = dynamic_cast<const Allocator*>(&other);
    if (!otherAllocator) {
        return false;
    }
    return buffer_ == otherAllocator->buffer_ && size_ == otherAllocator->size_;
}

size_t Allocator::getAllocatedSize() const
{
    return offset_;
}

SyncedMemoryRegion::SyncedMemoryRegion(Server* server, std::string id, size_t numPages)
    : server_(server)
    , id_(std::move(id))
    , mem_(numPages)
    , pageVersions_(numPages, 0)
{
    // Allocators are inside the memory region, so they are synchronized.
    // Also pmr containers keep a pointer to the memory resource, which should not point
    // outside of the memory region, so it needs to live in there.
    allocator_ = new (mem_.ptr()) Allocator(mem_.ptr(), mem_.size(), sizeof(Allocator));
    assert(server_);
    server_->allocateRegion(id_, numPages, ::getpagesize());
}

SyncedMemoryRegion::~SyncedMemoryRegion()
{
    allocator_->~Allocator();
}

std::byte* SyncedMemoryRegion::ptr()
{
    return mem_.ptr();
}

size_t SyncedMemoryRegion::size() const
{
    return mem_.size();
}

void SyncedMemoryRegion::lock()
{
    server_->lockRegion(id_);
    const auto& serverPageVersions = server_->getPageVersions(id_);
    for (size_t pageIdx = 0; pageIdx < pageVersions_.size(); ++pageIdx) {
        // This intentionally does nothing for the first time, when the version is 0.
        if (serverPageVersions[pageIdx] > pageVersions_[pageIdx]) {
            receivePage(pageIdx);
            pageVersions_[pageIdx] = serverPageVersions[pageIdx];
        }
    }
    mem_.startTracking();
}

void SyncedMemoryRegion::unlock()
{
    mem_.stopTracking();
    auto pageIdx = mem_.getNextModifiedPageIndex(0);
    while (pageIdx) {
        pageVersions_[*pageIdx] = sendPage(*pageIdx);
        pageIdx = mem_.getNextModifiedPageIndex(*pageIdx + 1);
    }
    server_->unlockRegion(id_);
}

Allocator* SyncedMemoryRegion::getAllocator()
{
    return allocator_;
}

void SyncedMemoryRegion::receivePage(size_t pageIdx)
{
    std::vector<std::byte> buffer(::getpagesize());
    std::vector<uintptr_t> addressOffsets;
    server_->receivePage(id_, buffer.data(), addressOffsets, pageIdx);

    // Make all the addresses absolute again
    for (const auto off : addressOffsets) {
        const auto ptr = buffer.data() + off;
        const auto relAddr = *reinterpret_cast<const uintptr_t*>(ptr);
        const std::byte* absAddr = relAddr + mem_.ptr();
        std::memcpy(ptr, &absAddr, sizeof(absAddr));
    }

    const auto offset = pageIdx * ::getpagesize();
    std::memcpy(mem_.ptr() + offset, buffer.data(), ::getpagesize());
}

PageVersion SyncedMemoryRegion::sendPage(size_t pageIdx)
{
    std::vector<std::byte> buffer(::getpagesize());
    const auto offset = pageIdx * ::getpagesize();
    std::memcpy(buffer.data(), mem_.ptr() + offset, ::getpagesize());

    // Make all addresses relative and save their offsets
    std::vector<uintptr_t> addressOffsets;
    constexpr auto ptrAlignment = std::alignment_of_v<std::byte*>;
    auto ptr = buffer.data() + alignOffset(reinterpret_cast<uintptr_t>(mem_.ptr()), ptrAlignment);
    // Look at all addresses that are aligned like a pointer
    for (; ptr < buffer.data() + buffer.size(); ptr += ptrAlignment) {
        // Find everything that has a pointer value inside this region
        // Pretty sure this violates strict aliasing rules and is therefore UB
        const auto absAddr = *reinterpret_cast<const std::byte**>(ptr);
        if (absAddr >= mem_.ptr() && absAddr < mem_.ptr() + mem_.size()) {
            const auto offset = ptr - buffer.data();
            addressOffsets.push_back(offset);
            const uintptr_t relAddr = absAddr - mem_.ptr();
            std::memcpy(ptr, &relAddr, sizeof(relAddr));
        }
    }

    return server_->sendPage(id_, pageIdx, buffer.data(), addressOffsets);
}
}
