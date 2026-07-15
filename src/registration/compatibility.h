#pragma once

#include <cstdint>
#include <string>

namespace registration {

// 兼容工具函数集合：
// 提供符号函数、范围裁剪、字符串转数字、路径分隔符处理和环境变量读取。

int sign(int value);
int sign(std::int64_t value);
int sign(double value);

bool in_range(int value, int min_value, int max_value);
bool in_range(double value, double min_value, double max_value);

int ensure_range(int value, int min_value, int max_value);
double ensure_range(double value, double min_value, double max_value);

double str_to_double_def(const std::string& s, double default_value);

std::string include_trailing_path_delimiter(const std::string& s);
std::string exclude_trailing_path_delimiter(const std::string& s);

bool directory_exists(const std::string& path);
std::string get_environment_variable(const std::string& name);

}  // namespace registration


