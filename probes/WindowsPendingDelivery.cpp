#include "WindowsPendingDelivery.h"
#include "VulkanProbeInstance.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/LocalControlClient.h"
#include "control/backends/vulkan/VulkanImageConsumer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <windows.h>

namespace simxr::probe
{
static void Require(bool condition, const char* detail)
{
	if (!condition)
		throw std::runtime_error(detail);
}

[[noreturn]] static void PreservePending(const char* detail)
{
	std::fprintf(stderr, "FAIL: pending-runtime delivery: %s; preserving native owners until process exit\n", detail);
	std::fflush(stderr);
	TerminateProcess(GetCurrentProcess(), 1);
	std::_Exit(1);
}

static std::string ControlEndpoint(std::uint32_t process)
{
	const auto* selected = std::getenv("SIMXR_CONTROL_SOCKET");
	return selected && *selected ? selected : std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(process);
}

static control::ControlPacket Exchange(control::LocalControlClient& client, control::OpCode operation,
									   std::vector<control::Field> fields = {},
									   std::vector<std::shared_ptr<control::TransferHandle>> handles = {})
{
	control::ControlPacket request;
	request.message.opcode = static_cast<std::uint16_t>(operation);
	request.message.fields = std::move(fields);
	request.handles = std::move(handles);
	return client.Request(std::move(request));
}

static control::ErrorCode Result(const control::ControlPacket& response)
{
	const auto* field = response.message.Find(control::tag::Error);
	const auto value = field ? field->AsU32() : std::nullopt;
	Require(value.has_value(), "Control response omitted its result");
	return static_cast<control::ErrorCode>(*value);
}

static void CheckResponse(const control::ControlPacket& response)
{
	if (Result(response) == control::ErrorCode::Ok)
		return;
	const auto* field = response.message.Find(control::tag::Detail);
	const auto detail = field ? field->AsText() : std::nullopt;
	throw std::runtime_error(detail ? std::string(*detail) : "Pending-runtime control request failed");
}

static std::uint64_t Id(const control::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	const auto value = field ? field->AsU64() : std::nullopt;
	Require(value && *value, "Control response omitted a nonzero identity");
	return *value;
}

struct PendingPreview
{
	std::uint64_t session, generation, image;
	control::ImageDescription description;
};

static PendingPreview RetainPreview(control::LocalControlClient& client)
{
	namespace tag = control::tag;
	CheckResponse(Exchange(client, control::OpCode::Hello,
						   {control::Field::Text(tag::ClientName, "Runtime unload GPU consumer"),
							control::Field::U64(tag::RequiredCapabilities,
												static_cast<std::uint64_t>(control::Capability::GpuPreview) |
													static_cast<std::uint64_t>(control::Capability::LivePreview))}));
	const auto sessions = Exchange(client, control::OpCode::Sessions);
	CheckResponse(sessions);
	const auto* list = sessions.message.Find(tag::Sessions);
	Require(list != nullptr, "Session discovery omitted its list");
	const auto entries = control::DecodeFields(list->value);
	Require(entries.error == control::DecodeError::None && entries.fields.size() == 1,
			"Pending-runtime proof requires exactly one current session");
	auto entry = control::DecodeFields(entries.fields.front().value);
	Require(entry.error == control::DecodeError::None, "Session discovery returned invalid fields");
	control::Message session;
	session.fields = std::move(entry.fields);
	PendingPreview result{Id(session, tag::Session), Id(session, tag::Generation), 0, {}};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (true)
	{
		const auto retained = Exchange(client, control::OpCode::RetainPreview,
									   {control::Field::U64(tag::Session, result.session),
										control::Field::U64(tag::Generation, result.generation)});
		if (Result(retained) != control::ErrorCode::Busy)
		{
			CheckResponse(retained);
			const auto* available = retained.message.Find(tag::ImageAvailable);
			Require(available && available->AsBool().has_value(), "Preview omitted image availability");
			if (*available->AsBool())
			{
				result.image = Id(retained.message, tag::ImageReaderId);
				Require(Id(retained.message, tag::Session) == result.session &&
							Id(retained.message, tag::Generation) == result.generation,
						"Retained preview changed session identity");
				const auto* descriptor = retained.message.Find(tag::ImageDescriptor);
				Require(descriptor != nullptr, "Retained preview omitted its description");
				result.description = control::DecodeImageDescription(*descriptor);
				return result;
			}
		}
		Require(std::chrono::steady_clock::now() < deadline, "Current session did not publish a GPU preview");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

struct PendingConsumerDevice
{
	Instance instance;
	VkDevice device = VK_NULL_HANDLE;

	~PendingConsumerDevice()
	{
		if (device)
			vkDestroyDevice(device, nullptr);
	}
};

static std::shared_ptr<VulkanDevice> CreateConsumerDevice(const control::ImageDescription& source, bool timelineEnabled,
														  std::vector<const char*>& enabled)
{
	const auto owner = std::make_shared<PendingConsumerDevice>();
	owner->instance.Create();
	std::uint32_t count = 0;
	Check(vkEnumeratePhysicalDevices(owner->instance.handle, &count, nullptr), "enumerate pending-consumer adapters");
	std::vector<VkPhysicalDevice> adapters(count);
	Check(vkEnumeratePhysicalDevices(owner->instance.handle, &count, adapters.data()),
		  "enumerate pending-consumer adapters");
	adapters.resize(count);
	for (const auto physical : adapters)
	{
		VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		properties.pNext = &identity;
		vkGetPhysicalDeviceProperties2(physical, &properties);
		if (properties.properties.apiVersion < VK_API_VERSION_1_1 ||
			std::memcmp(source.identity.device.data(), identity.deviceUUID, VK_UUID_SIZE) != 0 ||
			std::memcmp(source.identity.driver.data(), identity.driverUUID, VK_UUID_SIZE) != 0)
			continue;
		VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
		enabled = {VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension};
		if (timelineEnabled)
		{
			VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			features.pNext = &timeline;
			vkGetPhysicalDeviceFeatures2(physical, &features);
			Require(timeline.timelineSemaphore == VK_TRUE, "Pending-consumer timeline support is unavailable");
			enabled.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
		}
		if (source.completionKind == ImageCompletionKind::Fence)
		{
			const auto capabilities = VulkanExternalDevice::QueryFence(physical, VK_API_VERSION_1_1);
			Require(capabilities.CanImport() && capabilities.CanExport(),
					"Pending-consumer external fence is unavailable");
			enabled.push_back(VulkanExternalFenceExtension);
		}
		else
		{
			Require(source.completionKind == ImageCompletionKind::Timeline, "Unknown preview completion kind");
			const auto capabilities = VulkanExternalDevice::QueryTimeline(physical, VK_API_VERSION_1_1);
			Require(capabilities.CanImportSemaphore() && capabilities.CanExportSemaphore(),
					"Pending-consumer external timeline is unavailable");
		}
		Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr),
			  "enumerate pending-consumer device extensions");
		std::vector<VkExtensionProperties> extensions(count);
		Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()),
			  "enumerate pending-consumer device extensions");
		extensions.resize(count);
		for (const auto* required : enabled)
			Require(
				std::any_of(extensions.begin(), extensions.end(),
							[&](const auto& extension) { return std::strcmp(required, extension.extensionName) == 0; }),
				"Pending-consumer device extension is unavailable");
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
		for (std::uint32_t family = 0; family < count; ++family)
		{
			if (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			const float priority = 1;
			VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			queue.queueFamilyIndex = family;
			queue.queueCount = 1;
			queue.pQueuePriorities = &priority;
			VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
			create.pNext = timelineEnabled ? &timeline : nullptr;
			create.queueCreateInfoCount = 1;
			create.pQueueCreateInfos = &queue;
			create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
			create.ppEnabledExtensionNames = enabled.data();
			Check(vkCreateDevice(physical, &create, nullptr, &owner->device), "create pending-consumer device");
			return std::make_shared<VulkanDevice>(
				VulkanBinding{owner->instance.handle, physical, owner->device, family, 0}, owner);
		}
	}
	throw std::runtime_error("No matching pending-consumer device and driver");
}

