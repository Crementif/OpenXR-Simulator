#include "app/render/ProofConsumer.h"
#include "app/UiPch.h"
#include "app/render/UiRenderer.h"

#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

namespace simxr::app
{
ProofConsumer::ProofConsumer(UiRenderer& renderer, int descriptor, bool verifyPixels)
	: m_renderer(renderer), m_receiver(renderer.DeviceAccess(), descriptor), m_verifyPixels(verifyPixels)
{
}
ProofConsumer::~ProofConsumer()
{
	try
	{
		m_renderer.WaitIdle();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Companion proof UI completion remains unknown: %s\n", error.what());
		std::_Exit(1);
	}
	for (const auto& [id, texture] : m_textures)
		ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
}

void ProofConsumer::Poll()
{
	m_receiver.Poll();
	for (const auto& frame : m_receiver.ReadyFrames())
		if (!m_textures.contains(frame.id))
		{
			const auto descriptor = ImGui_ImplVulkan_AddTexture(frame.sampler, frame.view, frame.layout);
			if (!descriptor)
				throw std::runtime_error("Proof image descriptor allocation failed");
			try
			{
				m_textures.emplace(frame.id, Texture{frame, descriptor});
			}
			catch (...)
			{
				ImGui_ImplVulkan_RemoveTexture(descriptor);
				throw;
			}
		}
}

void ProofConsumer::Draw()
{
	m_drawn.clear();
	if (ImGui::GetCurrentWindow()->SkipItems)
		return;
	ImGui::Separator();
	ImGui::TextUnformatted("GPU sharing proof stream");
	ImGui::Text("Retained private images: %zu / 3", m_textures.size());
	for (const auto& [id, texture] : m_textures)
	{
		ImGui::BeginGroup();
		ImGui::Text("Frame %u", id);
		ImGui::Image(std::bit_cast<ImTextureID>(texture.descriptor), {96, 96});
		if (ImGui::IsItemVisible())
			m_drawn.push_back(id);
		ImGui::EndGroup();
		if (id != m_textures.rbegin()->first)
			ImGui::SameLine();
	}
}

std::vector<VkSemaphore> ProofConsumer::ReadyWaits() const
{
	std::vector<VkSemaphore> result;
	for (auto id : m_drawn)
	{
		const auto& texture = m_textures.at(id);
		if (!texture.consumed)
			result.push_back(texture.frame.sampleReady);
	}
	return result;
}

void ProofConsumer::Submitted(std::uint64_t serial)
{
	for (auto id : m_drawn)
		m_textures.at(id).consumed = true;
	if (!m_allSampledSerial && m_receiver.AllCopied() && m_drawn.size() == 3)
		m_allSampledSerial = serial;
}

bool ProofConsumer::FinishIfReady()
{
	if (m_finished)
		return true;
	if (!m_allSampledSerial || m_renderer.CompletedFrames() < m_allSampledSerial)
		return false;
	for (const auto& [id, texture] : m_textures)
		m_receiver.MarkSampled(id);
	m_receiver.Finish(m_verifyPixels);
	m_finished = true;
	return true;
}

bool ProofConsumer::ReadyForScreenshot() const
{
	return m_receiver.AllCopied() && m_drawn.size() == 3;
}
} // namespace simxr::app
