#pragma once

#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace util {
std::string toHex(const std::byte* buffer, size_t size);
std::string toStr(const std::byte* buffer, size_t size);
std::string toHexNonNull(const std::byte* buffer, size_t size);
std::string toStrNonNull(const std::byte* buffer, size_t size);
}

namespace invisibledb {
using PageVersion = uint32_t;

class Server {
public:
    bool connect(const std::string& address);

    void allocateRegion(const std::string& id, size_t numPages, size_t pageSize);
    void destroyRegion(const std::string& id);

    void lockRegion(const std::string& id);
    void unlockRegion(const std::string& id);

    const std::vector<PageVersion>& getPageVersions(const std::string& id) const;

    PageVersion sendPage(const std::string& id, size_t pageIdx, const std::byte* buffer,
        const std::vector<uintptr_t>& addressOffsets);

    void receivePage(const std::string& id, std::byte* buffer,
        std::vector<uintptr_t>& addressOffsets, size_t pageIdx);

    void addObject(const std::string& regionId, const std::string& objectId, uintptr_t offset);

    std::optional<uintptr_t> getObject(
        const std::string& regionId, const std::string& objectId) const;

private:
    struct Region {
        std::string id;
        std::vector<std::byte> data;
        size_t pageSize;
        std::vector<PageVersion> pageVersions;
        std::vector<std::vector<uintptr_t>> addressOffsets;
        std::unique_ptr<std::mutex> mutex;
        std::unordered_map<std::string, uintptr_t> objects = {};
        bool locked = false;
    };

    Region& getRegion(const std::string& id);
    const Region& getRegion(const std::string& id) const;

    mutable std::mutex mapMutex_;
    // We use a std::map, so the pointers to the Regions are stable and we
    // can keep a single region locked without also locking mapMutex_.
    // If we used an unordered_map, we would have to prevent modification
    // of that map as long as a region is locked.
    std::map<std::string, Region> regions_;
};

class TrackedMemoryRegion {
public:
    TrackedMemoryRegion(size_t numPages);

    std::byte* ptr();
    size_t size() const; // in bytes

    void startTracking();
    void stopTracking();

    bool isModified(size_t pageIndex) const;

    std::optional<size_t> getNextModifiedPageIndex(size_t idx) const;

private:
    std::vector<std::byte> data_;
    std::vector<std::byte> preTrackingData_;
    std::vector<uint8_t> isPageDirty_;
};

// Very simple linear allocator
class Allocator : public std::pmr::memory_resource {
public:
    Allocator(std::byte* buffer, size_t size, size_t offset);

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    void* do_allocate(size_t size, size_t alignment) override;
    void do_deallocate(void*, size_t, size_t) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    size_t getAllocatedSize() const;

private:
    std::byte* buffer_;
    size_t size_;
    size_t offset_ = 0;
};

class SyncedMemoryRegion {
public:
    SyncedMemoryRegion(Server* server, std::string id, size_t numPages);
    ~SyncedMemoryRegion();

    std::byte* ptr();
    size_t size() const;

    void lock();
    void unlock();

    Allocator* getAllocator();

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
    void receivePage(size_t pageIdx);
    PageVersion sendPage(size_t pageIdx);

    Server* server_;
    std::string id_;
    TrackedMemoryRegion mem_;
    Allocator* allocator_;
    std::vector<PageVersion> pageVersions_;
};
}
