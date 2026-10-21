#pragma once

// CPU-only service/transport fixture. Its memfd and pipe are deliberately not
// Vulkan image/semaphore handles; native GPU qualification lives in the probes.
#include "../TestSupport.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/ImageInspection.h"
#include "platform/linux/ControlFd.h"
#include "runtime/Runtime.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/mman.h>
#include <unistd.h>

namespace simxr::control::test
{
struct State
{
	std::atomic<bool> failReaderDescription{false}, failDeliveryDescription{false}, failExportDescription{false};
	std::atomic<unsigned> reads{0}, shares{0}, exports{0}, retains{0}, exposures{0};
	std::atomic<unsigned> readersDestroyed{0}, deliveriesDestroyed{0}, exportsDestroyed{0};
	std::atomic<CompletionStatus> exportPhase{CompletionStatus::Pending}, consumerPhase{CompletionStatus::Pending};
	std::atomic<std::uint64_t> nextImage{1};
	std::function<void()> onReaderDestroyed;
};
inline ImageDescription Description()
{
	ImageDescription result;
	for (std::size_t index = 0; index < 16; ++index)
	{
		result.identity.device[index] = std::byte(index + 1);
		result.identity.driver[index] = std::byte(32 + index);
	}
	result.format = 43;
	result.width = 4;
	result.height = 3;
	result.arrayLayers = 2;
	result.usage = 7;
	result.allocationSize = 4096;
	result.dedicated = true;
	result.layout = 1;
	return result;
}
inline std::vector<std::byte> Pixels()
{
	std::vector<std::byte> result(4 * 3 * 2 * 4);
	for (std::size_t index = 0; index < result.size(); ++index)
		result[index] = std::byte((index * 17 + 3) & 255);
	return result;
}
class Output final : public FrameOutput
{
  public:
	explicit Output(std::shared_ptr<FrameReservation> slot) : slot(std::move(slot)) {}
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	std::shared_ptr<FrameReservation> slot;
};
struct Data
{
	std::vector<std::byte> pixels = Pixels();
	ImageDescription description = Description();
	std::shared_ptr<const FrameOutput> source;
};
class Delivery final : public ImageDelivery
{
  public:
	Delivery(std::shared_ptr<State> state, std::shared_ptr<Data> data, std::shared_ptr<TransferHandle> consumer)
		: state(std::move(state)), data(std::move(data)), consumer(std::move(consumer)), image(this->state->nextImage++)
	{
		memory = OwnControlFd(::memfd_create("simxr-cpu-image-fixture", MFD_CLOEXEC));
		Require(::write(static_cast<int>(memory->Value()), this->data->pixels.data(), this->data->pixels.size()) ==
					static_cast<ssize_t>(this->data->pixels.size()),
				"Write fixture memfd");
		int pipe[2];
		Require(::pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0, "Create fixture readiness pipe");
		ready = OwnControlFd(pipe[0]);
		auto writer = OwnControlFd(pipe[1]);
		const char value = 1;
		Require(::write(static_cast<int>(writer->Value()), &value, 1) == 1, "Signal fixture pipe");
	}
	~Delivery() override
	{
		++state->deliveriesDestroyed;
	}
	SharedImageKey Key() const noexcept override
	{
		return {{0xfedcba9876543210ULL}, {image}};
	}
	ImageDescription Description() const override
	{
		if (state->failDeliveryDescription.exchange(false))
			throw std::bad_alloc{};
		return data->description;
	}
	CompletionStatus ProducerCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	CompletionStatus ConsumerCompletion() const noexcept override
	{
		return state->consumerPhase.load();
	}
	std::vector<std::shared_ptr<TransferHandle>> PrepareHandles() override
	{
		return {memory->Duplicate(), ready->Duplicate()};
	}
	void MarkTransferred() noexcept override
	{
		++state->exposures;
	}

  private:
	std::shared_ptr<State> state;
	std::shared_ptr<Data> data;
	std::shared_ptr<TransferHandle> consumer, memory, ready;
	std::uint64_t image;
};
class Export final : public ImageExport
{
  public:
	Export(std::shared_ptr<State> state, std::shared_ptr<Data> data, std::optional<std::uint32_t> layer)
		: state(std::move(state)), data(std::move(data)), description(this->data->description)
	{
		if (layer && *layer >= description.arrayLayers)
			throw ImageInspectionError{ErrorCode::Invalid, "Fixture layer outside array"};
		const auto size = this->data->pixels.size() / description.arrayLayers;
		if (layer)
		{
			description.arrayLayers = 1;
			bytes.assign(this->data->pixels.begin() + *layer * size, this->data->pixels.begin() + (*layer + 1) * size);
		}
		else
			bytes = this->data->pixels;
	}
	~Export() override
	{
		++state->exportsDestroyed;
	}
	ImageDescription Description() const override
	{
		if (state->failExportDescription.exchange(false))
			throw std::bad_alloc{};
		return description;
	}
	std::uint64_t ByteSize() const noexcept override
	{
		return bytes.size();
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return state->exportPhase.load();
	}
	std::span<const std::byte> Read() override
	{
		++state->reads;
		if (PollCompletion() != CompletionStatus::Complete)
			throw ImageInspectionError{ErrorCode::Busy, "Fixture export not ready"};
		return bytes;
	}

