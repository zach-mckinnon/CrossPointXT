#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace writerfiles {
constexpr size_t kMaxDocumentBytes = 64 * 1024;
constexpr size_t kMaxDocuments = 40;

// Restrict names so the USB protocol cannot escape /drafts or inject delimiters.
bool validName(const std::string& name);
std::vector<std::string> list();
std::string nextName();
bool exists(const std::string& name);
bool read(const std::string& name, std::string& content);
bool save(const std::string& name, const std::string& content);
bool readActiveName(std::string& name);
bool saveActiveName(const std::string& name);
bool readManifest(std::string& content);
bool saveManifest(const std::string& content);
}