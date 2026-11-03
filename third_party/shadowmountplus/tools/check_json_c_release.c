/* Run against the actual Release library, including ASan/LSan builds. */
#include <assert.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#error Build the checker with assertions; only the dependency should use NDEBUG
#endif

static unsigned deleted;
static void delete_cookie(struct json_object *object, void *data) {
  assert(json_object_get_type(object) == json_type_object);
  unsigned *cookie = data;
  assert(*cookie == 0x187u);
  *cookie = 0;
  free(cookie);
  ++deleted;
}
static void attach_cookie(struct json_object *object) {
  unsigned *cookie = malloc(sizeof(*cookie)); assert(cookie);
  *cookie = 0x187u;
  json_object_set_userdata(object, cookie, delete_cookie);
}
int main(void) {
  /* Empty containers and scalars take the immediate destruction path. */
  assert(json_object_put(NULL) == 0);
  struct json_object *empty = json_object_new_object();
  assert(empty);
  attach_cookie(empty);
  assert(json_object_put(empty) == 1 && deleted == 1);
  assert(json_object_put(json_object_new_array()) == 1);
  assert(json_object_put(json_object_new_int(187)) == 1);
  deleted = 0;
  for (unsigned pass = 0; pass < 2000; ++pass) {
    struct json_object *root = json_object_new_object();
    struct json_object *items = json_object_new_array();
    struct json_object *shared = json_object_new_object();
    assert(root && items && shared);
    attach_cookie(root); attach_cookie(shared);
    assert(json_object_object_add(shared, "value", json_object_new_int(187)) == 0);
    struct json_object *external = json_object_get(shared);
    assert(json_object_array_add(items, shared) == 0);
    assert(json_object_array_add(items, json_object_get(shared)) == 0);
    assert(json_object_object_add(root, "items", items) == 0);
    unsigned before = deleted;
    int freed = json_object_put(root);
    assert(freed == 1 && deleted == before + 1);
    struct json_object *value;
    assert(json_object_object_get_ex(external, "value", &value));
    assert(json_object_get_int(value) == 187);
    freed = json_object_put(external);
    assert(freed == 1 && deleted == before + 2);
    struct json_tokener *tok = json_tokener_new_ex(8); assert(tok);
    const char *text = "{\"data\":[{\"value\":1.25},null,[1,2,3]],\"name\":\"test\"}";
    root = json_tokener_parse_ex(tok, text, -1);
    assert(root && json_tokener_get_error(tok) == json_tokener_success);
    json_object_put(root); json_tokener_free(tok);
    tok = json_tokener_new_ex(8); assert(tok);
    root = json_tokener_parse_ex(tok, "{\"partial\":[{\"child\":", -1);
    assert(!root && json_tokener_get_error(tok) != json_tokener_success);
    json_tokener_free(tok);
    root = json_object_new_object(); assert(root);
    struct json_object *node = root;
    for (unsigned depth = 0; depth < 512; ++depth) {
      struct json_object *child = json_object_new_object(); assert(child);
      assert(json_object_object_add(node, "next", child) == 0); node = child;
    }
    json_object_put(root);
  }
  assert(deleted == 4000);
  puts("json-c Release: nested containers, shared references, userdata cleanup and incomplete parsing passed");
  return 0;
}
