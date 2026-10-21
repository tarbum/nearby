// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#import "internal/platform/implementation/apple/Log/GNCLogger.h"

#import <Foundation/Foundation.h>
#import <os/log.h>

#include <string.h>

#include "internal/platform/logging.h"

NS_ASSUME_NONNULL_BEGIN

// Mirror of the Nearby Connections log stream in Apple's unified logging system.
// absl writes to stderr, which is /dev/null for GUI apps; os_log makes the same
// messages visible via `log stream --process QuickShareMac` (or Console.app),
// and unlike absl VLOG it does not require a verbosity flag to be enabled.
static os_log_t GNCNearbyLog(void) {
  static os_log_t log;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    log = os_log_create("com.google.nearby", "GNCLogger");
  });
  return log;
}

static void GNCWriteToOSLog(GNCLogSeverity severity, NSString *message, const char *fileName,
                            int lineNumber) {
  os_log_type_t type;
  switch (severity) {
    case GNCLogSeverityDebug:
      type = OS_LOG_TYPE_DEBUG;
      break;
    case GNCLogSeverityInfo:
      type = OS_LOG_TYPE_INFO;
      break;
    case GNCLogSeverityWarning:
      type = OS_LOG_TYPE_DEFAULT;
      break;
    case GNCLogSeverityError:
      type = OS_LOG_TYPE_ERROR;
      break;
    case GNCLogSeverityFatal:
      type = OS_LOG_TYPE_FAULT;
      break;
    default:
      type = OS_LOG_TYPE_DEFAULT;
      break;
  }
  const char *baseName = strrchr(fileName, '/');
  baseName = baseName ? baseName + 1 : fileName;
  os_log_with_type(GNCNearbyLog(), type, "%{public}s:%d %{public}s", baseName, lineNumber,
                   [message UTF8String]);
}

void GNCLog(const char *fileName, int lineNumber, GNCLogSeverity severity, NSString *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  NSString *messageString = [[NSString alloc] initWithFormat:format arguments:arguments];
  va_end(arguments);
  GNCWriteToOSLog(severity, messageString, fileName, lineNumber);
  switch (severity) {
    case GNCLogSeverityDebug:
      VLOG(1).AtLocation(fileName, lineNumber).WithVerbosity(1)
          << std::string([messageString UTF8String]);
      break;
    case GNCLogSeverityInfo:
      LOG(INFO).AtLocation(fileName, lineNumber) << std::string([messageString UTF8String]);
      break;
    case GNCLogSeverityWarning:
      LOG(WARNING).AtLocation(fileName, lineNumber) << std::string([messageString UTF8String]);
      break;
    case GNCLogSeverityError:
      LOG(ERROR).AtLocation(fileName, lineNumber) << std::string([messageString UTF8String]);
      break;
    case GNCLogSeverityFatal:
      LOG(FATAL).AtLocation(fileName, lineNumber) << std::string([messageString UTF8String]);
      break;
  }
}

NS_ASSUME_NONNULL_END
