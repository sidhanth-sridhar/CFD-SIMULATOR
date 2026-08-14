#include "cfd/core/Error.hpp"

#include <filesystem>
#include <format>

namespace cfd {

std::string_view toString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::InvalidArgument:       return "InvalidArgument";
    case ErrorCode::OutOfRange:            return "OutOfRange";
    case ErrorCode::NotFound:              return "NotFound";
    case ErrorCode::IoFailure:             return "IoFailure";
    case ErrorCode::NotImplemented:        return "NotImplemented";
    case ErrorCode::InitializationFailure: return "InitializationFailure";
    case ErrorCode::NumericalFailure:      return "NumericalFailure";
    case ErrorCode::Internal:              return "Internal";
  }
  // Reached only if an ErrorCode is constructed from an out-of-range integer.
  return "UnknownErrorCode";
}

std::string Error::format() const {
  // std::source_location::file_name() is an absolute path on most toolchains,
  // which makes log lines unreadable. Show just the filename.
  const std::string_view file = origin_.file_name();
  const std::filesystem::path path{file};
  const std::string name = path.filename().string();

  return std::format("{}: {} ({}:{})", toString(code_), message_,
                     name.empty() ? file : std::string_view{name}, origin_.line());
}

}  // namespace cfd
