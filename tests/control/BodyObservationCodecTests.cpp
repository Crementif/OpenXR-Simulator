#include "../TestSupport.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "simulation/BodyModel.h"

#include <iostream>

namespace simxr::tests
{
namespace wire = control;

static BodyObservation Sample(BodyJointSet set)
{
	BodyObservation value;
	value.jointSet = set;
	value.requestedTime = {Duration{9007199254740993LL}};
	value.located = true;
	const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
	if (fb)
	{
		value.returnedTime = RuntimeTime{Duration{9007199254740991LL}};
		value.active = true;
		value.confidence = .75f;
		value.skeletonRevision = 0;
		value.skeleton.emplace();
	}
	else
		value.allJointPosesTracked = false;
	const auto map = BodyModel::JointMap(set);
	for (const auto joint : map)
	{
		const auto index = static_cast<std::size_t>(joint);
		value.joints[index] = ObservedBodyJoint{IdentityPose(), XR_SPACE_LOCATION_ORIENTATION_VALID_BIT};
		value.joints[index]->pose.position.x = static_cast<float>(index);
		if (fb)
			(*value.skeleton)[index] =
				SkeletonJoint{joint, joint == map[0] ? std::nullopt : std::optional{map[0]}, IdentityPose()};
	}
	return value;
}

static wire::Field& Find(std::vector<wire::Field>& fields, std::uint16_t tag)
{
	const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.tag == tag; });
	Require(found != fields.end(), "Fixture field");
	return *found;
}

static void RoundTrips()
{
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		SetBodyControls command{{}, Sample(set)};
		const auto encoded = wire::EncodeSimulationCommand(command);
		const auto decoded = wire::DecodeSimulationCommand(encoded);
		const auto& value = *std::get<SetBodyControls>(decoded).observation;
		Require(wire::EncodeSimulationCommand(decoded).value == encoded.value && encoded.value.size() < 65536,
				"Bounded canonical body command round trip");
		Require(value.requestedTime == command.observation->requestedTime &&
					value.returnedTime == command.observation->returnedTime &&
					value.active == command.observation->active &&
					value.allJointPosesTracked == command.observation->allJointPosesTracked &&
					value.skeletonRevision == command.observation->skeletonRevision,
				"Optional false/zero metadata and 64-bit time retained");
		for (std::size_t index = 0; index < BodyJointCount; ++index)
		{
			Require(value.joints[index].has_value() == command.observation->joints[index].has_value(),
					"Sparse schema membership");
			if (value.joints[index])
				Require(value.joints[index]->flags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT &&
							value.joints[index]->pose.position.x == index,
						"Joint validity/pose retained");
		}
		for (unsigned fault = 0; fault < 5; ++fault)
		{
			auto changed = encoded;
			auto envelope = wire::DecodeFields(changed.value).fields;
			auto payload = wire::DecodeFields(Find(envelope, wire::tag::Values).value).fields;
			auto body = wire::DecodeFields(Find(payload, wire::tag::BodyObservation).value).fields;
			if (fault == 0)
				Find(body, 2) = wire::Field::U32(2, 10, true);
			if (fault == 1)
				body.push_back(wire::Field::Bool(65000, true, true));
			if (fault >= 2)
			{
				auto joints = wire::DecodeFields(Find(body, 9).value).fields;
				if (fault == 2)
					joints.pop_back();
				if (fault == 3)
					std::swap(joints.front(), joints.back());
				if (fault == 4)
					joints.back().tag = BodyJointCount + 1;
				Find(body, 9).value = wire::EncodeFields(joints);
			}
			Find(payload, wire::tag::BodyObservation).value = wire::EncodeFields(body);
			Find(envelope, wire::tag::Values).value = wire::EncodeFields(payload);
			changed.value = wire::EncodeFields(envelope);
			bool rejected = false;
			try
			{
				wire::DecodeSimulationCommand(changed);
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}
			Require(rejected, "Malformed body wire data must reject atomically");
		}
		CommandRecording recording;
		recording.origin.configuration.tracking.bodyObservation = command.observation;
		bool rejected = false;
		try
		{
			wire::SimulationRecording(recording);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Observed bodies cannot be mislabeled as recording schema 3");
	}
	const auto anchored = wire::EncodeSimulationCommand(
		SetBodyControls{{}, Sample(BodyJointSet::UpperBodyFB), RuntimeTime{Duration{9007199254740993LL}}});
	const auto decodedAnchor = wire::DecodeSimulationCommand(anchored);
	Require(std::get<SetBodyControls>(decodedAnchor).observationOrigin->value.count() == 9007199254740993LL &&
				wire::EncodeSimulationCommand(decodedAnchor).value == anchored.value,
			"Body replay anchor lost integer precision in transport");
	for (const auto origin : {0LL, -1LL})
	{
		bool rejected = false;
		try
		{
			wire::EncodeSimulationCommand(
				SetBodyControls{{}, Sample(BodyJointSet::UpperBodyFB), RuntimeTime{Duration{origin}}});
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Nonpositive replay anchor accepted");
	}
	const auto authored = wire::DecodeSimulationCommand(wire::EncodeSimulationCommand(SetBodyControls{}));
	Require(!std::get<SetBodyControls>(authored).observation, "Legacy authored body command unchanged");
}
} // namespace simxr::tests

int main(int argc, char** argv)
{
	try
	{
		if (argc == 2 && std::string_view(argv[1]) == "--python-fixtures")
		{
			for (const auto set : {simxr::BodyJointSet::UpperBodyFB, simxr::BodyJointSet::FullBodyMETA,
								   simxr::BodyJointSet::WithoutArmsBD, simxr::BodyJointSet::FullBodyBD})
			{
				const auto field = simxr::control::EncodeSimulationCommand(simxr::SetBodyControls{
					{}, simxr::tests::Sample(set), simxr::RuntimeTime{simxr::Duration{9007199254740993LL}}});
				for (const auto byte : field.value)
				{
					const auto value = std::to_integer<unsigned>(byte);
					std::cout << "0123456789abcdef"[value >> 4] << "0123456789abcdef"[value & 15];
				}
				std::cout << '\n';
			}
			return 0;
		}
		simxr::tests::RoundTrips();
		std::cout << "Body observation codec passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