static control::ControlPacket Share(control::LocalControlClient& client, const PendingPreview& preview,
									control::VulkanImageConsumer& consumer)
{
	namespace tag = control::tag;
	Require(consumer.Identity() == preview.description.identity, "Recreated runtime changed native device identity");
	return Exchange(
		client, control::OpCode::ShareImage,
		{control::Field::U64(tag::ImageReaderId, preview.image),
		 control::Field::Bytes(tag::DeviceUuid, preview.description.identity.device),
		 control::Field::Bytes(tag::DriverUuid, preview.description.identity.driver),
		 control::Field::U32(tag::CompletionKind, static_cast<std::uint32_t>(preview.description.completionKind), true),
		 control::Field::U32(tag::HandleCount, 1)},
		{consumer.ConsumerFence()});
}

static SharedImageKey Import(const control::ControlPacket& response, const PendingPreview& preview,
							 control::VulkanImageConsumer& consumer)
{
	CheckResponse(response);
	const auto* descriptor = response.message.Find(control::tag::ImageDescriptor);
	Require(descriptor && response.handles.size() == 2, "Delivery omitted its description or native handles");
	const auto description = control::DecodeImageDescription(*descriptor);
	Require(description.identity == preview.description.identity &&
				description.completionKind == preview.description.completionKind &&
				description.handleKind == static_cast<std::uint32_t>(control::TransferHandleKind::OpaqueWin32) &&
				description.width == preview.description.width && description.height == preview.description.height &&
				description.arrayLayers == preview.description.arrayLayers,
			"Delivery changed the retained preview identity or image");
	consumer.Import(description, response.handles);
	return {{Id(response.message, control::tag::PoolGeneration)}, {Id(response.message, control::tag::SharedImageId)}};
}

