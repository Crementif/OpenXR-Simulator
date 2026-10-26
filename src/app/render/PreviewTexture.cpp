#include "app/render/PreviewTexture.h"
#include "Pch.h"
#include "graphics/SnapshotBudget.h"
#if defined(__linux__) || defined(_WIN32)
#include "control/backends/vulkan/VulkanImageConsumer.h"
#endif

#include <bit>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace simxr::app
{
float PreviewAspect(const control::ImageDescription& image, std::optional<std::size_t> slice, bool correctProportions)
{
	const float pixels = image.width && image.height ? float(image.height) / image.width : 1;
	if (!correctProportions || image.views.empty())
		return pixels;
	float aspect = 0;
	const auto first = slice.value_or(0);
	const auto end = slice ? first + 1 : image.arrayLayers;
	for (auto index = first; index < end; ++index)
	{
		float value = pixels;
		if (index < image.views.size())
		{
			const auto& fov = image.views[index].fov;
			const float horizontal = std::abs(std::tan(fov.angleRight) - std::tan(fov.angleLeft));
			const float vertical = std::abs(std::tan(fov.angleUp) - std::tan(fov.angleDown));
			if (horizontal > 0 && vertical > 0 && std::isfinite(vertical / horizontal))
				value = vertical / horizontal;
		}
		aspect = std::max(aspect, value);
	}
	return aspect > 0 ? aspect : pixels;
}

struct PreviewTexture::State
{
	struct Retirement;
	static Retirement& Retirements();
	struct Reservation
	{
		std::size_t index;
		Reservation();
		~Reservation();
		Reservation(const Reservation&) = delete;
		Reservation& operator=(const Reservation&) = delete;
	} reservation;
	std::shared_ptr<const void> deviceOwner;
#if defined(__linux__) || defined(_WIN32)
	std::unique_ptr<control::VulkanImageConsumer> native;
#endif
	control::ImageDescription description;
	std::vector<VkDescriptorSet> descriptors;
	std::uint64_t lastSubmission = 0;
	std::uint64_t completed = 0;
	bool imported = false;
	bool ready = false;
	bool drawn = false;
	bool sampled = false;
	~State()
	{
		for (auto descriptor : descriptors)
			ImGui_ImplVulkan_RemoveTexture(descriptor);
	}
	bool Retired()
	{
		if (lastSubmission > completed)
			return false;
#if defined(__linux__) || defined(_WIN32)
		if (native && !native->NativeIdle())
		{
			native->CancelUnused();
			native->Poll();
			return native->NativeIdle();
		}
#endif
		return true;
	}
};

// Unknown completion cannot justify native destruction. Each texture reserves
// storage before acquiring native resources, so final preservation never allocates.
struct PreviewTexture::State::Retirement
{
	std::mutex mutex;
	std::array<bool, SnapshotBudget::MaxCapacity> reserved{};
	std::array<std::unique_ptr<State>, SnapshotBudget::MaxCapacity> preserved;
	std::uint64_t preservedBytes = 0;
	std::size_t Reserve()
	{
		std::lock_guard lock(mutex);
		if (static_cast<std::size_t>(std::count(reserved.begin(), reserved.end(), true)) >= SnapshotBudget::Capacity())
			throw std::runtime_error("Companion texture snapshot budget exhausted");
		for (std::size_t index = 0; index < reserved.size(); ++index)
			if (!reserved[index])
			{
				reserved[index] = true;
				return index;
			}
		throw std::runtime_error("Companion texture preservation capacity exhausted");
	}
	void Release(std::size_t index) noexcept
	{
		std::lock_guard lock(mutex);
		reserved[index] = false;
	}
	void Preserve(std::unique_ptr<State> texture) noexcept
	{
		const auto index = texture->reservation.index;

		std::lock_guard lock(mutex);
#if defined(__linux__) || defined(_WIN32)
		preservedBytes += texture->native ? texture->native->AllocatedBytes() : 0;
#endif
		preserved[index] = std::move(texture);
	}
};
PreviewTexture::State::Retirement& PreviewTexture::State::Retirements()
{
	// Process lifetime is deliberate: neither shutdown order nor a failed wait
	// establishes completion. Preserved entries retain their actual device owner.
	static auto* retirement = new Retirement;
	return *retirement;
}
PreviewTexture::State::Reservation::Reservation() : index(Retirements().Reserve()) {}
PreviewTexture::State::Reservation::~Reservation()
{
	Retirements().Release(index);
}

PreviewTexture::PreviewTexture(const VulkanDeviceAccess& device, control::ImageDescription description)
	: m_state(std::make_unique<State>())
{
	m_state->deviceOwner = device.owner;
	m_state->description = std::move(description);
#if defined(__linux__) || defined(_WIN32)
	const auto kind = m_state->description.completionKind;
	if (!device.externalImages || !device.ioQueue || !device.owner ||
		(kind == ImageCompletionKind::Fence && !device.externalFence) ||
		(kind == ImageCompletionKind::Timeline && !device.externalTimeline))
		throw std::runtime_error("Companion device lacks external image sharing or an independent IO queue");
	std::vector<const char*> extensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension};
	if (device.externalFence)
		extensions.push_back(VulkanExternalFenceExtension);
	if (device.externalTimeline)
		extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
	auto consumerDevice = std::make_shared<VulkanDevice>(
		VulkanBinding{device.instance, device.physical, device.device, device.queueFamily, 1}, device.owner);
	m_state->native = std::make_unique<control::VulkanImageConsumer>(consumerDevice, extensions, VK_API_VERSION_1_1,
																	 kind, device.externalTimeline);
	if (m_state->description.identity != m_state->native->Identity())
		throw std::runtime_error("The app and companion use different GPUs or drivers. Select the app's GPU.");
#else
	(void)device;
	throw std::runtime_error("Native preview is unavailable on this companion platform");
#endif
}
PreviewTexture::~PreviewTexture()
{
	try
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (m_state->lastSubmission <= m_state->completed)
		{
			if (m_state->Retired())
				return;
			if (std::chrono::steady_clock::now() >= deadline)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Companion texture retirement: %s\n", error.what());
	}
	std::fputs("Companion texture completion unknown; preserving bounded native owners\n", stderr);
	State::Retirements().Preserve(std::move(m_state));
}
control::ImageDeviceIdentity PreviewTexture::Identity() const
{
#if defined(__linux__) || defined(_WIN32)
	return m_state->native->Identity();
#else
	return {};
#endif
}
std::shared_ptr<control::TransferHandle> PreviewTexture::ConsumerFence()
{
#if defined(__linux__) || defined(_WIN32)
	return m_state->native->ConsumerFence();
#else
	return {};
#endif
}
void PreviewTexture::Import(control::ImageDescription description,
							std::span<const std::shared_ptr<control::TransferHandle>> handles)
{
#if defined(__linux__) || defined(_WIN32)
	m_state->description = std::move(description);
	m_state->imported = true;
	m_state->native->Import(m_state->description, handles);
	m_state->native->Submit();
#else
	(void)description;
	(void)handles;
#endif
}
void PreviewTexture::AbandonUnsent()
{
#if defined(__linux__) || defined(_WIN32)
	m_state->native->AbandonUnsent();
#endif
}
CompletionStatus PreviewTexture::Poll()
{
	if (m_state->ready)
		return CompletionStatus::Complete;
#if defined(__linux__) || defined(_WIN32)
	if (!m_state->imported)
		return CompletionStatus::Pending;
	const auto phase = m_state->native->Poll();
	if (phase != CompletionStatus::Complete)
		return phase;
	std::vector<VkDescriptorSet> descriptors;
	descriptors.reserve(m_state->native->Views().size());
	try
	{
		for (auto view : m_state->native->Views())
		{
			const auto descriptor =
				ImGui_ImplVulkan_AddTexture(m_state->native->Sampler(), view, VK_IMAGE_LAYOUT_GENERAL);
			if (!descriptor)
				throw std::runtime_error("Companion image descriptor allocation failed");
			descriptors.push_back(descriptor);
		}
	}
	catch (...)
	{
		for (auto descriptor : descriptors)
			ImGui_ImplVulkan_RemoveTexture(descriptor);
		throw;
	}
	m_state->descriptors = std::move(descriptors);
	m_state->ready = true;
	return CompletionStatus::Complete;
#else
	return CompletionStatus::Failed;
#endif
}
bool PreviewTexture::Ready() const
{
	return m_state->ready;
}
const control::ImageDescription& PreviewTexture::Description() const
{
	return m_state->description;
}
std::size_t PreviewTexture::SliceCount() const
{
	return m_state->descriptors.size();
}
void PreviewTexture::BeginFrame()
{
	m_state->drawn = false;
}
void PreviewTexture::Draw(float width, std::optional<std::size_t> slice, ImVec4 crop, bool correctProportions)
{
	if (!m_state->ready || (slice && *slice >= m_state->descriptors.size()))
		return;
	const auto first = slice.value_or(0);
	const auto end = slice ? first + 1 : m_state->descriptors.size();
	for (std::size_t index = first; index < end; ++index)
	{
		const float height = width * PreviewAspect(m_state->description, index, correctProportions) *
							 (crop.w - crop.y) / (crop.z - crop.x);
		ImGui::Image(std::bit_cast<ImTextureID>(m_state->descriptors[index]), {width, height}, {crop.x, crop.y},
					 {crop.z, crop.w});
		if (index + 1 < end)
			ImGui::SameLine(0, 12 * ImGui::GetFontSize() / 16);
	}
	m_state->drawn = true;
}
VkSemaphore PreviewTexture::ReadyWait() const
{
#if defined(__linux__) || defined(_WIN32)
	if (m_state->drawn && !m_state->sampled)
		return m_state->native->SampleReady();
#endif
	return VK_NULL_HANDLE;
}
void PreviewTexture::Submitted(std::uint64_t serial)
{
	if (m_state->drawn)
	{
		m_state->lastSubmission = serial;
		m_state->sampled = true;
	}
}
void PreviewTexture::ObserveCompletion(std::uint64_t serial)
{
	m_state->completed = std::max(m_state->completed, serial);
}
VkImage PreviewTexture::UseForCopy()
{
	if (!m_state->ready)
		return VK_NULL_HANDLE;
	m_state->drawn = true;
#if defined(__linux__) || defined(_WIN32)
	return m_state->native->Image().image;
#else
	return VK_NULL_HANDLE;
#endif
}
std::uint64_t PreviewTexture::PreservedBytes()
{
	auto& retirement = State::Retirements();
	std::lock_guard lock(retirement.mutex);
	return retirement.preservedBytes;
}
bool PreviewTexture::CanRetire(std::uint64_t completed)
{
	ObserveCompletion(completed);
	return m_state->Retired();
}
} // namespace simxr::app
