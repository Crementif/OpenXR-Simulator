#include "../TestSupport.h"
#include "platform/windows/PreviewCapturePixels.h"
#include "platform/windows/PreviewFrameSchedule.h"
#include <vector>

#include <iostream>
#include <limits>

using simxr::windows::PreviewExtent;
using simxr::windows::PreviewFrameSchedule;

int main()
{
	try
	{
		PreviewFrameSchedule schedule;
		const auto first = schedule.Begin();
		Require(first.has_value(), "First preview frame was not admitted");
		Require(schedule.Begin() == first, "Repeated layer begin changed the recording slot");
		Require(!schedule.Release(*first), "Recording frame was released as complete");
		const auto sequence = schedule.Queue();
		Require(sequence == 1, "Preview fence identity must begin at one");
		schedule.Observe(100);
		Require(schedule.State(*first) == PreviewFrameSchedule::Phase::Queued,
				"A fence observation completed an unpublished host submission");
		Require(schedule.Submitted(*first), "Queue publication failed");
		schedule.Observe(0);
		Require(!schedule.Release(*first), "Pending GPU work became reusable");
		schedule.Observe(std::numeric_limits<std::uint64_t>::max());
		Require(!schedule.Release(*first), "Device removal was interpreted as completion");
		for (std::size_t index = 1; index < PreviewFrameSchedule::Capacity; ++index)
		{
			const auto admitted = schedule.Begin();
			Require(admitted.has_value(), "Preview ring capacity shrank");
			Require(schedule.Queue().has_value(), "Frame queueing failed");
			Require(schedule.Submitted(*admitted), "Frame publication failed");
		}
		Require(!schedule.Begin(), "Preview exceeded its bounded native capacity");
		schedule.Observe(1);
		Require(schedule.Release(*first), "Actually completed frame was not released");
		Require(!schedule.Release(*first), "Completion was consumed twice");
		const auto reused = schedule.Begin();
		Require(reused == first, "Completed slot was not reused");
		schedule.CancelRecording();
		Require(schedule.State(*first) == PreviewFrameSchedule::Phase::Free, "Unsubmitted rollback held capacity");
		Require(schedule.LastQueued() == 3, "Rollback consumed an extra fence identity");
		schedule.CloseAdmission();
		Require(!schedule.Begin(), "Closed presentation admitted more work");
		Require(!schedule.Idle(), "Close fabricated GPU completion");
		schedule.Observe(3);
		Require(schedule.Idle(), "Actual final fence failed to settle the ring");
		Require(!schedule.Submitted(PreviewFrameSchedule::Capacity), "Invalid native slot accepted");

		Require(!PreviewExtent::Make(0, 1), "Zero width accepted");
		Require(!PreviewExtent::Make(1, 0), "Zero height accepted");
		Require(!PreviewExtent::Make(std::numeric_limits<std::uint32_t>::max(), 2), "Overflow width accepted");
		Require(!PreviewExtent::Make(2, std::numeric_limits<std::uint32_t>::max()), "Overflow height accepted");
		const auto extent = PreviewExtent::Make(65, 7);
		Require(extent && extent->rowPitch == 512 && extent->readbackBytes == 3584,
				"Explicit readback footprint alignment is incorrect");
		const auto largest = PreviewExtent::Make(16384, 16384);
		Require(largest && largest->readbackBytes == std::uint64_t{16384} * 16384 * 4,
				"Maximum native extent arithmetic overflowed");

		using simxr::windows::PreviewCaptureLayout;
		const auto layout = PreviewCaptureLayout::Make(128, 64, 3, 4, 2, {10, 7, 2, 3});
		Require(layout && layout->Slice() == 2 && layout->Subresource() == 6, "Raw quad slice/mip identity changed");
		Require(layout->Crop().x == 10 && layout->Crop().y == 7 && layout->Extent().rowPitch == 256,
				"Raw quad crop/footprint changed");
		Require(!PreviewCaptureLayout::Make(128, 64, 3, 4, 4, {10, 7, 2, 3}), "Invalid array slice accepted");
		Require(!PreviewCaptureLayout::Make(128, 64, 3, 4, 0, {127, 7, 2, 3}), "Out-of-source crop accepted");
		Require(!PreviewCaptureLayout::Make(128, 64, 3, 4, 0, {10, 63, 2, 3}), "Out-of-source rows accepted");
		Require(!PreviewCaptureLayout::Make(128, 64, 0, 4, 0, {10, 7, 2, 3}), "Zero mip count accepted");
		Require(!PreviewCaptureLayout::Make(128, 64, UINT32_MAX, 4, 2, {10, 7, 2, 3}),
				"Subresource multiplication overflow accepted");
		Require(PreviewCaptureLayout::Make(8192, 4096, 1, 1, 0, {0, 0, 8192, 4096}).has_value(),
				"Maximum explicit capture bound rejected");
		Require(!PreviewCaptureLayout::Make(8192, 4097, 1, 1, 0, {0, 0, 8192, 4097}),
				"Explicit raw capture exceeded its byte bound");
		std::vector<std::uint8_t> mapped(layout->Extent().readbackBytes, 0xEE);
		for (std::size_t row = 0; row < 3; ++row)
			for (std::size_t pixel = 0; pixel < 2; ++pixel)
			{
				const auto offset = row * 256 + pixel * 4;
				mapped[offset] = 11;
				mapped[offset + 1] = 22;
				mapped[offset + 2] = 33;
				mapped[offset + 3] = static_cast<std::uint8_t>(row * 2 + pixel);
			}
		const auto rgba = layout->Rgba(mapped, true);
		Require(rgba.size() == 24, "Raw quad output included padded GPU rows");
		for (std::size_t pixel = 0; pixel < 6; ++pixel)
			Require(rgba[pixel * 4] == 33 && rgba[pixel * 4 + 1] == 22 && rgba[pixel * 4 + 2] == 11 &&
						rgba[pixel * 4 + 3] == pixel,
					"BGRA swizzle changed source bytes or alpha");
		const auto alreadyRgba = layout->Rgba(mapped, false);
		Require(alreadyRgba[0] == 11 && alreadyRgba[2] == 33, "RGBA input was swizzled unnecessarily");
		bool truncatedRejected = false;
		try
		{
			layout->Rgba(std::span(mapped).first(mapped.size() - 1), true);
		}
		catch (const std::invalid_argument&)
		{
			truncatedRejected = true;
		}
		Require(truncatedRejected, "Truncated mapped buffer was read");
		std::vector<std::uint8_t> sourcePixels(4 * 3 * 4);
		for (std::size_t pixel = 0; pixel < 12; ++pixel)
		{
			sourcePixels[pixel * 4] = static_cast<std::uint8_t>(pixel);
			sourcePixels[pixel * 4 + 3] = 255;
		}
		const auto crop = simxr::windows::CopyPreviewRgbaCrop(sourcePixels, 4, 3, {1, 1, 2, 2});
		Require(crop.size() == 16 && crop[0] == 5 && crop[4] == 6 && crop[8] == 9 && crop[12] == 10 && crop[3] == 255 &&
					sourcePixels[0] == 0,
				"Tightly packed crop lost source offset, row pitch, alpha or ownership");
		for (const auto invalid : {simxr::windows::PreviewCrop{3, 0, 2, 1}, {0, 3, 1, 1}, {0, 0, 0, 1}})
		{
			bool rejected = false;
			try
			{
				simxr::windows::CopyPreviewRgbaCrop(sourcePixels, 4, 3, invalid);
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}
			Require(rejected, "Invalid tightly packed crop was accepted");
		}
		const auto flipped = simxr::windows::CopyPreviewRgbaCrop(sourcePixels, 4, 3, {1, 1, 2, 2}, true);
		Require(flipped[0] == 9 && flipped[4] == 10 && flipped[8] == 5 && flipped[12] == 6,
				"OpenGL crop flip used full-image rather than crop-relative rows");
		const auto padded = simxr::windows::PadPreviewRgba(crop, 2, 2, 4, 3);
		Require(padded.size() == 48 && padded[0] == 5 && padded[4] == 6 && padded[16] == 9 && padded[20] == 10 &&
					padded[8] == 0 && padded[11] == 255 && padded[32] == 0 && padded[35] == 255,
				"Asymmetric stereo screenshot did not preserve exact crop and opaque black padding");
		bool overflowRejected = false;
		try
		{
			simxr::windows::CopyPreviewRgbaCrop(sourcePixels, 0x80000000u, 0x80000000u, {0, 0, 1, 1});
		}
		catch (const std::invalid_argument&)
		{
			overflowRejected = true;
		}
		Require(overflowRejected, "Full-image byte-count overflow bypassed the preview copy bound");
		std::cout << "PASS: bounded preview publication, actual fence reuse, cancellation and extent bounds\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