  private:
	std::shared_ptr<State> state;
	std::shared_ptr<Data> data;
	ImageDescription description;
	std::vector<std::byte> bytes;
};
class Reader final : public ImageReader
{
  public:
	Reader(std::shared_ptr<State> state, std::shared_ptr<Data> data) : state(std::move(state)), data(std::move(data)) {}
	~Reader() override
	{
		++state->readersDestroyed;
		if (state->onReaderDestroyed)
			state->onReaderDestroyed();
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	ImageDescription Description() const override
	{
		if (state->failReaderDescription.exchange(false))
			throw std::bad_alloc{};
		return data->description;
	}
	ImageCapabilities Capabilities() const noexcept override
	{
		return {true, true};
	}
	std::shared_ptr<ImageDelivery> Share(const ImageDeviceIdentity& identity,
										 const std::shared_ptr<TransferHandle>& consumer) const override
	{
		if (identity != data->description.identity)
			throw ImageInspectionError{ErrorCode::Invalid, "Fixture UUID mismatch"};
		if (!consumer || consumer->Kind() != TransferHandleKind::OpaqueFd ||
			::fcntl(static_cast<int>(consumer->Value()), F_GETFD) == -1)
			throw ImageInspectionError{ErrorCode::Invalid, "Fixture consumer FD invalid"};
		++state->shares;
		return std::make_shared<Delivery>(state, data, consumer->Duplicate());
	}
	std::shared_ptr<ImageExport> Export(std::optional<std::uint32_t> layer) const override
	{
		++state->exports;
		return std::make_shared<test::Export>(state, data, layer);
	}

  private:
	std::shared_ptr<State> state;
	std::shared_ptr<Data> data;
};
class Provider final : public ImageInspectionProvider
{
  public:
	explicit Provider(std::shared_ptr<State> state) : state(std::move(state)) {}
	ImageCapabilities Capabilities(XrSession) const noexcept override
	{
		return {true, true};
	}
	std::shared_ptr<ImageReader> Retain(std::shared_ptr<const FrameOutput> output) const override
	{
		++state->retains;
		auto data = std::make_shared<Data>();
		data->source = std::move(output);
		return std::make_shared<Reader>(state, std::move(data));
	}

  private:
	std::shared_ptr<State> state;
};
struct Fixture
{
	Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	FramePool pool{16};
	std::shared_ptr<State> state = std::make_shared<State>();
	std::shared_ptr<Provider> provider = std::make_shared<Provider>(state);
	ControlService service{runtime, 16, provider};
	CaptureId capture;
	Fixture()
	{
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "CPU image endpoint fixture");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS &&
					session->ConfigureCapturePool(pool) == XR_SUCCESS,
				"Create CPU image fixture");
		FrameSchedulerConfig config;
		config.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS &&
					session->Display().Pause(),
				"Begin CPU image fixture");
		session->Scheduler().Pause();
		capture = Submit();
	}
	CaptureId Submit()
	{
		auto reservation = session->ReserveCapture();
		Require(reservation.result == SnapshotAdmission::Accepted && session->Scheduler().GrantPermits() == XR_SUCCESS,
				"Reserve CPU image fixture");
		FramePrediction prediction;
		Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
				"Issue CPU image fixture");
		const FrameKey key{session->Snapshot().generation, prediction.id};
		auto token = session->Captures().ProducerReservation(key);
		Require(pool.Consume(token), "Consume CPU fixture token");
		auto output = std::make_shared<Output>(std::move(token));
		auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
															std::vector<LayerDescription>{QuadLayerDescription{}});
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
								 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
		Require(session->EndFrame(prediction, &end, std::move(metadata), std::move(output)) == XR_SUCCESS,
				"Submit CPU fixture output");
		session->Captures().Poll();
		Require(session->Captures().Read(reservation.id)->phase == SnapshotPhase::Ready,
				"CPU fixture capture not ready");
		return reservation.id;
	}
	~Fixture()
	{
		service.Close();
		runtime.Close();
	}
};
} // namespace simxr::control::test
