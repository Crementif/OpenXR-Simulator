#include "app/Notices.h"

#include <algorithm>
#include <imgui.h>

namespace simxr::app
{
namespace
{
constexpr auto Lifetime = std::chrono::seconds(6);
constexpr std::size_t MaximumShown = 3;
} // namespace
void Notices::Push(Notice notice)
{
	std::erase_if(m_entries,
				  [&](const Entry& entry)
				  {
					  return entry.notice.title == notice.title;
				  });
	m_entries.push_back({std::move(notice), std::chrono::steady_clock::now()});
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
	const float margin = ImGui::GetStyle().WindowPadding.x;
	ImGui::SetNextWindowPos({area.x + area.width - margin, area.y + margin}, ImGuiCond_Always, {1, 0});
	ImGui::SetNextWindowBgAlpha(.92f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
								   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
								   ImGuiWindowFlags_NoNav;
	if (ImGui::Begin("##Notices", nullptr, flags))
	{
		for (std::size_t index = m_entries.size(); index-- > 0;)
		{
			const auto& notice = m_entries[index].notice;
			ImGui::PushID(static_cast<int>(index));
			ImGui::TextColored({0.96f, 0.69f, 0.32f, 1}, "!");
			ImGui::SameLine();
			ImGui::TextUnformatted(notice.title.c_str());
			if (!notice.detail.empty() && ImGui::BeginItemTooltip())
			{
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
				ImGui::TextUnformatted(notice.detail.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("x"))
				m_entries[index].shown = now - Lifetime - std::chrono::seconds(1);
			ImGui::SetItemTooltip("Dismiss");
			ImGui::PopID();
		}
	}
	ImGui::End();
}
} // namespace simxr::app
