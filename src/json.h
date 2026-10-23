#pragma once

#include <string>
#include <string_view>

namespace json
{
// A lenient, non-owning view of one compatibility JSON object. The document must
// outlive this view; omitted, malformed or incorrectly typed fields use defaults.
class Object
{
  public:
	Object() = default;
	explicit Object(const char* text);
	bool Valid() const;
	bool Has(const char* key) const;
	std::string_view Raw(const char* key) const;
	// Double reads retain nonfinite values for callers that reject invalid input.
	double Number(const char* key, double fallback) const;
	float Number(const char* key, float fallback) const;
	int Number(const char* key, int fallback) const;
	bool Boolean(const char* key, bool fallback) const;
	std::string String(const char* key, const char* fallback = "") const;
	Object Child(const char* key) const;

  private:
	const char* Find(const char* key) const;
	const char* m_begin = nullptr;
};
} // namespace json
