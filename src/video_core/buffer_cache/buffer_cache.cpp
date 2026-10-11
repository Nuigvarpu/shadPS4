// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

// Texture cache statistics, defined in texture_cache.cpp.
void TextureCacheNoteTexelSync();
void TextureCacheNoteBufferRead(bool synced);
void TextureCacheNoteBufferOverlapRead();

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    if (!instance.IsMaintenance6Supported()) {
        null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                     MemoryType::DeviceLocal, "Null Index Buffer");
        runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
    }
}

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

namespace {

/// Size of the window downloaded around a CPU read of GPU-modified memory. Larger windows mean
/// fewer GPU synchronizations when the guest reads many nearby addresses, at the cost of copying
/// more data per fault. Can be tuned with SHADPS4_READBACK_WINDOW_KB (power of two, 64..65536).
u64 ReadbackWindowSize() {
    static const u64 window = []() -> u64 {
        constexpr u64 Default = 512_KB;
        const char* env = std::getenv("SHADPS4_READBACK_WINDOW_KB");
        if (env == nullptr) {
            return Default;
        }
        const u64 kb = std::strtoull(env, nullptr, 10);
        if (kb < 64 || kb > 65536 || (kb & (kb - 1)) != 0) {
            LOG_WARNING(Render, "Ignoring invalid SHADPS4_READBACK_WINDOW_KB={}", env);
            return Default;
        }
        LOG_INFO(Render, "Readback window size: {} KB", kb);
        return kb * 1_KB;
    }();
    return window;
}

/// Statistics about guest-triggered GPU readbacks, logged about once per second, to find out
/// where synchronization time goes and which memory the guest keeps reading back.
struct ReadbackStats {
    struct Region {
        u32 reads;
        u32 writes;
    };

    std::mutex mutex;
    u64 faults{};
    u64 write_faults{};
    u64 gpu_thread_faults{};
    u64 empty_faults{};
    u64 queue_ns{};
    u64 flush_ns{};
    s64 window_start_ns{};
    std::unordered_map<VAddr, Region> regions;
    std::atomic<u64> downloads{};
    std::atomic<u64> ranges{};
    std::atomic<u64> bytes{};
    std::atomic<u64> skipped{};
    std::atomic<u64> prefetches{};
    std::atomic<u64> async_jobs{};
    std::atomic<u64> async_wait_ns{};

    static s64 Now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void Report(VAddr addr, bool is_write, bool gpu_thread, bool downloaded, u64 waited_queue_ns,
                u64 waited_flush_ns) {
        std::scoped_lock lk{mutex};
        ++faults;
        write_faults += is_write ? 1 : 0;
        gpu_thread_faults += gpu_thread ? 1 : 0;
        empty_faults += downloaded ? 0 : 1;
        queue_ns += waited_queue_ns;
        flush_ns += waited_flush_ns;
        auto& region = regions[addr >> 16];
        ++(is_write ? region.writes : region.reads);

        const s64 now = Now();
        if (window_start_ns == 0) {
            window_start_ns = now;
            return;
        }
        const s64 elapsed = now - window_start_ns;
        if (elapsed < 1'000'000'000) {
            return;
        }
        std::vector<std::pair<VAddr, Region>> hot(regions.begin(), regions.end());
        const size_t num_hot = std::min<size_t>(hot.size(), 6);
        std::partial_sort(
            hot.begin(), hot.begin() + num_hot, hot.end(), [](const auto& a, const auto& b) {
                return a.second.reads + a.second.writes > b.second.reads + b.second.writes;
            });
        std::string hot_list;
        for (size_t i = 0; i < num_hot; ++i) {
            hot_list += fmt::format(" {:#x}:{}r/{}w", hot[i].first << 16, hot[i].second.reads,
                                    hot[i].second.writes);
        }
        LOG_INFO(Render,
                 "Readbacks: {} faults ({} write, {} from GPU thread, {} empty, {} skipped), {} "
                 "downloads ({} prefetched, {} ranges), {} KB, waited {:.1f} ms in queue + {:.1f} "
                 "ms flushing in {:.1f} s, {} asynchronous ({:.1f} ms waited off the GPU thread), "
                 "{} regions; hot 64K regions:{}",
                 faults, write_faults, gpu_thread_faults, empty_faults,
                 skipped.exchange(0, std::memory_order_relaxed),
                 downloads.exchange(0, std::memory_order_relaxed),
                 prefetches.exchange(0, std::memory_order_relaxed),
                 ranges.exchange(0, std::memory_order_relaxed),
                 bytes.exchange(0, std::memory_order_relaxed) / 1024,
                 static_cast<double>(queue_ns) / 1e6, static_cast<double>(flush_ns) / 1e6,
                 static_cast<double>(elapsed) / 1e9, async_jobs.exchange(0),
                 static_cast<double>(async_wait_ns.exchange(0)) / 1e6, regions.size(), hot_list);
        faults = write_faults = gpu_thread_faults = empty_faults = queue_ns = flush_ns = 0;
        regions.clear();
        window_start_ns = now;
    }
};
ReadbackStats readback_stats;

/// Whether readback windows that the guest keeps faulting on are downloaded together with the
/// faulting one. Games often read back several GPU-written buffers every frame; downloading the
/// recently hot ones while the GPU is already synchronized saves a full GPU wait per buffer.
/// Can be disabled with SHADPS4_READBACK_PREFETCH=0.
bool ReadbackPrefetchEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_READBACK_PREFETCH");
        const bool value = env == nullptr || std::string_view{env} != "0";
        LOG_INFO(Render, "Readback prefetch: {}", value ? "enabled" : "disabled");
        return value;
    }();
    return enabled;
}

