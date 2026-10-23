#pragma once

namespace simxr::windows
{
// The caller must keep an independent module reference until destruction returns.
class ModuleReference final
{
  public:
	ModuleReference();
	~ModuleReference() noexcept;
	ModuleReference(const ModuleReference&) = delete;
	ModuleReference& operator=(const ModuleReference&) = delete;
	ModuleReference(ModuleReference&& other) noexcept;
	ModuleReference& operator=(ModuleReference&& other) noexcept;

  private:
	void Release() noexcept;
	void* m_module = nullptr;
};
} // namespace simxr::windows
