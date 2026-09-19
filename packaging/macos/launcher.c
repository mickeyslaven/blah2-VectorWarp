/* VectorWarp app-bundle launcher: selects only a packaged architecture runtime. */
#include <mach-o/dyld.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(__arm64__)
#define VECTORWARP_RUNTIME_ARCH "arm64"
#elif defined(__x86_64__)
#define VECTORWARP_RUNTIME_ARCH "x86_64"
#else
#error "VectorWarp launcher supports only arm64 and x86_64"
#endif

static int executable_path(char *result, size_t result_size) {
  uint32_t size = 0;
  char *unresolved;
  if (_NSGetExecutablePath(NULL, &size) != -1 || size == 0) {
    fprintf(stderr, "VectorWarp launcher: cannot determine executable path\n");
    return -1;
  }
  unresolved = malloc(size);
  if (unresolved == NULL) {
    fprintf(stderr, "VectorWarp launcher: cannot allocate executable path\n");
    return -1;
  }
  if (_NSGetExecutablePath(unresolved, &size) != 0 ||
      realpath(unresolved, result) == NULL) {
    fprintf(stderr, "VectorWarp launcher: cannot resolve executable path: %s\n",
        strerror(errno));
    free(unresolved);
    return -1;
  }
  free(unresolved);
  if (strlen(result) >= result_size) {
    fprintf(stderr, "VectorWarp launcher: resolved executable path is too long\n");
    return -1;
  }
  return 0;
}

static int bundle_root(char *path) {
  int component;
  for (component = 0; component < 3; ++component) {
    char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) return -1;
    *slash = '\0';
  }
  return 0;
}

static int is_finder_process_serial_number(const char *argument) {
  return strncmp(argument, "-psn_", 5) == 0;
}

int main(int argc, char *const argv[]) {
  char executable[PATH_MAX];
  char payload[PATH_MAX];
  char **arguments;
  size_t root_length;
  int payload_length;
  int index;
  int output = 0;

  if (executable_path(executable, sizeof(executable)) != 0 || bundle_root(executable) != 0) {
    fprintf(stderr, "VectorWarp launcher: expected an executable inside App.app/Contents/MacOS\n");
    return 127;
  }
  root_length = strlen(executable);
  payload_length = snprintf(payload, sizeof(payload),
      "%s/Contents/Resources/runtime/%s/script/vectorwarp-standalone",
      executable, VECTORWARP_RUNTIME_ARCH);
  if (payload_length < 0 || (size_t)payload_length >= sizeof(payload) || root_length == 0) {
    fprintf(stderr, "VectorWarp launcher: runtime payload path is too long\n");
    return 127;
  }
  if (access(payload, X_OK) != 0) {
    fprintf(stderr, "VectorWarp launcher: required %s runtime payload is unavailable: %s\n",
        VECTORWARP_RUNTIME_ARCH, payload);
    return 127;
  }
  arguments = calloc((size_t)argc + 1, sizeof(*arguments));
  if (arguments == NULL) {
    fprintf(stderr, "VectorWarp launcher: cannot allocate argument vector\n");
    return 127;
  }
  arguments[output++] = payload;
  for (index = 1; index < argc; ++index) {
    if (!is_finder_process_serial_number(argv[index])) arguments[output++] = argv[index];
  }
  arguments[output] = NULL;
  execv(payload, arguments);
  fprintf(stderr, "VectorWarp launcher: cannot start %s runtime payload: %s\n",
      VECTORWARP_RUNTIME_ARCH, strerror(errno));
  free(arguments);
  return 127;
}