static void Release(control::LocalControlClient& client, const PendingPreview& preview,
					std::span<const std::uint64_t> deliveries)
{
	for (const auto delivery : deliveries)
		CheckResponse(Exchange(client, control::OpCode::ReleaseDelivery,
							   {control::Field::U64(control::tag::DeliveryId, delivery)}));
	CheckResponse(Exchange(client, control::OpCode::ReleaseImage,
						   {control::Field::U64(control::tag::ImageReaderId, preview.image)}));
	client.Close();
}

static void AwaitConsumers(std::span<const std::unique_ptr<control::VulkanImageConsumer>> consumers)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (true)
	{
		bool complete = true;
		for (const auto& consumer : consumers)
		{
			const auto result = consumer->Poll();
			Require(result != CompletionStatus::Failed, "Pending-runtime consumer GPU work failed");
			complete &= result == CompletionStatus::Complete && consumer->NativeIdle();
		}
		if (complete)
			return;
		Require(std::chrono::steady_clock::now() < deadline, "Pending-runtime consumer GPU work did not retire");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

static std::wstring PendingPipe(std::uint32_t process)
{
	return LR"(\\.\pipe\SimXR.PendingConsumer.)" + std::to_wstring(process);
}

int RunWindowsPendingConsumer(bool gated, std::uint32_t runtimeProcess)
{
	control::windows::Handle pipe(CreateFileW(PendingPipe(runtimeProcess).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
											  nullptr, OPEN_EXISTING, 0, nullptr));
	Require(pipe.value != INVALID_HANDLE_VALUE, "Cannot connect pending-consumer command pipe");
	WindowsPendingDelivery consumer(gated, runtimeProcess);
	while (true)
	{
		std::uint8_t command = 0;
		DWORD transferred = 0;
		Require(ReadFile(pipe.value, &command, sizeof(command), &transferred, nullptr) &&
					transferred == sizeof(command),
				"Pending-consumer parent disconnected");
		std::array<char, 1024> response{};
		try
		{
			switch (command)
			{
			case 1:
				consumer.Hold();
				break;
			case 2:
				consumer.VerifyPending();
				break;
			case 3:
				consumer.ReleaseAndVerify();
				break;
			case 4:
				consumer.CheckWatchdog();
				break;
			case 5:
				break;
			default:
				throw std::runtime_error("Unknown pending-consumer command");
			}
		}
		catch (const std::exception& error)
		{
			std::snprintf(response.data(), response.size(), "%s", error.what());
		}
		Require(WriteFile(pipe.value, response.data(), static_cast<DWORD>(response.size()), &transferred, nullptr) &&
					transferred == response.size(),
				"Cannot acknowledge pending-consumer command");
		if (response.front())
			PreservePending(response.data());
		if (command == 5)
			return 0;
	}
}

WindowsPendingDelivery::WindowsPendingDelivery(bool gated, std::uint32_t runtimeProcess)
	: m_gated(gated), m_runtimeProcess(runtimeProcess ? runtimeProcess : GetCurrentProcessId())
{
	if (runtimeProcess)
		return;
	control::windows::Security security;
	m_pipe = control::windows::Handle(
		CreateNamedPipeW(PendingPipe(m_runtimeProcess).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
						 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096,
						 0, &security.attributes));
	Require(m_pipe.value != INVALID_HANDLE_VALUE, "Cannot create pending-consumer command pipe");
	std::wstring executable(32768, L'\0');
	const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
	Require(size && size < executable.size(), "Cannot identify pending-consumer executable");
	executable.resize(size);
	auto command = L"\"" + executable + L"\" --pending-consumer " + std::to_wstring(m_runtimeProcess) +
				   (m_gated ? L" gated" : L" pending");
	STARTUPINFOW startup{sizeof(startup)};
	PROCESS_INFORMATION child{};
	Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
						   nullptr, &startup, &child),
			"Cannot start isolated pending consumer");
	m_process = control::windows::Handle(child.hProcess);
	control::windows::Handle thread(child.hThread);
	try
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!ConnectNamedPipe(m_pipe.value, nullptr))
		{
			const auto error = GetLastError();
			if (error == ERROR_PIPE_CONNECTED)
				break;
			Require(error == ERROR_PIPE_LISTENING && std::chrono::steady_clock::now() < deadline &&
						WaitForSingleObject(m_process.value, 0) == WAIT_TIMEOUT,
					"Pending consumer did not connect");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
		Require(SetNamedPipeHandleState(m_pipe.value, &mode, nullptr, nullptr), "Cannot configure consumer pipe");
	}
	catch (...)
	{
		TerminateProcess(m_process.value, 1);
		WaitForSingleObject(m_process.value, 5000);
		throw;
	}
}

