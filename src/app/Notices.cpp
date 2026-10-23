#include "app/Notices.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"

#include <algorithm>
#include <imgui.h>

namespace simxr::app
{
namespace
{
constexpr auto Lifetime = std::chrono::seconds(6);
constexpr std::size_t MaximumShown = 3, MaximumLogged = 50;
} // namespace
void Notices::Push(Notice notice)
{
	std::erase_if(m_entries,
				  [&](const Entry& entry)
				  {
					  return entry.notice.title == notice.title;
				  });
	const auto now = std::chrono::steady_clock::now();
	if (m_log.size() == MaximumLogged)
		m_log.erase(m_log.begin());
	m_log.push_back({notice.title, notice.detail, now});
	m_unseen = true;
	m_entries.push_back({std::move(notice), now});
	if (m_entries.size() > MaximumShown)
		m_entries.erase(m_entries.begin());
}
const Notice* Notices::Latest() const
{
	return m_entries.empty() ? nullptr : &m_entries.back().notice;
}
void Notices::Draw(ShellRect area)
{
	const auto now = std::chrono::steady_clock::now();
	std::erase_if(m_entries,
				  [&](const Entry& entry)
				  {
					  return now - entry.shown > Lifetime;
				  });
	if (m_entries.empty())
		return;
	const float scale = ImGui::GetFontSize() / 16, margin = 8 * scale;
	const ImVec2 padding{10 * scale, 6 * scale};
	ImGui::SetNextWindowPos({area.x + area.width / 2, area.y + margin}, ImGuiCond_Always, {.5f, 0});
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
								   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
								   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, padding.y * 2 + 4 * scale});
	if (ImGui::Begin("##Notices", nullptr, flags))
	{
		auto* draw = ImGui::GetWindowDrawList();
		// Each notice is a card: the content first, then its backing behind it on the lower channel.
		for (std::size_t index = m_entries.size(); index-- > 0;)
		{
			const auto& notice = m_entries[index].notice;
			ImGui::PushID(notice.title.c_str());
			draw->ChannelsSplit(2);
			draw->ChannelsSetCurrent(1);
			ImGui::BeginGroup();
			const float height = ImGui::GetFrameHeight();
			const auto icon = ImGui::GetCursorScreenPos();
			DrawIcon(draw, Icon::Warning, {icon.x + height / 2, icon.y + height / 2}, height * .62f,
					 notice.run ? theme::Accent : theme::Warn);
			ImGui::Dummy({height, height});
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(notice.title.c_str());
			const bool titleHovered = ImGui::IsItemHovered();
			// A short detail reads in place; a long one waits in the tooltip and the history.
			const auto newline = notice.detail.find('\n');
			const bool brief = !notice.detail.empty() && newline == std::string::npos && notice.detail.size() <= 48;
			if (brief)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s", notice.detail.c_str());
			}
			if (notice.run)
			{
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent);
				if (ImGui::SmallButton(notice.action.c_str()))
				{
					notice.run();
					m_entries[index].shown = now - Lifetime - std::chrono::seconds(1);
				}
				ImGui::PopStyleColor();
			}
			ImGui::SameLine();
			if (IconButton("Hide notice", Icon::Close, "Hide this notice; the history keeps it"))
				m_entries[index].shown = now - Lifetime - std::chrono::seconds(1);
			ImGui::EndGroup();
			if (!brief && !notice.detail.empty() && titleHovered)
			{
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
				ImGui::TextUnformatted(notice.detail.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			draw->ChannelsSetCurrent(0);
			const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
			const ImVec2 first{minimum.x - padding.x / 2, minimum.y - padding.y},
				last{maximum.x + padding.x / 2, maximum.y + padding.y};
			draw->AddRectFilled(first, last, theme::SurfaceRaised, 6 * scale);
			draw->AddRect(first, last, theme::Line, 6 * scale);
			draw->ChannelsMerge();
			ImGui::PopID();
		}
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}
} // namespace simxr::app