/// Readback windows the guest recently faulted on. Only accessed from the GPU thread.
struct HotWindow {
    VAddr start;
    VAddr end;
    s64 last_fault_ns;
};
constexpr s64 HotWindowLifetimeNs = 2'000'000'000;
std::array<HotWindow, 16> hot_windows{};

void RememberHotWindow(VAddr start, VAddr end, s64 now) {
    HotWindow* slot = nullptr;
    for (auto& window : hot_windows) {
        if (window.start == start && window.end == end) {
            window.last_fault_ns = now;
            return;
        }
        if (!slot || window.last_fault_ns < slot->last_fault_ns) {
            slot = &window;
        }
    }
    *slot = HotWindow{.start = start, .end = end, .last_fault_ns = now};
}

/// Debug switch: SHADPS4_READBACK_TRACE=1 logs every guest fault on GPU-written memory with a
/// hash of the data the guest will read, to see whether that data changes between frames.
bool ReadbackTraceEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_READBACK_TRACE");
        const bool value = env != nullptr && std::string_view{env} == "1";
        LOG_INFO(Render, "Readback trace: {}", value ? "enabled" : "disabled");
        return value;
    }();
    return enabled;
}

std::atomic<u64> readback_trace_lines{};

u64 HashGuestLine(Core::MemoryManager* memory, VAddr address) {
    std::array<u8, 64> line{};
    memory->CopySparseMemory(address & ~VAddr{63}, line.data(), line.size());
    u64 hash = 1469598103934665603ULL;
    for (const u8 byte : line) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

/// Whether readbacks requested by guest threads wait for the GPU on the faulting thread instead
/// of the GPU thread, so command processing continues meanwhile. Can be disabled with
/// SHADPS4_READBACK_ASYNC=0.
bool ReadbackAsyncEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_READBACK_ASYNC");
        const bool value = env == nullptr || std::string_view{env} != "0";
        LOG_INFO(Render, "Asynchronous readbacks: {}", value ? "enabled" : "disabled");
        return value;
    }();
    return enabled;
}

/// Buffer reads of memory that holds a rendered image. The buffer cache does not see image
/// writes, so such reads may observe stale data. SHADPS4_BUF_IMAGE_SYNC=1 copies the image into
/// the buffer before the read (like read-only texel buffers already do); 0 disables detection.
enum class BufferImageMode { Off, Count, Sync };