WindowsPendingDelivery::~WindowsPendingDelivery()
{
	if (m_process.value != INVALID_HANDLE_VALUE)
	{
		try
		{
			Command(5);
			DWORD code = 1;
			Require(WaitForSingleObject(m_process.value, 5000) == WAIT_OBJECT_0 &&
						GetExitCodeProcess(m_process.value, &code) && code == 0,
					"Pending consumer did not exit cleanly");
		}
		catch (...)
		{
			TerminateProcess(m_process.value, 1);
			WaitForSingleObject(m_process.value, 5000);
			PreservePending("isolated consumer failed to retire");
		}
		return;
	}
	if (m_active)
		PreservePending("test ended before confirmed GPU retirement");
	StopWatchdog();
	if (m_gateRetired)
		vkDestroyFence(m_device->binding.device, m_gateRetired, nullptr);
	if (m_gate)
		vkDestroySemaphore(m_device->binding.device, m_gate, nullptr);
	m_consumers = {};
	m_device.reset();
	if (s_validationErrors.load() != 0)
		PreservePending("consumer device teardown reported Vulkan validation errors");
}

void WindowsPendingDelivery::Hold()
{
	if (m_process.value != INVALID_HANDLE_VALUE)
	{
		Command(1);
		std::puts("pending-runtime: eight isolated consumers held");
		return;
	}
	Require(!m_device && !m_complete, "Pending-runtime hold was already started");
	control::LocalControlClient client(ControlEndpoint(m_runtimeProcess), std::chrono::milliseconds(5000));
	const auto preview = RetainPreview(client);
	m_completion = preview.description.completionKind;
	m_timelineEnabled = m_gated || m_completion == ImageCompletionKind::Timeline;
	m_device = CreateConsumerDevice(preview.description, m_timelineEnabled, m_extensions);
	for (auto& consumer : m_consumers)
		consumer = std::make_unique<control::VulkanImageConsumer>(m_device, m_extensions, VK_API_VERSION_1_1,
																  m_completion, m_timelineEnabled);
	if (m_gated)
	{
		m_signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
			vkGetDeviceProcAddr(m_device->binding.device, "vkSignalSemaphoreKHR"));
		Require(m_signal != nullptr, "Pending-consumer host gate signal command is unavailable");
		VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		semaphore.pNext = &type;
		Check(vkCreateSemaphore(m_device->binding.device, &semaphore, nullptr, &m_gate),
			  "create pending-consumer gate");
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		Check(vkCreateFence(m_device->binding.device, &fence, nullptr, &m_gateRetired),
			  "create pending-consumer gate retirement");
		const std::uint64_t value = 1;
		VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		timeline.waitSemaphoreValueCount = 1;
		timeline.pWaitSemaphoreValues = &value;
		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.pNext = &timeline;
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &m_gate;
		submit.pWaitDstStageMask = &stage;
		m_active = true;
		Check(vkQueueSubmit(m_device->queue, 1, &submit, m_gateRetired), "queue pending-consumer gate");
		StartWatchdog();
	}
	m_active = true;
	std::array<std::uint64_t, DeliveryCount> deliveries{};
	for (std::size_t index = 0; index < DeliveryCount; ++index)
	{
		const auto shared = Share(client, preview, *m_consumers[index]);
		m_keys[index] = Import(shared, preview, *m_consumers[index]);
		Require(std::find(m_keys.begin(), m_keys.begin() + index, m_keys[index]) == m_keys.begin() + index,
				"Concurrent delivery reused an immutable allocation identity");
		deliveries[index] = Id(shared.message, control::tag::DeliveryId);
		if (m_gated)
			m_consumers[index]->Submit();
	}
	Release(client, preview, deliveries);
	for (const auto& consumer : m_consumers)
		Require(consumer->Poll() == CompletionStatus::Pending && !consumer->NativeIdle(),
				"Pending consumer completed before runtime teardown");
	m_held = true;
	std::puts(
		m_gated
			? "pending-runtime: eight GPU consumers submitted and gated; remote readers released and disconnected"
			: "pending-runtime: eight imported consumers await submission; remote readers released and disconnected");
}

