#include "chtholly/next_task_v1.h"

#include <stdbool.h>
#include <stddef.h>

typedef void (*wake_entry_fn)(void *);
typedef void (*release_fn)(void *);

static int cancel_arm = 0;
static int arm_count = 0;
static bool branch_value = true;
static int foreign_resource_close_count = 0;
static int temporary_drop_log = 0;

int os_open(int code) { return code; }
void os_close(int handle) {
  (void)handle;
  ++foreign_resource_close_count;
}
int os_close_count(void) { return foreign_resource_close_count; }

void record_temporary_drop(int value) {
  temporary_drop_log = temporary_drop_log * 3 + value;
}
int read_temporary_drop_log(void) {
  return temporary_drop_log;
}

void c_set_cancel_arm(int value) {
  cancel_arm = value;
  arm_count = 0;
}

void c_set_branch(int value) {
  branch_value = value != 0;
}

bool c_branch(void) {
  return branch_value;
}

void *c_register(void *entry, void *userdata) {
  (void)entry;
  (void)userdata;
  return (void *)1;
}

void *c_register_transferred(void *entry, void *userdata, void *release) {
  (void)entry;
  (void)userdata;
  (void)release;
  return (void *)1;
}

void c_unregister(void *handle) { (void)handle; }
void c_cancel(void *handle) { (void)handle; }
void *c_cancel_async(void *handle) {
  (void)handle;
  return (void *)1;
}
void c_wait(void *token) { (void)token; }
bool c_poll(void *token) {
  (void)token;
  return false;
}

bool c_arm(void *userdata, void *token, release_fn release,
           wake_entry_fn entry) {
  (void)token;
  ++arm_count;
  if (cancel_arm != 0 && arm_count == cancel_arm)
    (void)chtholly_next_task_v1_task_request_cancel(
        (chtholly_next_task_v1_task *)userdata);
  else
    entry(userdata);
  release(userdata);
  return false;
}

void c_detach(release_fn release, void *token, void *userdata) {
  (void)token;
  release(userdata);
}

void c_detach_transferred(void *token) { (void)token; }
