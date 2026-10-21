#include "app/PreviewTexture.h"
#ifdef __linux__
#include "control/backends/vulkan/VulkanImageConsumer.h"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace simxr::app
{
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
#ifdef __linux__
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
#ifdef __linux__
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
	std::array<bool, 8> reserved{};
	std::array<std::unique_ptr<State>, 8> preserved;
	std::size_t Reserve()
	{
		std::lock_guard lock(mutex);
		for (std::size_t index = 0; index < reserved.size(); ++index)
			if (!reserved[index])
			{
				reserved[index] = true;
				return index;
			}
		throw std::runtime_error("Eight companion textures are retained; release an image before importing another");
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
#ifdef __linux__
	if (!device.externalImages || !device.externalFence || !device.ioQueue || !device.owner)
		throw std::runtime_error("Companion device lacks external image sharing or an independent IO queue");
	const std::array extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
								VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
	auto consumerDevice = std::make_shared<VulkanDevice>(
		VulkanBinding{device.instance, device.physical, device.device, device.queueFamily, 1}, device.owner);
	m_state->native = std::make_unique<control::VulkanImageConsumer>(consumerDevice, extensions, VK_API_VERSION_1_1);
	if (m_state->description.identity != m_state->native->Identity())
		throw std::runtime_error("Runtime and companion device/driver UUIDs differ; select the runtime's adapter");
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
#ifdef __linux__
	return m_state->native->Identity();
#else
	return {};
#endif
}
std::shared_ptr<control::TransferHandle> PreviewTexture::ConsumerFence()
{
#ifdef __linux__
	return m_state->native->ConsumerFence();
#else
	return {};
#endif
}
void PreviewTexture::Import(control::ImageDescription description,
							std::span<const std::shared_ptr<control::TransferHandle>> handles)
{
#ifdef __linux__
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
#ifdef __linux__
	m_state->native->AbandonUnsent();
#endif
}
CompletionStatus PreviewTexture::Poll()
{
	if (m_state->ready)
		return CompletionStatus::Complete;
#ifdef __linux__
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
void PreviewTexture::Draw(float width)
{
	if (!m_state->ready)
		return;
	const auto height = width * m_state->description.height / m_state->description.width;
	for (std::size_t index = 0; index < m_state->descriptors.size(); ++index)
	{
		ImGui::Image(std::bit_cast<ImTextureID>(m_state->descriptors[index]), {width, height});
		if (index + 1 < m_state->descriptors.size())
			ImGui::SameLine(0, 12);
	}
	m_state->drawn = true;
}
VkSemaphore PreviewTexture::ReadyWait() const
{
#ifdef __linux__
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
bool PreviewTexture::CanRetire(std::uint64_t completed)
{
	ObserveCompletion(completed);
	return m_state->Retired();
}
} // namespace simxr::app