void WindowsPendingDelivery::VerifyPending()
{
	if (m_process.value != INVALID_HANDLE_VALUE)
	{
		Command(2);
		std::puts("pending-runtime: recreated runtime preserves occupied delivery capacity");
		return;
	}
	CheckWatchdog();
	Require(m_held && !m_verified, "Pending-runtime verification is out of order");
	control::LocalControlClient client(ControlEndpoint(m_runtimeProcess), std::chrono::milliseconds(5000));
	const auto preview = RetainPreview(client);
	Require(preview.description.completionKind == m_completion, "Recreated runtime changed consumer completion kind");
	for (const auto& consumer : m_consumers)
		Require(consumer->Poll() == CompletionStatus::Pending && !consumer->NativeIdle(),
				"Consumer work completed before the explicit release");
	const auto rejected = Share(client, preview, *m_consumers.front());
	Require(Result(rejected) == control::ErrorCode::Capacity && rejected.handles.empty(),
			"Runtime recreation reset the capacity held by pending GPU consumers");
	Release(client, preview, {});
	m_verified = true;
	std::puts("pending-runtime: recreated session preserves the occupied eight-delivery limit");
}

void WindowsPendingDelivery::ReleaseAndVerify()
{
	if (m_process.value != INVALID_HANDLE_VALUE)
	{
		Command(3);
		std::puts("pending-runtime: isolated consumers retired; all eight delivery slots recovered");
		return;
	}
	Require(m_verified && !m_complete, "Pending-runtime release is out of order");
	StopWatchdog();
	CheckWatchdog();
	if (m_gated)
	{
		VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		signal.semaphore = m_gate;
		signal.value = 1;
		Check(m_signal(m_device->binding.device, &signal), "release pending-consumer gate");
	}
	else
		for (const auto& consumer : m_consumers)
			consumer->Submit();
	AwaitConsumers(m_consumers);
	m_consumers = {};
	if (m_gated)
	{
		Check(vkGetFenceStatus(m_device->binding.device, m_gateRetired), "retire pending-consumer gate");
		vkDestroyFence(m_device->binding.device, m_gateRetired, nullptr);
		m_gateRetired = VK_NULL_HANDLE;
		vkDestroySemaphore(m_device->binding.device, m_gate, nullptr);
		m_gate = VK_NULL_HANDLE;
	}
	control::LocalControlClient client(ControlEndpoint(m_runtimeProcess), std::chrono::milliseconds(5000));
	const auto preview = RetainPreview(client);
	Require(preview.description.completionKind == m_completion, "Recovery changed consumer completion kind");
	std::array<std::uint64_t, DeliveryCount> deliveries{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (std::size_t index = 0; index < DeliveryCount; ++index)
	{
		m_consumers[index] = std::make_unique<control::VulkanImageConsumer>(m_device, m_extensions, VK_API_VERSION_1_1,
																			m_completion, m_timelineEnabled);
		while (true)
		{
			const auto shared = Share(client, preview, *m_consumers[index]);
			if (Result(shared) != control::ErrorCode::Capacity)
			{
				const auto key = Import(shared, preview, *m_consumers[index]);
				Require(std::none_of(m_keys.begin(), m_keys.end(),
									 [&key](const auto& previous) { return previous.pool == key.pool; }),
						"Recreated runtime reused an old immutable allocation pool");
				deliveries[index] = Id(shared.message, control::tag::DeliveryId);
				m_consumers[index]->Submit();
				break;
			}
			Require(std::chrono::steady_clock::now() < deadline,
					"Completed consumers did not return all delivery capacity");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	AwaitConsumers(m_consumers);
	for (const auto delivery : deliveries)
	{
		const auto info =
			Exchange(client, control::OpCode::DeliveryInfo, {control::Field::U64(control::tag::DeliveryId, delivery)});
		CheckResponse(info);
		const auto* phase = info.message.Find(control::tag::ConsumerPhase);
		Require(phase && phase->AsText() == "complete", "Runtime did not observe actual consumer completion");
	}
	Release(client, preview, deliveries);
	m_consumers = {};
	Require(s_validationErrors.load() == 0, "Pending-runtime consumer reported Vulkan validation errors");
	m_complete = true;
	m_active = false;
	std::puts(
		m_gated
			? "pending-runtime: gate signaled; all eight slots recovered with new identities and completed GPU copies"
			: "pending-runtime: late reads completed after producer destruction; all eight slots recovered with new "
			  "identities");
}

void WindowsPendingDelivery::CheckWatchdog() const
{
	if (m_process.value != INVALID_HANDLE_VALUE)
	{
		Command(4);
		return;
	}
	if (m_watchdogExpired)
		PreservePending("native call returned only after the watchdog released pending consumer work");
}

void WindowsPendingDelivery::Command(std::uint8_t command) const
{
	DWORD transferred = 0;
	Require(WriteFile(m_pipe.value, &command, sizeof(command), &transferred, nullptr) && transferred == sizeof(command),
			"Cannot send pending-consumer command");
	std::array<char, 1024> response{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (true)
	{
		DWORD available = 0;
		Require(PeekNamedPipe(m_pipe.value, nullptr, 0, nullptr, &available, nullptr),
				"Pending consumer closed its command pipe");
		if (available >= response.size())
			break;
		Require(std::chrono::steady_clock::now() < deadline && WaitForSingleObject(m_process.value, 0) == WAIT_TIMEOUT,
				"Pending consumer did not answer");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Require(ReadFile(m_pipe.value, response.data(), static_cast<DWORD>(response.size()), &transferred, nullptr) &&
				transferred == response.size(),
			"Incomplete pending-consumer response");
	Require(response.front() == '\0', response.data());
}

void WindowsPendingDelivery::StartWatchdog()
{
	m_watchdog = std::thread([this] {
		try
		{
			std::unique_lock lock(m_watchdogMutex);
			if (m_watchdogChanged.wait_for(lock, std::chrono::seconds(10), [this] { return m_stopWatchdog; }))
				return;
			m_watchdogExpired = true;
			lock.unlock();
			std::fputs("FAIL: pending-runtime watchdog expired after 10 seconds; signaling the GPU gate\n", stderr);
			std::fflush(stderr);
			VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
			signal.semaphore = m_gate;
			signal.value = 1;
			if (m_signal(m_device->binding.device, &signal) != VK_SUCCESS)
				PreservePending("watchdog could not signal the GPU gate");
			lock.lock();
			if (!m_watchdogChanged.wait_for(lock, std::chrono::seconds(2), [this] { return m_stopWatchdog; }))
				PreservePending("native teardown did not return after the watchdog released its gate");
		}
		catch (...)
		{
			PreservePending("pending-consumer watchdog failed");
		}
	});
}

void WindowsPendingDelivery::StopWatchdog()
{
	{
		std::lock_guard lock(m_watchdogMutex);
		m_stopWatchdog = true;
	}
	m_watchdogChanged.notify_all();
	if (m_watchdog.joinable())
		m_watchdog.join();
}
} // namespace simxr::probe
