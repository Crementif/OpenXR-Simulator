#include "app/ProofConsumer.h"
#include "app/UiRenderer.h"
#include "probes/CompanionFrameReceiver.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <map>

namespace simxr::app
{
struct ProofConsumer::Impl
{
	struct Texture
	{
		CompanionProofFrame frame;
		VkDescriptorSet descriptor;
		bool consumed = false;
	};
	UiRenderer& renderer;
	std::unique_ptr<CompanionFrameReceiver> receiver;
	std::map<std::uint32_t, Texture> textures;
	std::vector<std::uint32_t> drawn;
	std::uint64_t allSampledSerial = 0;
	bool verifyPixels;
	bool finished = false;

	Impl(UiRenderer& renderer, int descriptor, bool verify)
		: renderer(renderer), receiver(std::make_unique<CompanionFrameReceiver>(renderer.DeviceAccess(), descriptor)),
		  verifyPixels(verify)
	{
	}
	~Impl()
	{
		try
		{
			renderer.WaitIdle();
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "Companion proof UI completion remains unknown: %s\n", error.what());
			std::_Exit(1);
		}
		for (const auto& [id, texture] : textures)
			ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
		receiver.reset();
	}
};

ProofConsumer::ProofConsumer(UiRenderer& renderer, int descriptor, bool verifyPixels)
	: m_impl(std::make_unique<Impl>(renderer, descriptor, verifyPixels))
{
}
ProofConsumer::~ProofConsumer() = default;

void ProofConsumer::Poll()
{
	m_impl->receiver->Poll();
	for (const auto& frame : m_impl->receiver->ReadyFrames())
		if (!m_impl->textures.contains(frame.id))
		{
			const auto descriptor = ImGui_ImplVulkan_AddTexture(frame.sampler, frame.view, frame.layout);
			if (!descriptor)
				throw std::runtime_error("Proof image descriptor allocation failed");
			try
			{
				m_impl->textures.emplace(frame.id, Impl::Texture{frame, descriptor});
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
	m_impl->drawn.clear();
	ImGui::Separator();
	ImGui::TextUnformatted("GPU sharing proof stream");
	ImGui::Text("Retained private images: %zu / 3", m_impl->textures.size());
	for (const auto& [id, texture] : m_impl->textures)
	{
		ImGui::BeginGroup();
		ImGui::Text("Frame %u", id);
		ImGui::Image((ImTextureID)texture.descriptor, {96, 96});
		ImGui::EndGroup();
		if (id != m_impl->textures.rbegin()->first)
			ImGui::SameLine();
		m_impl->drawn.push_back(id);
	}
}

std::vector<VkSemaphore> ProofConsumer::ReadyWaits() const
{
	std::vector<VkSemaphore> result;
	for (auto id : m_impl->drawn)
	{
		const auto& texture = m_impl->textures.at(id);
		if (!texture.consumed)
			result.push_back(texture.frame.sampleReady);
	}
	return result;
}

void ProofConsumer::Submitted(std::uint64_t serial)
{
	for (auto id : m_impl->drawn)
		m_impl->textures.at(id).consumed = true;
	if (!m_impl->allSampledSerial && m_impl->receiver->AllCopied() && m_impl->drawn.size() == 3)
		m_impl->allSampledSerial = serial;
}

bool ProofConsumer::FinishIfReady()
{
	if (m_impl->finished)
		return true;
	if (!m_impl->allSampledSerial || m_impl->renderer.CompletedFrames() < m_impl->allSampledSerial)
		return false;
	for (const auto& [id, texture] : m_impl->textures)
		m_impl->receiver->MarkSampled(id);
	m_impl->receiver->Finish(m_impl->verifyPixels);
	m_impl->finished = true;
	return true;
}

bool ProofConsumer::ReadyForScreenshot() const
{
	return m_impl->receiver->AllCopied() && m_impl->drawn.size() == 3;
}
} // namespace simxr::app
