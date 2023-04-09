#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <iostream> // DEBUG
#include <thread> // DEBUG

std::string hex(const std::byte* buffer, size_t size)
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

std::string strDump(const std::byte* buffer, size_t size)
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

std::string hexNonNull(const std::byte* buffer, size_t size)
{
    return hex(buffer, nonNullLength(buffer, size));
}

std::string strNonNull(const std::byte* buffer, size_t size)
{
    return strDump(buffer, nonNullLength(buffer, size));
}

namespace nms {
using PageVersion = uint32_t;

class Server {
public:
    bool connect(const std::string&)
    {
        return true;
    }

    void allocateRegion(const std::string& id, size_t numPages, size_t pageSize)
    {
        const auto it = regions_.find(id);
        if (it == regions_.end()) {
            regions_.emplace(id,
                Region {
                    id,
                    std::vector<std::byte>(pageSize * numPages),
                    pageSize,
                    std::vector<PageVersion>(numPages, 0),
                    std::make_unique<std::mutex>(),
                });
        } else {
            assert(it->second.data.size() == pageSize * numPages);
            assert(it->second.pageSize == pageSize);
        }
    }

    void destroyRegion(const std::string& id)
    {
        regions_.erase(id);
    }

    void lockRegion(const std::string& id)
    {
        auto& region = getRegion(id);
        region.mutex->lock();
        assert(!region.locked);
        region.locked = true;
    }

    void unlockRegion(const std::string& id)
    {
        auto& region = getRegion(id);
        region.mutex->unlock();
        assert(region.locked);
        region.locked = false;
    }

    const std::vector<PageVersion>& getPageVersions(const std::string& id) const
    {
        const auto& region = getRegion(id);
        return region.pageVersions;
    }

    PageVersion sendPage(const std::string& id, size_t pageIdx, const std::byte* buffer,
        const std::vector<uintptr_t>& addressOffsets)
    {
        auto& region = getRegion(id);
        assert(region.locked);
        const auto offset = pageIdx * region.pageSize;
        std::memcpy(region.data.data() + offset, buffer, region.pageSize);
        region.addressOffsets = addressOffsets;
        region.pageVersions[pageIdx]++;
        return region.pageVersions[pageIdx];
    }

    void receivePage(const std::string& id, std::byte* buffer,
        std::vector<uintptr_t>& addressOffsets, size_t pageIdx)
    {
        auto& region = getRegion(id);
        assert(region.locked);
        const auto offset = pageIdx * region.pageSize;
        std::memcpy(buffer, region.data.data() + offset, region.pageSize);
        addressOffsets = region.addressOffsets;
    }

    void addObject(const std::string& regionId, const std::string& objectId, uintptr_t offset)
    {
        auto& region = getRegion(regionId);
        assert(region.objects.count(objectId) == 0);
        region.objects.emplace(objectId, offset);
    }

