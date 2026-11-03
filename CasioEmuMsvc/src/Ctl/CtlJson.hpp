#pragma once

// Minimal, dependency-free JSON value used by the casioctl control channel.
// The same header is compiled into the emulator (C++20) and the casioctl CLI
// (C++17), so keep it free of project and third-party includes.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace casioctl {

	class Json {
	public:
		enum class Type {
			Null,
			Bool,
			Int,
			Double,
			String,
			Array,
			Object,
		};

		Json();
		Json(std::nullptr_t);
		Json(bool value);
		Json(int value);
		Json(unsigned int value);
		Json(long value);
		Json(unsigned long value);
		Json(long long value);
		Json(unsigned long long value);
		Json(double value);
		Json(const char* value);
		Json(std::string value);

		static Json Array();
		static Json Object();

		Type GetType() const { return type_; }
		bool IsNull() const { return type_ == Type::Null; }
		bool IsBool() const { return type_ == Type::Bool; }
		bool IsNumber() const { return type_ == Type::Int || type_ == Type::Double; }
		bool IsString() const { return type_ == Type::String; }
		bool IsArray() const { return type_ == Type::Array; }
		bool IsObject() const { return type_ == Type::Object; }

		bool AsBool(bool fallback = false) const;
		int64_t AsInt(int64_t fallback = 0) const;
		double AsDouble(double fallback = 0.0) const;
		std::string AsString(const std::string& fallback = std::string()) const;

		const std::vector<Json>& Items() const { return array_; }
		const std::map<std::string, Json>& Fields() const { return object_; }
		std::vector<Json>& Items() { return array_; }
		std::map<std::string, Json>& Fields() { return object_; }

		// Object helpers. operator[] promotes a null value to an object.
		const Json* Find(const std::string& key) const;
		bool Has(const std::string& key) const;
		Json& operator[](const std::string& key);
		Json& Set(const std::string& key, Json value);
		Json& Push(Json value);

		const Json& operator[](size_t index) const;

		std::string Dump() const;
		std::string DumpPretty(int indent = 2) const;

		static bool Parse(const std::string& text, Json& out, std::string& error);

	private:
		void DumpTo(std::string& out, int indent, int depth) const;

		Type type_ = Type::Null;
		bool bool_ = false;
		int64_t int_ = 0;
		double double_ = 0.0;
		std::string string_;
		std::vector<Json> array_;
		std::map<std::string, Json> object_;
	};

} // namespace casioctl