BufferImageMode GetBufferImageMode() {
    static const BufferImageMode mode = [] {
        const char* env = std::getenv("SHADPS4_BUF_IMAGE_SYNC");
        const std::string_view value = env != nullptr ? env : "";
        const BufferImageMode result = value == "1"   ? BufferImageMode::Sync
                                       : value == "0" ? BufferImageMode::Off
                                                      : BufferImageMode::Count;
        LOG_INFO(Render, "Buffer reads of rendered images: {}",
                 result == BufferImageMode::Sync    ? "synchronized"
                 : result == BufferImageMode::Count ? "counted"
                                                    : "ignored");
        return result;
    }();
    return mode;
}

bool IsRenderedImageAt(VideoCore::TextureCache& texture_cache, VAddr device_addr) {
    bool found = false;
    texture_cache.ForEachImageWithAddress(
        device_addr, [&](VideoCore::ImageId, VideoCore::Image& image) {
            if (image.info.guest_address == device_addr &&
                True(image.flags & VideoCore::ImageFlagBits::GpuModified) &&
                (image.usage.render_target || image.usage.depth_target || image.usage.storage)) {
                found = true;
            }
        });
    return found;
}

bool OverlapsRenderedImage(VideoCore::TextureCache& texture_cache, VAddr device_addr, u32 size) {
    bool found = false;
    texture_cache.ForEachImageInRegion(
        device_addr, size, [&](VideoCore::ImageId, VideoCore::Image& image) {
            if (True(image.flags & VideoCore::ImageFlagBits::GpuModified) &&
                (image.usage.render_target || image.usage.depth_target || image.usage.storage)) {
                found = true;
            }
        });
    return found;
}

/// A guest memory range whose GPU-written data is being downloaded.
struct ReadbackWindow {
    const VideoCore::Buffer* arena;
    VAddr start;
    u64 size;
    size_t first_copy;
    size_t num_copies;
};

/// A batch of readback windows downloaded with one GPU submission. The windows stay protected
/// until the data has been written back to guest memory.
struct AsyncReadback {
    boost::container::small_vector<ReadbackWindow, 17> windows;
    boost::container::small_vector<vk::BufferCopy, 64> copies;
    Vulkan::StagingBufferRef staging{};
    u64 tick = 0;
    bool finished = false; // GPU thread only
    std::mutex write_mutex;
    bool written = false;

    bool Overlaps(VAddr start, VAddr end) const {
        return std::ranges::any_of(windows, [&](const ReadbackWindow& window) {
            return window.start < end && start < window.start + window.size;
        });
    }

    /// Copies the downloaded data into guest memory. Safe to call from several threads.
    void WriteBack(Core::MemoryManager* memory) {
        std::scoped_lock lk{write_mutex};
        if (written) {
            return;
        }
        staging.buffer->Invalidate(staging.offset, staging.size);
        for (const auto& window : windows) {
            for (size_t i = 0; i < window.num_copies; ++i) {
                const auto& copy = copies[window.first_copy + i];
                auto* dst_addr = std::bit_cast<u8*>(window.arena->cpu_addr + copy.srcOffset);
                memory->TryWriteBacking(
                    dst_addr, staging.mapped + (copy.dstOffset - staging.offset), copy.size);
            }
        }
        written = true;
    }
};

