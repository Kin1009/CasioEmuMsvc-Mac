#include "CtlJson.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace casioctl {
	namespace {
		constexpr int kMaxDepth = 64;

		void AppendUtf8(std::string& out, uint32_t code) {
			if (code <= 0x7f) {
				out.push_back(static_cast<char>(code));
			}
			else if (code <= 0x7ff) {
				out.push_back(static_cast<char>(0xc0 | (code >> 6)));
				out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
			}
			else if (code <= 0xffff) {
				out.push_back(static_cast<char>(0xe0 | (code >> 12)));
				out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
				out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
			}
			else {
				out.push_back(static_cast<char>(0xf0 | (code >> 18)));
				out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
				out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
				out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
			}
		}

		void AppendEscaped(std::string& out, const std::string& value) {
			out.push_back('"');
			for (unsigned char ch : value) {
				switch (ch) {
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\b':
					out += "\\b";
					break;
				case '\f':
					out += "\\f";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					if (ch < 0x20) {
						char buffer[7];
						std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
						out += buffer;
					}
					else {
						out.push_back(static_cast<char>(ch));
					}
					break;
				}
			}
			out.push_back('"');
		}

		class Parser {
		public:
			Parser(const std::string& text) : text_(text) {}

			bool ParseValue(Json& out, int depth, std::string& error) {
				if (depth > kMaxDepth) {
					error = "maximum nesting depth exceeded";
					return false;
				}
				SkipWhitespace();
				if (pos_ >= text_.size()) {
					error = "unexpected end of input";
					return false;
				}
				const char ch = text_[pos_];
				switch (ch) {
				case '{':
					return ParseObject(out, depth, error);
				case '[':
					return ParseArray(out, depth, error);
				case '"': {
					std::string value;
					if (!ParseString(value, error))
						return false;
					out = Json(value);
					return true;
				}
				case 't':
					if (!ConsumeLiteral("true")) {
						error = "invalid literal";
						return false;
					}
					out = Json(true);
					return true;
				case 'f':
					if (!ConsumeLiteral("false")) {
						error = "invalid literal";
						return false;
					}
					out = Json(false);
					return true;
				case 'n':
					if (!ConsumeLiteral("null")) {
						error = "invalid literal";
						return false;
					}
					out = Json();
					return true;
				default:
					if (ch == '-' || (ch >= '0' && ch <= '9'))
						return ParseNumber(out, error);
					error = "unexpected character";
					return false;
				}
			}

			bool AtEnd() {
				SkipWhitespace();
				return pos_ >= text_.size();
			}

			const std::string& ErrorPosition() const { return error_position_; }

		private:
			void SkipWhitespace() {
				while (pos_ < text_.size()) {
					const char ch = text_[pos_];
					if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
						++pos_;
					else
						break;
				}
			}

			bool ConsumeLiteral(const char* literal) {
				const size_t length = std::strlen(literal);
				if (text_.compare(pos_, length, literal) != 0)
					return false;
				pos_ += length;
				return true;
			}

			bool ParseObject(Json& out, int depth, std::string& error) {
				++pos_; // '{'
				out = Json::Object();
				SkipWhitespace();
				if (pos_ < text_.size() && text_[pos_] == '}') {
					++pos_;
					return true;
				}
				for (;;) {
					SkipWhitespace();
					if (pos_ >= text_.size() || text_[pos_] != '"') {
						error = "expected object key";
						return false;
					}
					std::string key;
					if (!ParseString(key, error))
						return false;
					SkipWhitespace();
					if (pos_ >= text_.size() || text_[pos_] != ':') {
						error = "expected ':' after object key";
						return false;
					}
					++pos_;
					Json value;
					if (!ParseValue(value, depth + 1, error))
						return false;
					out.Fields()[key] = std::move(value);
					SkipWhitespace();
					if (pos_ >= text_.size()) {
						error = "unterminated object";
						return false;
					}
					if (text_[pos_] == ',') {
						++pos_;
						continue;
					}
					if (text_[pos_] == '}') {
						++pos_;
						return true;
					}
					error = "expected ',' or '}' in object";
					return false;
				}
			}

			bool ParseArray(Json& out, int depth, std::string& error) {
				++pos_; // '['
				out = Json::Array();
				SkipWhitespace();
				if (pos_ < text_.size() && text_[pos_] == ']') {
					++pos_;
					return true;
				}
				for (;;) {
					Json value;
					if (!ParseValue(value, depth + 1, error))
						return false;
					out.Items().push_back(std::move(value));
					SkipWhitespace();
					if (pos_ >= text_.size()) {
						error = "unterminated array";
						return false;
					}
					if (text_[pos_] == ',') {
						++pos_;
						continue;
					}
					if (text_[pos_] == ']') {
						++pos_;
						return true;
					}
					error = "expected ',' or ']' in array";
					return false;
				}
			}

			bool ParseString(std::string& out, std::string& error) {
				out.clear();
				++pos_; // opening quote
				while (pos_ < text_.size()) {
					const unsigned char ch = static_cast<unsigned char>(text_[pos_++]);
					if (ch == '"')
						return true;
					if (ch != '\\') {
						if (ch < 0x20) {
							error = "control character in string";
							return false;
						}
						out.push_back(static_cast<char>(ch));
						continue;
					}
					if (pos_ >= text_.size()) {
						error = "unterminated escape";
						return false;
					}
					const char esc = text_[pos_++];
					switch (esc) {
					case '"':
						out.push_back('"');
						break;
					case '\\':
						out.push_back('\\');
						break;
					case '/':
						out.push_back('/');
						break;
					case 'b':
						out.push_back('\b');
						break;
					case 'f':
						out.push_back('\f');
						break;
					case 'n':
						out.push_back('\n');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case 't':
						out.push_back('\t');
						break;
					case 'u': {
						uint32_t code = 0;
						if (!ParseHex4(code, error))
							return false;
						if (code >= 0xd800 && code <= 0xdbff && pos_ + 1 < text_.size() &&
							text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
							pos_ += 2;
							uint32_t low = 0;
							if (!ParseHex4(low, error))
								return false;
							if (low >= 0xdc00 && low <= 0xdfff) {
								code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
							}
							else {
								AppendUtf8(out, code);
								code = low;
							}
						}
						AppendUtf8(out, code);
						break;
					}
					default:
						error = "invalid escape sequence";
						return false;
					}
				}
				error = "unterminated string";
				return false;
			}

			bool ParseHex4(uint32_t& value, std::string& error) {
				if (pos_ + 4 > text_.size()) {
					error = "truncated \\u escape";
					return false;
				}
				value = 0;
				for (int i = 0; i < 4; ++i) {
					const char ch = text_[pos_++];
					value <<= 4;
					if (ch >= '0' && ch <= '9')
						value |= static_cast<uint32_t>(ch - '0');
					else if (ch >= 'a' && ch <= 'f')
						value |= static_cast<uint32_t>(ch - 'a' + 10);
					else if (ch >= 'A' && ch <= 'F')
						value |= static_cast<uint32_t>(ch - 'A' + 10);
					else {
						error = "invalid hex digit in \\u escape";
						return false;
					}
				}
				return true;
			}

			bool ParseNumber(Json& out, std::string& error) {
				const size_t start = pos_;
				if (pos_ < text_.size() && text_[pos_] == '-')
					++pos_;
				bool is_integer = true;
				while (pos_ < text_.size()) {
					const char ch = text_[pos_];
					if (ch >= '0' && ch <= '9') {
						++pos_;
						continue;
					}
					if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
						is_integer = false;
						++pos_;
						continue;
					}
					break;
				}
				const std::string token = text_.substr(start, pos_ - start);
				if (token.empty() || token == "-") {
					error = "invalid number";
					return false;
				}
				if (is_integer) {
					char* end = nullptr;
					errno = 0;
					const long long value = std::strtoll(token.c_str(), &end, 10);
					if (errno == 0 && end && *end == '\0') {
						out = Json(static_cast<int64_t>(value));
						return true;
					}
				}
				char* end = nullptr;
				const double value = std::strtod(token.c_str(), &end);
				if (!end || *end != '\0') {
					error = "invalid number";
					return false;
				}
				out = Json(value);
				return true;
			}

			const std::string& text_;
			size_t pos_ = 0;
			std::string error_position_;
		};
	} // namespace

	Json::Json() = default;
	Json::Json(std::nullptr_t) : type_(Type::Null) {}
	Json::Json(bool value) : type_(Type::Bool), bool_(value) {}
	Json::Json(int value) : type_(Type::Int), int_(value) {}
	Json::Json(unsigned int value) : type_(Type::Int), int_(static_cast<int64_t>(value)) {}
	Json::Json(long value) : type_(Type::Int), int_(static_cast<int64_t>(value)) {}
	Json::Json(unsigned long value) : type_(Type::Int), int_(static_cast<int64_t>(value)) {}
	Json::Json(long long value) : type_(Type::Int), int_(static_cast<int64_t>(value)) {}
	Json::Json(unsigned long long value) : type_(Type::Int), int_(static_cast<int64_t>(value)) {}
	Json::Json(double value) : type_(Type::Double), double_(value) {}
	Json::Json(const char* value) : type_(Type::String), string_(value ? value : "") {}
	Json::Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

	Json Json::Array() {
		Json value;
		value.type_ = Type::Array;
		return value;
	}

	Json Json::Object() {
		Json value;
		value.type_ = Type::Object;
		return value;
	}

	bool Json::AsBool(bool fallback) const {
		if (type_ == Type::Bool)
			return bool_;
		if (type_ == Type::Int)
			return int_ != 0;
		return fallback;
	}

	int64_t Json::AsInt(int64_t fallback) const {
		if (type_ == Type::Int)
			return int_;
		if (type_ == Type::Double) {
			if (double_ >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
				double_ <= static_cast<double>(std::numeric_limits<int64_t>::max()))
				return static_cast<int64_t>(double_);
			return fallback;
		}
		if (type_ == Type::Bool)
			return bool_ ? 1 : 0;
		return fallback;
	}

	double Json::AsDouble(double fallback) const {
		if (type_ == Type::Double)
			return double_;
		if (type_ == Type::Int)
			return static_cast<double>(int_);
		return fallback;
	}

	std::string Json::AsString(const std::string& fallback) const {
		if (type_ == Type::String)
			return string_;
		return fallback;
	}

	const Json* Json::Find(const std::string& key) const {
		if (type_ != Type::Object)
			return nullptr;
		const auto it = object_.find(key);
		return it == object_.end() ? nullptr : &it->second;
	}

	bool Json::Has(const std::string& key) const {
		return Find(key) != nullptr;
	}

	Json& Json::operator[](const std::string& key) {
		if (type_ != Type::Object) {
			type_ = Type::Object;
			array_.clear();
			string_.clear();
			bool_ = false;
			int_ = 0;
			double_ = 0.0;
		}
		return object_[key];
	}

	Json& Json::Set(const std::string& key, Json value) {
		Json& slot = (*this)[key];
		slot = std::move(value);
		return *this;
	}

	Json& Json::Push(Json value) {
		if (type_ != Type::Array)
			type_ = Type::Array;
		array_.push_back(std::move(value));
		return *this;
	}

	const Json& Json::operator[](size_t index) const {
		static const Json kNull;
		if (type_ != Type::Array || index >= array_.size())
			return kNull;
		return array_[index];
	}

	void Json::DumpTo(std::string& out, int indent, int depth) const {
		switch (type_) {
		case Type::Null:
			out += "null";
			return;
		case Type::Bool:
			out += bool_ ? "true" : "false";
			return;
		case Type::Int:
			out += std::to_string(int_);
			return;
		case Type::Double: {
			char buffer[40];
			if (std::isfinite(double_)) {
				std::snprintf(buffer, sizeof(buffer), "%.17g", double_);
				out += buffer;
				if (!std::strchr(buffer, '.') && !std::strchr(buffer, 'e') && !std::strchr(buffer, 'E'))
					out += ".0";
			}
			else {
				out += "null";
			}
			return;
		}
		case Type::String:
			AppendEscaped(out, string_);
			return;
		case Type::Array: {
			out.push_back('[');
			const bool pretty = indent > 0 && !array_.empty();
			for (size_t i = 0; i < array_.size(); ++i) {
				if (i)
					out.push_back(',');
				if (pretty) {
					out.push_back('\n');
					out.append(static_cast<size_t>(indent * (depth + 1)), ' ');
				}
				array_[i].DumpTo(out, indent, depth + 1);
			}
			if (pretty) {
				out.push_back('\n');
				out.append(static_cast<size_t>(indent * depth), ' ');
			}
			out.push_back(']');
			return;
		}
		case Type::Object: {
			out.push_back('{');
			const bool pretty = indent > 0 && !object_.empty();
			bool first = true;
			for (const auto& entry : object_) {
				if (!first)
					out.push_back(',');
				first = false;
				if (pretty) {
					out.push_back('\n');
					out.append(static_cast<size_t>(indent * (depth + 1)), ' ');
				}
				AppendEscaped(out, entry.first);
				out.push_back(':');
				if (pretty)
					out.push_back(' ');
				entry.second.DumpTo(out, indent, depth + 1);
			}
			if (pretty) {
				out.push_back('\n');
				out.append(static_cast<size_t>(indent * depth), ' ');
			}
			out.push_back('}');
			return;
		}
		}
	}

	std::string Json::Dump() const {
		std::string out;
		out.reserve(128);
		DumpTo(out, 0, 0);
		return out;
	}

	std::string Json::DumpPretty(int indent) const {
		std::string out;
		out.reserve(256);
		DumpTo(out, indent < 0 ? 0 : indent, 0);
		return out;
	}

	bool Json::Parse(const std::string& text, Json& out, std::string& error) {
		Parser parser(text);
		Json value;
		if (!parser.ParseValue(value, 0, error))
			return false;
		if (!parser.AtEnd()) {
			error = "trailing characters after JSON value";
			return false;
		}
		out = std::move(value);
		return true;
	}

} // namespace casioctl