    std::optional<uintptr_t> getObject(
        const std::string& regionId, const std::string& objectId) const
    {
        const auto& region = getRegion(regionId);
        const auto it = region.objects.find(objectId);
        if (it == region.objects.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    struct Region {
        std::string id;
        std::vector<std::byte> data;
        size_t pageSize;
        std::vector<PageVersion> pageVersions;
        std::unique_ptr<std::mutex> mutex;
        std::vector<uintptr_t> addressOffsets = {};
        std::unordered_map<std::string, uintptr_t> objects = {};
        bool locked = false;
    };

    Region& getRegion(const std::string& id)
    {
        const auto it = regions_.find(id);
        assert(it != regions_.end());
        return it->second;
    }

    const Region& getRegion(const std::string& id) const
    {
        const auto it = regions_.find(id);
        assert(it != regions_.end());
        return it->second;
    }

    std::unordered_map<std::string, Region> regions_;
};

class TrackedMemoryRegion {
public:
    TrackedMemoryRegion(size_t numPages)
        : data_(::getpagesize() * numPages)
        , preTrackingData_(::getpagesize() * numPages)
        , isPageDirty_(numPages, 0)
    {
    }

    std::byte* ptr()
    {
        return data_.data();
    }

    size_t size() const // in bytes
    {
        return data_.size();
    }

    void startTracking()
    {
        std::cout << "start tracking\n";
        std::memcpy(preTrackingData_.data(), data_.data(), data_.size());
    }

    void stopTracking()
    {
        std::cout << "stop tracking\n";
        for (size_t pageIdx = 0; pageIdx < isPageDirty_.size(); ++pageIdx) {
            const auto offset = pageIdx * ::getpagesize();
            isPageDirty_[pageIdx] = std::memcmp(preTrackingData_.data() + offset,
                                        data_.data() + offset, ::getpagesize())
                != 0;
            if (isPageDirty_[pageIdx]) {
                std::cout << "page " << pageIdx << " dirty\n";
            }
        }
    }

    bool isModified(size_t pageIndex) const
    {
        assert(pageIndex < isPageDirty_.size());
        return isPageDirty_[pageIndex];
    }

    std::optional<size_t> getNextModifiedPageIndex(size_t idx) const
    {
        while (!isModified(idx)) {
            idx++;
            if (idx >= isPageDirty_.size()) {
                return std::nullopt;
            }
        }
        return idx;
    }

private:
    std::vector<std::byte> data_;
    std::vector<std::byte> preTrackingData_;
    std::vector<uint8_t> isPageDirty_;
};

// Very simple linear allocator
class Allocator : public std::pmr::memory_resource {
public:
    Allocator(std::byte* buffer, size_t size, size_t offset)
        : buffer_(buffer)
        , size_(size)
        , offset_(offset)
    {
    }

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    void* do_allocate(size_t size, size_t alignment) override
    {
        std::cout << "allocate " << size << " at " << offset_ << "\n";

        offset_ += alignOffset(reinterpret_cast<uintptr_t>(buffer_ + offset_), alignment);

        if (offset_ + size > size_) {
            throw std::bad_alloc();
        }
        offset_ += size;
        return buffer_ + offset_;
    }

    void do_deallocate(void*, size_t, size_t) override
    {
        // pass
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
    {
        const auto otherAllocator = dynamic_cast<const Allocator*>(&other);
        if (!otherAllocator) {
            return false;
        }
        return buffer_ == otherAllocator->buffer_ && size_ == otherAllocator->size_;
    }

    size_t getAllocatedSize() const
    {
        return offset_;
    }

private:
    std::byte* buffer_;
    size_t size_;
    size_t offset_ = 0;
};

class SyncedMemoryRegion {
public:
    SyncedMemoryRegion(Server* server, std::string id, size_t numPages)
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

    ~SyncedMemoryRegion()
    {
        allocator_->~Allocator();
    }

    std::byte* ptr()
    {
        return mem_.ptr();
    }

    size_t size() const
    {
        return mem_.size();
    }

    void lock()
    {
        server_->lockRegion(id_);
        const auto& serverPageVersions = server_->getPageVersions(id_);
        for (size_t pageIdx = 0; pageIdx < pageVersions_.size(); ++pageIdx) {
            // This intentionally does nothing for the first time, when the version is 0.
            if (serverPageVersions[pageIdx] > pageVersions_[pageIdx]) {
                std::cout << "receive page " << pageIdx
                          << " (server version: " << serverPageVersions[pageIdx]
                          << ", local version: " << pageVersions_[pageIdx] << ")" << std::endl;
                receivePage(pageIdx);
                pageVersions_[pageIdx] = serverPageVersions[pageIdx];
            }
        }
        mem_.startTracking();
    }

    void unlock()
    {
        mem_.stopTracking();
        auto pageIdx = mem_.getNextModifiedPageIndex(0);
        while (pageIdx) {
            std::cout << "modified page " << *pageIdx << "\n";
            pageVersions_[*pageIdx] = sendPage(*pageIdx);
            pageIdx = mem_.getNextModifiedPageIndex(*pageIdx + 1);
        }
        server_->unlockRegion(id_);
    }

    Allocator* getAllocator()
    {
        return allocator_;
    }

    template <typename T, typename... Args>
    T* getObject(const std::string& objectId, Args&&... args)
    {
        const auto offset = server_->getObject(id_, objectId);
        if (offset) {
            // Obviously UB again. Nice.
            return reinterpret_cast<T*>(mem_.ptr() + *offset);
        } else {
            auto obj = std::pmr::polymorphic_allocator<>(allocator_)
                           .new_object<T>(std::forward<Args>(args)...);
            server_->addObject(id_, objectId, reinterpret_cast<std::byte*>(obj) - mem_.ptr());
            return obj;
        }
    }

private:
    void receivePage(size_t pageIdx)
    {
        std::vector<std::byte> buffer(::getpagesize());
        std::vector<uintptr_t> addressOffsets;
        server_->receivePage(id_, buffer.data(), addressOffsets, pageIdx);

        std::cout << "page after receive\n" << hexNonNull(buffer.data(), buffer.size()) << "\n";

        // Make all the addresses absolute again
        for (const auto off : addressOffsets) {
            const auto ptr = buffer.data() + off;
            const auto relAddr = *reinterpret_cast<const uintptr_t*>(ptr);
            const std::byte* absAddr = relAddr + mem_.ptr();
            std::memcpy(ptr, &absAddr, sizeof(absAddr));
        }

        std::cout << "page after addr fixup\n" << hexNonNull(buffer.data(), buffer.size()) << "\n";

        const auto offset = pageIdx * ::getpagesize();
        std::memcpy(mem_.ptr() + offset, buffer.data(), ::getpagesize());
    }

    PageVersion sendPage(size_t pageIdx)
    {
        std::vector<std::byte> buffer(::getpagesize());
        const auto offset = pageIdx * ::getpagesize();
        std::memcpy(buffer.data(), mem_.ptr() + offset, ::getpagesize());

        std::cout << "page before addr fixup\n" << hexNonNull(buffer.data(), buffer.size()) << "\n";
        std::cout << "memory region: " << mem_.ptr() << " - " << mem_.ptr() + mem_.size() << "\n";

        // Make all addresses relative and save their offsets
        std::vector<uintptr_t> addressOffsets;
        constexpr auto ptrAlignment = std::alignment_of_v<std::byte*>;
        auto ptr
            = buffer.data() + alignOffset(reinterpret_cast<uintptr_t>(mem_.ptr()), ptrAlignment);
        // Look at all addresses that are aligned like a pointer
        for (; ptr < buffer.data() + buffer.size(); ptr += ptrAlignment) {
            // Find everything that has a pointer value inside this region
            // Pretty sure this violates strict aliasing rules and is therefore UB
            const auto absAddr = *reinterpret_cast<const std::byte**>(ptr);
            if (absAddr >= mem_.ptr() && absAddr < mem_.ptr() + mem_.size()) {
                std::cout << absAddr << std::endl;
                addressOffsets.push_back(ptr - buffer.data());
                const uintptr_t relAddr = absAddr - mem_.ptr();
                std::memcpy(ptr, &relAddr, sizeof(relAddr));
            }
        }

        std::cout << "page after addr fixup\n" << hexNonNull(buffer.data(), buffer.size()) << "\n";

        return server_->sendPage(id_, pageIdx, buffer.data(), addressOffsets);
    }

    Server* server_;
    std::string id_;
    TrackedMemoryRegion mem_;
    Allocator* allocator_;
    std::vector<PageVersion> pageVersions_;
};
}

#include <iostream>

void client(std::string_view clientId, nms::Server* server)
{
    std::cout << "enter thread " << clientId << "\n";

    nms::SyncedMemoryRegion region(server, "test", 16);

    auto hexRegion
        = [&region]() { return hex(region.ptr(), region.getAllocator()->getAllocatedSize()); };
    auto regionOffset = [&region](const auto* ptr) {
        return reinterpret_cast<const std::byte*>(ptr) - region.ptr();
    };

    struct TestStruct {
        std::pmr::string str;
        unsigned int num;
    };

    std::cout << "pre lock\n" << hexRegion() << "\n";
    region.lock();
    std::cout << "post lock\n" << hexRegion() << "\n";
    auto vec = region.getObject<std::pmr::vector<TestStruct>>("vec");
    std::cout << "post vector creation\n" << hexRegion() << "\n";
    std::cout << "vector address: " << regionOffset(vec) << "\n";
    region.unlock();
    std::cout << "post unlock\n" << hexRegion() << "\n";

    unsigned int counter = 0;
    while (true) {
        std::cout << "loop thread " << clientId << "\n";

        std::cout << "pre loop lock\n" << hexRegion() << "\n";
        region.lock();
        std::cout << "post loop lock\n" << hexRegion() << "\n";

        std::pmr::string name("clientId=", region.getAllocator());
        std::cout << "post string creation\n" << hexRegion() << "\n";
        name.append(clientId);
        std::cout << "post string append\n" << hexRegion() << "\n";
        std::cout << "vec size: " << vec->size() << "\n";
        vec->push_back(TestStruct { std::move(name), counter++ });
        std::cout << "post push_back\n" << hexRegion() << "\n";

        for (const auto& elem : *vec) {
            std::cout << " " << elem.str << ": " << elem.num << "\n";
        }

        for (const auto& elem : *vec) {
            std::cout << "element offset: " << regionOffset(&elem) << "\n";
            std::cout << "str offset: " << regionOffset(&elem.str) << "\n";
            std::cout << "data offset: " << regionOffset(elem.str.data()) << "\n";
        }

        std::cout << "pre loop unlock\n" << hexRegion() << "\n";
        region.unlock();
        std::cout << "post loop unlock\n" << hexRegion() << "\n";
        std::cout << "post loop unlock str\n" << strNonNull(region.ptr(), region.size()) << "\n";
        // TODO: region.quickUnlock(); just tells the server it's contents are outdated,
        // but terminates immediately and synchronizes in the background.
        std::cout << "\n\n";

        ::usleep(1000 * 1000);

        if (counter == 4) {
            break;
        }
    }
}

#include <thread>

int main(int, char**)
{
    nms::Server server;
    server.connect("localhost:6969");

    std::thread t1 { [&server]() { client("FOOBAR", &server); } };
    ::usleep(1000 * 500);
    std::thread t2 { [&server]() { client("BAZBAZ", &server); } };

    t1.join();
    t2.join();
}
