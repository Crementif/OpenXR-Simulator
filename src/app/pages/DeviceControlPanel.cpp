#include "app/pages/DeviceControlPanel.h"
#include "Pch.h"
#include "app/services/PlayerRig.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include <exception>
#include <imgui_internal.h>
#include <stdexcept>

namespace simxr::app
{
DeviceControlPanel::DeviceControlPanel(PlayerRig& rig) : m_playerRig(rig) {}
bool DeviceControlPanel::Active() const
{
	return m_activeField && ImGui::GetActiveID() == m_activeField;
}
void DeviceControlPanel::CancelEditing()
{
	if (!m_activeField)
		return;
	if (Active())
		ImGui::ClearActiveID();
	// ImGui retains a final text value after deactivation, including when another page reuses its ID.
	auto& context = *ImGui::GetCurrentContext();
	if (context.DeactivatedItemData.ID == m_activeField)
		context.DeactivatedItemData = {};
	if (context.InputTextDeactivatedState.ID == m_activeField)
		context.InputTextDeactivatedState.ID = 0;
	m_activeField = 0;
	m_fieldFrame = -1;
}
void DeviceControlPanel::SelectDevice(const DeviceSel& device)
{
	const unsigned slot =
		device.page == DevicePage::Trackers ? std::min(device.slot, static_cast<unsigned>(MaxTrackerDevices - 1)) : 0;
	if (!m_selection || m_selection->page != device.page || m_selection->slot != slot)
	{
		CancelEditing();
		m_selection = DeviceSel{device.page, slot};
	}
	if (device.page == DevicePage::Trackers)
		m_selectedTracker = static_cast<int>(slot);
	m_fieldScope = "device/" + std::to_string(static_cast<int>(device.page)) + "/" + std::to_string(slot) + "/";
}
bool DeviceControlPanel::CanControl(ControlChannel channel) const
{
	if (!m_observed)
		return false;
	const auto owner = m_observed->owners[static_cast<std::size_t>(channel)];
	return !owner || owner == m_manualSource;
}
bool DeviceControlPanel::CanReset(DevicePage page) const
{
	const auto allowed = [&](ControlChannel channel)
	{
		return CanControl(channel) && !ScenarioActive(channel);
	};
	switch (page)
	{
	case DevicePage::Player:
		return allowed(ControlChannel::Head) && allowed(ControlChannel::LeftPose) &&
			   allowed(ControlChannel::RightPose) && allowed(ControlChannel::LeftControls) &&
			   allowed(ControlChannel::RightControls) && allowed(ControlChannel::Body) &&
			   allowed(ControlChannel::Gaze) && allowed(ControlChannel::Trackers);
	case DevicePage::Head:
		return allowed(ControlChannel::Head);
	case DevicePage::Left:
		return allowed(ControlChannel::LeftPose) && allowed(ControlChannel::LeftControls);
	case DevicePage::Right:
		return allowed(ControlChannel::RightPose) && allowed(ControlChannel::RightControls);
	case DevicePage::Body:
		return allowed(ControlChannel::Body) && m_observed->bodyOverridesAvailable;
	case DevicePage::Gaze:
		return allowed(ControlChannel::Gaze);
	case DevicePage::Trackers:
		return allowed(ControlChannel::Trackers) && m_observed->trackerAttachmentsAvailable &&
			   !m_observed->values.tracking.trackers.empty();
	}
	return false;
}
std::optional<SimulationEdit> DeviceControlPanel::Draw(bool supported, const Selection* selection,
													   const std::function<void()>& placement)
{
	m_edit.reset();
	m_continuousEdit = false;
	if (selection)
		if (const auto device = std::get_if<DeviceSel>(selection))
			SelectDevice(*device);
	if (m_observed && !m_pending && !Active())
		LoadObservedSettings();
	if (supported && !m_observed)
	{
		ImGui::TextDisabled("Waiting for runtime values\u2026");
		if (!m_observationError.empty())
			ImGui::SetItemTooltip("%s", m_observationError.c_str());
	}
	supported = supported && m_observed.has_value() && m_configurationLoaded;
	ImGui::PushItemWidth(-1);
	if (selection)
	{
		ImGui::BeginDisabled(!supported);
		const auto device = std::get_if<DeviceSel>(selection);
		const bool hand = device && (device->page == DevicePage::Left || device->page == DevicePage::Right);
		if (placement && !hand)
		{
			ImGui::EndDisabled();
			placement();
			ImGui::BeginDisabled(!supported);
		}
		if (device)
		{
			switch (device->page)
			{
			case DevicePage::Player:
				DrawPlayer();
				break;
			case DevicePage::Head:
				DrawHead();
				DrawViews();
				break;
			case DevicePage::Left:
			case DevicePage::Right:
				DrawController(device->page == DevicePage::Right, placement);
				break;
			case DevicePage::Body:
				DrawBody();
				break;
			case DevicePage::Gaze:
				DrawGaze();
				break;
			case DevicePage::Trackers:
				DrawTrackers();
				break;
			}
		}
		ImGui::EndDisabled();
	}
	ImGui::PopItemWidth();
	return std::exchange(m_edit, {});
}
bool DeviceControlPanel::Field(const char* key, bool changed, bool continuous, const FieldValue& value)
{
	const std::string field = m_fieldScope + key;
	const bool editable = !(ImGui::GetItemFlags() & (ImGuiItemFlags_Disabled | ImGuiItemFlags_ReadOnly));
	if (editable && ImGui::IsItemActive())
	{
		m_activeField = ImGui::GetActiveID();
		m_fieldFrame = ImGui::GetFrameCount();
	}
	else if (!editable && ImGui::GetItemID() == m_activeField)
		CancelEditing();
	const bool finished = editable && continuous && ImGui::IsItemDeactivatedAfterEdit();
	changed = editable && (changed || finished);
	// A reset or wheel nudge is one complete edit.
	const bool acted = FieldActions(field.c_str(), value);
	changed |= acted;
	if (changed)
	{
		m_pendingFields.insert(field);
		m_continuousEdit = continuous && !finished && !acted;
		m_rejectedFields.erase(field);
	}
	DrawLiveField(m_pending && m_pendingFields.contains(field),
				  m_rejectedFields.contains(field) && ImGui::GetTime() < m_rejectedUntil ? m_deliveryError
																						 : std::string{});
	return changed;
}
bool DeviceControlPanel::InputField(const char* key, bool changed)
{
	const std::string field = m_fieldScope + key;
	if (changed)
	{
		m_pendingFields.insert(field);
		m_rejectedFields.erase(field);
	}
	DrawLiveField(false, m_rejectedFields.contains(field) && ImGui::GetTime() < m_rejectedUntil ? m_deliveryError
																								: std::string{});
	return changed;
}
std::optional<SimulationEdit> DeviceControlPanel::DrawHeader(const Selection& selection, bool showState)
{
	m_edit.reset();
	m_continuousEdit = false;
	const auto device = std::get_if<DeviceSel>(&selection);
	if (!device)
		return {};
	SelectDevice(*device);
	static constexpr const char* titles[] = {"Player", "Head",	   "Left hand", "Right hand",
											 "Body",   "Eye gaze", "Trackers"};
	static constexpr Icon icons[] = {Icon::Player, Icon::Head, Icon::HandLeft, Icon::HandRight,
									 Icon::Body,   Icon::Eye,  Icon::Tracker};
	const auto page = device->page;
	const bool ready = m_configurationLoaded && m_playerRig.Ready() && CanReset(page);
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	PageTitle(icons[static_cast<int>(page)], titles[static_cast<int>(page)]);
	if (page == DevicePage::Player)
	{
		const float width = ImGui::CalcTextSize("Reset all").x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(right - width);
		ImGui::BeginDisabled(!ready);
		if (ImGui::Button("Reset all"))
			ResetPage(page);
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Restore device defaults.");
	}
	else
	{
		if (showState && (page != DevicePage::Trackers || !m_authoredTracking.trackers.empty()))
		{
			ImGui::SameLine();
			DrawTracking(page);
		}
		ImGui::SameLine(right - ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!ready);
		const std::string tip =
			std::string("Restore defaults for ") + titles[static_cast<int>(page)] + ". Other pages are unchanged.";
		if (IconButton("Reset page", Icon::ResetPose, tip.c_str()))
			ResetPage(page);
		ImGui::EndDisabled();
	}
	ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y / 2});
	return std::exchange(m_edit, {});
}
void DeviceControlPanel::ResetPage(DevicePage page)
{
	if (!m_observed || !m_playerRig.Ready() || !CanReset(page))
		return;
	const auto allowed = [&](ControlChannel channel)
	{
		const auto& owner = m_observed->owners[static_cast<std::size_t>(channel)];
		return !owner || owner == m_manualSource;
	};
	std::vector<SimulationCommand> extra;
	const auto resetControls = [&](std::uint32_t hand)
	{
		const auto channel = hand ? ControlChannel::RightControls : ControlChannel::LeftControls;
		m_authoredControls[hand] = {};
		m_latches[hand] = {};
		if (!allowed(channel))
			return;
		extra.emplace_back(TakeControl{channel, false});
		extra.emplace_back(SetControllerControls{hand, ControllerControls{}});
	};
	switch (page)
	{
	case DevicePage::Player:
		m_playerRig.ResetAll();
		m_playerRig.height = m_observed->values.proportions.height;
		for (std::uint32_t hand = 0; hand < 2; ++hand)
			resetControls(hand);
		m_authoredTracking.gaze = {};
		if (allowed(ControlChannel::Gaze))
		{
			extra.emplace_back(TakeControl{ControlChannel::Gaze, false});
			extra.emplace_back(SetGaze{GazeInput{}});
		}
		break;
	case DevicePage::Head:
		m_playerRig.ResetHead();
		break;
	case DevicePage::Left:
	case DevicePage::Right:
		m_playerRig.ResetHand(page == DevicePage::Right);
		resetControls(page == DevicePage::Right);
		break;
	case DevicePage::Body:
		m_playerRig.ResetBody();
		break;
	case DevicePage::Gaze:
		m_authoredTracking.gaze = {};
		if (allowed(ControlChannel::Gaze))
			ApplyOwned(ControlChannel::Gaze, SetGaze{GazeInput{}});
		return;
	case DevicePage::Trackers:
		if (static_cast<std::size_t>(m_selectedTracker) < m_authoredTracking.trackers.size())
			m_playerRig.ResetTracker(static_cast<std::size_t>(m_selectedTracker));
		break;
	}
	auto commands = PlayerEdit(true).commands;
	for (auto& command : commands)
		if (auto controller = std::get_if<SetControllerPose>(&command);
			controller && (page == DevicePage::Player || (page == DevicePage::Left && controller->hand == 0) ||
						   (page == DevicePage::Right && controller->hand == 1)))
			controller->value.connected = true;
	for (auto& command : extra)
		commands.push_back(std::move(command));
	m_continuousEdit = false;
	Apply(std::move(commands));
	if (m_edit)
		if (auto edit = std::get_if<ManualTransaction>(&*m_edit))
			edit->releaseEdge = !extra.empty();
}
void DeviceControlPanel::ObserveDelivery(bool pending, const std::string& error)
{
	if (pending && error.empty())
		m_deliveryError.clear();
	if (!error.empty() && error != m_deliveryError)
	{
		CancelEditing();
		m_deliveryError = error;
		m_rejectedFields = m_pendingFields;
		m_rejectedUntil = ImGui::GetTime() + 3;
	}
	m_pending = pending;
	if (!pending)
		m_pendingFields.clear();
}
void DeviceControlPanel::Apply(std::vector<SimulationCommand> commands, std::string label)
{
	if (commands.empty())
		return;
	ManualTransaction transaction;
	transaction.continuous = m_continuousEdit;
	m_pending = true;
	if (m_applyFrame)
		transaction.frame = FrameId{m_applyFrame};
	transaction.latePolicy = m_reschedule ? CommandLatePolicy::RescheduleNext : CommandLatePolicy::Reject;
	transaction.commands = std::move(commands);
	transaction.label = std::move(label);
	m_edit = std::move(transaction);
}
void DeviceControlPanel::ApplyOwned(ControlChannel channel, SimulationCommand command)
{
	if (CanControl(channel))
		Apply({TakeControl{channel, false}, std::move(command)});
}
void DeviceControlPanel::ResetObservations()
{
	ClearHeldInputs();
	m_selection.reset();
	m_observed.reset();
	m_playerRig = {};
	m_rigPiloting = false;
	m_manualSource.reset();
	m_observationError.clear();
	m_configurationLoaded = false;
	m_applyFrame = 0;
	m_replaceOwner = false;
	m_edit.reset();
	m_pending = false;
	m_pendingFields.clear();
	m_rejectedFields.clear();
	m_deliveryError.clear();
}
void DeviceControlPanel::LoadObservedSettings()
{
	if (!m_observed)
		return;
	const auto& values = m_observed->values;
	const bool topologyChanged = !std::equal(values.tracking.trackers.begin(), values.tracking.trackers.end(),
											 m_authoredTracking.trackers.begin(), m_authoredTracking.trackers.end(),
											 [](const auto& a, const auto& b)
											 {
												 return a.identity == b.identity;
											 });
	if (topologyChanged)
		CancelEditing();
	if (!m_rigPiloting || topologyChanged)
		m_playerRig.Observe(*m_observed);
	m_authoredTracking = values.tracking;
	m_authoredControls = values.input.controllers;
	m_authoredViews = values.views;
	m_authoredProportions = values.proportions;
	for (unsigned hand = 0; hand != 2; ++hand)
	{
		auto& profile = m_controllerProfiles[hand];
		profile.fill(0);
		const auto& path = values.input.preferredProfiles[hand];
		std::copy(path.begin(), path.end(), profile.begin());
	}
	m_configurationLoaded = true;
}
void DeviceControlPanel::ObserveState(const control::Message& state)
{
	try
	{
		const auto previous = m_observed ? m_observed->scenarios : ScenarioState{};
		auto observed = ReadObservedSetup(state);
		const bool topologyChanged =
			!m_observed ||
			!std::equal(observed.values.tracking.trackers.begin(), observed.values.tracking.trackers.end(),
						m_observed->values.tracking.trackers.begin(), m_observed->values.tracking.trackers.end(),
						[](const auto& a, const auto& b)
						{
							return a.identity == b.identity;
						});
		for (const auto& profile : observed.values.input.preferredProfiles)
			if (profile.size() >= m_controllerProfiles.front().size())
				throw std::runtime_error("Interaction profile path is too long");
		m_observed = std::move(observed);
		for (std::size_t index = 0; index < previous.size(); ++index)
			if (previous[index] && !m_observed->scenarios[index])
				ReloadRecoveredChannel(ScenarioInputChannel(static_cast<FailureScenario>(index)));
		m_observationError.clear();
		if (!m_configurationLoaded || topologyChanged)
			LoadObservedSettings();
	}
	catch (const std::exception& error)
	{
		m_observed.reset();
		m_observationError = error.what();
		m_configurationLoaded = false;
	}
}
void DeviceControlPanel::ReloadRecoveredChannel(ControlChannel channel)
{
	// Failure observations include their effects. Once restored, refresh only
	// that device so an edit cannot latch the simulated failure into its values.
	const auto& values = m_observed->values;
	switch (channel)
	{
	case ControlChannel::Head:
		m_authoredTracking.head = values.tracking.head;
		break;
	case ControlChannel::LeftPose:
	case ControlChannel::RightPose:
	{
		const auto hand = channel == ControlChannel::LeftPose ? 0 : 1;
		m_authoredTracking.controllers[hand] = values.tracking.controllers[hand];
		m_authoredControls[hand].connected = values.input.controllers[hand].connected;
		break;
	}
	case ControlChannel::Body:
		m_authoredTracking.body = values.tracking.body;
		break;
	case ControlChannel::Gaze:
		m_authoredTracking.gaze = values.tracking.gaze;
		break;
	case ControlChannel::Trackers:
		m_authoredTracking.trackers = values.tracking.trackers;
		break;
	default:
		break;
	}
}
bool DeviceControlPanel::ScenarioActive(ControlChannel channel) const
{
	if (m_observed)
		for (std::size_t index = 0; index < m_observed->scenarios.size(); ++index)
			if (m_observed->scenarios[index] && ScenarioInputChannel(static_cast<FailureScenario>(index)) == channel)
				return true;
	return false;
}
} // namespace simxr::app