/// Asynchronous readbacks still in flight. Only accessed from the GPU thread.
std::vector<std::shared_ptr<AsyncReadback>> async_readbacks;

} // Anonymous namespace

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    if (!is_write && !assume_locks && !memory_tracker->IsRegionGpuModified(device_addr, size)) {
        // Another thread downloaded this data while the fault was being raised; the access can
        // simply be retried without synchronizing with the GPU again.
        readback_stats.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool prefetch = ReadbackPrefetchEnabled();
    const bool use_async = !is_write && !assume_locks && prefetch && ReadbackAsyncEnabled();

    // Runs on the GPU thread once an asynchronous readback has been written back. Windows the GPU
    // wrote to again in the meantime stay protected, so the guest faults and reads them again.
    const auto finish_async = [this](const std::shared_ptr<AsyncReadback>& job) {
        if (job->finished) {
            return;
        }
        job->finished = true;
        for (const auto& window : job->windows) {
            bool rewritten = false;
            gpu_modified_ranges.ForEachInRange(window.start, window.size,
                                               [&](VAddr, VAddr) { rewritten = true; });
            if (!rewritten) {
                memory_tracker->UnmarkRegionAsGpuModified(window.start, window.size, false);
            }
        }
        staging_pool.FreeDeferred(job->staging);
        std::erase(async_readbacks, job);
    };

    s64 flush_start = 0;
    s64 flush_end = 0;
    bool downloaded = false;
    std::shared_ptr<AsyncReadback> async_job;
    const auto flush_request = [&] {
        flush_start = ReadbackStats::Now();
        const u64 downloads_before = readback_stats.downloads.load(std::memory_order_relaxed);
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        const u64 window_size = ReadbackWindowSize();
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, window_size), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + window_size, device_addr + size), arena_end);
        RememberHotWindow(window_start, window_end, flush_start);

        if (use_async) {
            // Another thread is already waiting for this data; wait for the same download.
            for (const auto& job : async_readbacks) {
                if (job->Overlaps(window_start, window_end)) {
                    async_job = job;
                    flush_end = ReadbackStats::Now();
                    return;
                }
            }
        } else {
            // Pending asynchronous downloads must land in guest memory before anything else
            // looks at those pages.
            const auto pending = async_readbacks;
            for (const auto& job : pending) {
                scheduler.Wait(job->tick);
                job->WriteBack(memory);
                finish_async(job);
            }
        }

        if (!prefetch) {
            DownloadMemory(arena, window_start, window_end - window_start);
        } else {
            // Download the faulting window together with the other windows the guest keeps
            // reading back, using a single GPU submission for all of them.
            auto job = std::make_shared<AsyncReadback>();
            u64 total_size_bytes = 0;
            const auto collect = [&](const Buffer* window_arena, VAddr start, u64 window_bytes) {
                const size_t first_copy = job->copies.size();
                const VAddr arena_base = window_arena->cpu_addr;
                memory_tracker->ForEachDownloadRange<false>(
                    start, window_bytes, [&](u64 address, u64 range_size) {
                        gpu_modified_ranges.ForEachInRange(
                            address, range_size, [&](VAddr range_start, VAddr range_end) {
                                const u64 copy_size = range_end - range_start;
                                job->copies.push_back(vk::BufferCopy{
                                    .srcOffset = range_start - arena_base,
                                    .dstOffset = total_size_bytes,
                                    .size = copy_size,
                                });
                                // Align up to avoid cache conflicts
                                total_size_bytes += Common::AlignUp(copy_size, 64ULL);
                            });
                        gpu_modified_ranges.Subtract(address, range_size);
                    });
                job->windows.push_back(ReadbackWindow{
                    .arena = window_arena,
                    .start = start,
                    .size = window_bytes,
                    .first_copy = first_copy,
                    .num_copies = job->copies.size() - first_copy,
                });
            };
            collect(arena, window_start, window_end - window_start);
            for (const auto& window : hot_windows) {
                if (window.end == 0 || window.start == window_start ||
                    flush_start - window.last_fault_ns > HotWindowLifetimeNs ||
                    !memory_tracker->IsRegionGpuModified(window.start, window.end - window.start) ||
                    std::ranges::any_of(async_readbacks, [&](const auto& pending) {
                        return pending->Overlaps(window.start, window.end);
                    })) {
                    continue;
                }
                const auto* hot_arena =
                    GetArena(window.start >> block_shift, (window.end - 1) >> block_shift);
                collect(hot_arena, window.start, window.end - window.start);
                if (job->windows.back().num_copies != 0) {
                    readback_stats.prefetches.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Windows without GPU-written data need no download.
            for (const auto& window : job->windows) {
                if (window.num_copies == 0) {
                    memory_tracker->UnmarkRegionAsGpuModified(window.start, window.size, false);
                }
            }
            if (total_size_bytes != 0) {
                readback_stats.downloads.fetch_add(1, std::memory_order_relaxed);
                readback_stats.ranges.fetch_add(job->copies.size(), std::memory_order_relaxed);
                readback_stats.bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
                job->staging = staging_pool.Request(
                    total_size_bytes, VideoCore::MemoryType::HostCached, 0, use_async);
                for (auto& copy : job->copies) {
                    copy.dstOffset += job->staging.offset;
                }
                for (const auto& window : job->windows) {
                    if (window.num_copies != 0) {
                        runtime.CopyBuffer(
                            window.arena, job->staging.buffer,
                            std::span{job->copies.data() + window.first_copy, window.num_copies});
                    }
                }
                if (use_async) {
                    // Submit without waiting; the faulting thread waits for the data itself.
                    job->tick = scheduler.CurrentTick();
                    scheduler.Flush();
                    async_readbacks.push_back(job);
                    async_job = job;
                } else {
                    scheduler.Finish();
                    job->WriteBack(memory);
                    for (const auto& window : job->windows) {
                        memory_tracker->UnmarkRegionAsGpuModified(window.start, window.size, false);
                    }
                }
            }
        }
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
        downloaded = readback_stats.downloads.load(std::memory_order_relaxed) != downloads_before;
        flush_end = ReadbackStats::Now();
    };
    const s64 start = ReadbackStats::Now();
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(flush_request);
    }
    if (async_job) {
        const s64 wait_start = ReadbackStats::Now();
        scheduler.GetWorkSemaphore()->Wait(async_job->tick);
        async_job->WriteBack(memory);
        readback_stats.async_jobs.fetch_add(1, std::memory_order_relaxed);
        readback_stats.async_wait_ns.fetch_add(ReadbackStats::Now() - wait_start,
                                               std::memory_order_relaxed);
        liverpool->SendCommand<true>([&] { finish_async(async_job); });
    }
    readback_stats.Report(device_addr, is_write, assume_locks, downloaded,
                          static_cast<u64>(flush_start - start),
                          static_cast<u64>(flush_end - flush_start));
    if (ReadbackTraceEnabled() && readback_trace_lines.fetch_add(1) < 30000) {
        u64 copies = 0;
        u64 copy_bytes = 0;
        if (async_job) {
            copies = async_job->copies.size();
            for (const auto& copy : async_job->copies) {
                copy_bytes += copy.size;
            }
        }
        LOG_INFO(Render,
                 "Readback trace: addr={:#x} size={} {} {} downloaded={} async={} copies={} "
                 "bytes={} line_hash={:016x} still_gpu_modified={}",
                 device_addr, size, is_write ? "write" : "read",
                 assume_locks ? "gpu-thread" : "guest", downloaded, async_job != nullptr, copies,
                 copy_bytes, HashGuestLine(memory, device_addr),
                 memory_tracker->IsRegionGpuModified(device_addr, size));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        // No GPU-written data is left in this range. Clear the page state anyway, otherwise the
        // pages stay protected and every later access faults and waits for the GPU for nothing.
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
        return;
    }
    readback_stats.downloads.fetch_add(1, std::memory_order_relaxed);
    readback_stats.ranges.fetch_add(copies.size(), std::memory_order_relaxed);
    readback_stats.bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    bool sync_image = false;
    if (!is_written && !is_texel_buffer && GetBufferImageMode() != BufferImageMode::Off &&
        !texture_cache.IsMeta(device_addr) && IsRenderedImageAt(texture_cache, device_addr)) {
        sync_image = GetBufferImageMode() == BufferImageMode::Sync;
        TextureCacheNoteBufferRead(sync_image);
    } else if (!is_written && GetBufferImageMode() != BufferImageMode::Off &&
               OverlapsRenderedImage(texture_cache, device_addr, size)) {
        TextureCacheNoteBufferOverlapRead();
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && !sync_image && size <= STREAM_THRESHOLD &&
        !IsRegionGpuModified(device_addr, size)) {
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer || sync_image);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }

    const vk::MemoryAllocateFlagsInfo alloc_flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::MemoryAllocateInfo alloc_info = {
        .pNext = &alloc_flags,
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    });
    if (!copies.empty()) {
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        const bool synced = SynchronizeMemoryFromImage(arena, device_addr, size);
        if (synced) {
            TextureCacheNoteTexelSync();
        }
        return synced;
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
