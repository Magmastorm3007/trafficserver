/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include <iostream>
#include <string>

#define CATCH_CONFIG_MAIN
#include <catch.hpp>

#include "print_funcs.h"
#include "Utils.h"

using std::cout;
using std::endl;
using std::string;
using namespace EsiLib;

void
checkAttributes(const char *check_id, const AttributeList &attr_list, const char *attr_info[])
{
  cout << check_id << ": checking attributes" << endl;
  AttributeList::const_iterator iter = attr_list.begin();
  for (int i = 0; attr_info[i]; i += 2, ++iter) {
    REQUIRE(iter->name_len == static_cast<int>(strlen(attr_info[i])));
    REQUIRE(strncmp(iter->name, attr_info[i], iter->name_len) == 0);
    REQUIRE(iter->value_len == static_cast<int>(strlen(attr_info[i + 1])));
    REQUIRE(strncmp(iter->value, attr_info[i + 1], iter->value_len) == 0);
  }
  REQUIRE(iter == attr_list.end());
}

TEST_CASE("esi utils test")
{
  Utils::init(&Debug, &Error);

  AttributeList attr_list;

  string str1("pos=SKY spaceid=12123");
  Utils::parseAttributes(str1, attr_list);
  const char *expected_strs1[] = {"pos", "SKY", "spaceid", "12123", nullptr};
  checkAttributes("test1", attr_list, expected_strs1);

  string str2("  pos=SKY	  spaceid=12123 ");
  Utils::parseAttributes(str2, attr_list);
  const char *expected_strs2[] = {"pos", "SKY", "spaceid", "12123", nullptr};
  checkAttributes("test2", attr_list, expected_strs2);

  string str3("  pos=\"SKY\"	  spaceid=12123 ");
  Utils::parseAttributes(str3, attr_list);
  const char *expected_strs3[] = {"pos", "SKY", "spaceid", "12123", nullptr};
  checkAttributes("test3", attr_list, expected_strs3);

  string str4("  pos=\" SKY BAR \"	  spaceid=12123 blah=\"foo");
  Utils::parseAttributes(str4, attr_list);
  const char *expected_strs4[] = {"pos", " SKY BAR ", "spaceid", "12123", nullptr};
  checkAttributes("test4", attr_list, expected_strs4);

  string str5(R"(a="b & xyz"&c=d&e=f&g=h")");
  Utils::parseAttributes(str5, attr_list, "&");
  const char *expected_strs5[] = {"a", "b & xyz", "c", "d", "e", "f", nullptr};
  checkAttributes("test5", attr_list, expected_strs5);

  string str6("abcd=&");
  Utils::parseAttributes(str6, attr_list, "&");
  const char *expected_strs6[] = {nullptr};
  checkAttributes("test6", attr_list, expected_strs6);

  string str7("&& abcd=& key1=val1 &=val2&val3&&");
  Utils::parseAttributes(str7, attr_list, "&");
  const char *expected_strs7[] = {"key1", "val1", nullptr};
  checkAttributes("test7", attr_list, expected_strs7);

  const char *escaped_sequence = R"({\"site-attribute\":\"content=no_expandable; ajax_cert_expandable\"})";
  string str8(R"(pos="FPM1" spaceid=96584352 extra_mime=")");
  str8.append(escaped_sequence);
  str8.append(R"(" foo=bar a="b")");
  const char *expected_strs8[] = {"pos", "FPM1", "spaceid", "96584352", "extra_mime", escaped_sequence,
                                  "foo", "bar",  "a",       "b",        nullptr};
  Utils::parseAttributes(str8, attr_list);
  checkAttributes("test8", attr_list, expected_strs8);

  REQUIRE(Utils::unescape(escaped_sequence) == "{\"site-attribute\":\"content=no_expandable; ajax_cert_expandable\"}");
  REQUIRE(Utils::unescape(nullptr) == "");
  REQUIRE(Utils::unescape("\\", 0) == "");
  REQUIRE(Utils::unescape("\\hello\"", 3) == "he");
  REQUIRE(Utils::unescape("\\hello\"", -3) == "");
  REQUIRE(Utils::unescape("hello") == "hello");

  string str9("n1=v1; n2=v2;, n3=v3, ;n4=v4=extrav4");
  Utils::parseAttributes(str9, attr_list, ";,");
  const char *expected_strs9[] = {"n1", "v1", "n2", "v2", "n3", "v3", "n4", "v4=extrav4", nullptr};
  checkAttributes("test9", attr_list, expected_strs9);

  string str10("hello=world&test=萌萌&a=b");
  Utils::parseAttributes(str10, attr_list, "&");
  const char *expected_strs10[] = {"hello", "world", "test", "萌萌", "a", "b", nullptr};
  checkAttributes("test10", attr_list, expected_strs10);

  SECTION("test 11")
  {
    std::list<string> lines;
    lines.push_back("allowlistCookie AGE");
    lines.push_back("allowlistCookie GRADE");
    lines.push_back("a b");
    Utils::KeyValueMap kv;
    Utils::HeaderValueList list;
    Utils::parseKeyValueConfig(lines, kv, list);
    REQUIRE(kv.find("a")->second == "b");
    REQUIRE(list.back() == "GRADE");
    list.pop_back();
    REQUIRE(list.back() == "AGE");
    list.pop_back();
  }
}
