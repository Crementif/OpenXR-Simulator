#include "../TestSupport.h"
#include "Math.h"
#include "app/SimulationEdits.h"
#include "app/SpatialMath.h"
#include "control/ControlSchema.h"
#include "control/FrameInspection.h"
#include "control/ImageInspection.h"
#include "simulation/TrackingSystem.h"
#include <cmath>
#include <iostream>
#include <numbers>

namespace
{
using namespace simxr;
using namespace simxr::app;
void Near(float actual, float expected)
{
	Require(std::abs(actual - expected) < .0001f, "Spatial coordinate differs");
}
void MatricesAndProjection()
{
	for (float yaw : {0.0f, .4f, 2.8f, -3.0f})
		for (float pitch : {0.0f, 1.1f, -2.0f})
		{
			const XrPosef pose{EulerAngles(yaw, pitch, .7f), {2, 1, -3}};
			const auto result = MatrixPose(PoseMatrix(pose));
			const auto delta = RelativePose(result, pose);
			Near(delta.position.x, 0);
			Near(delta.position.y, 0);
			Near(delta.position.z, 0);
			Near(std::abs(delta.orientation.w), 1);
			const auto inverse = MatrixPose(ViewMatrix(pose));
			const auto identity = Compose(inverse, pose);
			Near(identity.position.x, 0);
			Near(identity.position.y, 0);
			Near(identity.position.z, 0);
		}
	const FrameView camera{{EulerAngles(.3f, -.2f, .1f), {2, 1, 4}}, {-.4f, .8f, .7f, -.3f}};
	const XrVector3f point{std::tan(camera.fov.angleRight) * 2, std::tan(camera.fov.angleUp) * 2, -2};
	const auto projected = ProjectPoint(camera, Compose(camera.pose, {{0, 0, 0, 1}, point}).position);
	Near(projected.uv.x, 1);
	Near(projected.uv.y, 0);
	Require(!projected.behind, "Front point marked behind");
	const auto projection = ProjectionMatrix(camera.fov);
	const float x = (projection[0] * point.x + projection[8] * point.z) / -point.z;
	const float y = (projection[5] * point.y + projection[9] * point.z) / -point.z;
	Near((x + 1) / 2, projected.uv.x);
	Near((1 - y) / 2, projected.uv.y);
	const auto behind = ProjectPoint({IdentityPose(), {-.5f, .5f, .5f, -.5f}}, {1, 0, 2});
	Require(behind.behind && behind.uv.x < .5f, "Behind-eye arrow did not mirror its direction");
	const auto rotation = ArcballRotation({0, 0}, {0, 1});
	const auto rotated = Rotate(rotation, {0, 0, 1});
	Near(rotated.y, 1);
	Near(rotated.z, 0);
	const auto snapped = Rotate(SnapRotation(EulerAngles(.30f, 0, 0), std::numbers::pi_v<float> / 12), {0, 0, -1});
	Near(snapped.x, -std::sin(std::numbers::pi_v<float> / 12));
}
void MetadataAndHistory()
{
	control::ImageDescription image;
	image.arrayLayers = 2;
	image.views = {{{EulerAngles(.2f, .1f, 0), {2, 1.6f, -3}}, {-.4f, .8f, .7f, -.3f}},
				   {{EulerAngles(-.2f, .1f, 0), {2.064f, 1.6f, -3}}, {-.8f, .4f, .7f, -.3f}}};
	const auto decoded = control::DecodeImageDescription(control::EncodeImageDescription(image));
	Require(decoded.views.size() == 2, "Per-eye metadata lost");
	Near(decoded.views[1].pose.position.x, 2.064f);
	Near(decoded.views[0].fov.angleRight, .8f);
	image.arrayLayers = 1;
	bool rejected = false;
	try
	{
		control::DecodeImageDescription(control::EncodeImageDescription(image));
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Mismatched image/view count accepted");
	image.views.clear();
	Require(control::DecodeImageDescription(control::EncodeImageDescription(image)).views.empty(),
			"Legacy metadata fabricated a camera");
	FrameRecord frame;
	frame.key = {{1}, {55}};
	frame.stage = FrameStage::Submitted;
	frame.submission.emplace();
	TrackingState state;
	state.head.pose.position = {4, 1.6f, 2};
	state.controllers[1].pose.pose.position = {4.2f, 1.3f, 1.5f};
	state.body.rootPose = {EulerAngles(.4f, 0, 0), {4, 0, 2}};
	frame.submission->tracking = TrackingSystem::Evaluate(RuntimeTime{Duration{10}}, state, BodyModel{});
	control::InspectionStore store;
	const auto record = control::InspectFrame(store, {1}, frame);
	control::Message message;
	message.fields.push_back(control::Field::Bytes(control::tag::Document, record->document));
	const auto historical = ReadInspectionTracking(message);
	Require(historical.has_value(), "Historical tracking was not decoded");
	Near(historical->head.pose.position.x, 4);
	Near(historical->controllers[1].pose.pose.position.z, 1.5f);
	Near(historical->body.joints[static_cast<std::size_t>(BodyJoint::Root)].pose.position.x, 4);
}
} // namespace
int main()
{
	try
	{
		MatricesAndProjection();
		MetadataAndHistory();
		std::cout << "PASS spatial math and immutable view/history metadata\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
