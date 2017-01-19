#!/bin/sh
# 'register' is a deprecated keyword in C++1x
gperf -t value_types_by_class.gperf | sed 's/register //g' > value_types_by_class.hpp
gperf -t value_types_by_cql.gperf | sed 's/register //g' > value_types_by_cql.hpp
