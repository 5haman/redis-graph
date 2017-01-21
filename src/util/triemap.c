#include <sys/param.h>
#include "triemap.h"

size_t __trieMapNode_Sizeof(tm_len_t numChildren, tm_len_t slen) {
  return sizeof(TrieMapNode) + numChildren * sizeof(TrieMapNode *) + (slen + 1);
}

TrieMapNode *__newTrieMapNode(unsigned char *str, tm_len_t offset, tm_len_t len,
                              tm_len_t numChildren, void *value, int terminal) {
  TrieMapNode *n = calloc(1, __trieMapNode_Sizeof(numChildren, len - offset));
  n->len = len - offset;
  n->numChildren = numChildren;
  n->value = value;

  n->flags = terminal ? TM_NODE_TERMINAL : 0;

  memcpy(n->str, str + offset, (len - offset));

  return n;
}

TrieMap *NewTrieMap() {
  return __newTrieMapNode((unsigned char *)"", 0, 0, 0, NULL, 0);
}

TrieMapNode *__trieMapNode_AddChild(TrieMapNode *n, unsigned char *str, tm_len_t offset,
                                    tm_len_t len, void *value) {
  n->numChildren++;
  n = realloc((void *)n, __trieMapNode_Sizeof(n->numChildren, n->len));
  // a newly added child must be a terminal node
  TrieMapNode *child = __newTrieMapNode(str, offset, len, 0, value, 1);
  __trieMapNode_children(n)[n->numChildren - 1] = child;

  return n;
}

TrieMapNode *__trieMapNode_Split(TrieMapNode *n, tm_len_t offset) {
  // Copy the current node's data and children to a new child node
  TrieMapNode *newChild = __newTrieMapNode(n->str, offset, n->len, n->numChildren, n->value,
                                           __trieMapNode_isTerminal(n));
  newChild->flags = n->flags;
  TrieMapNode **children = __trieMapNode_children(n);
  TrieMapNode **newChildren = __trieMapNode_children(newChild);
  memcpy(newChildren, children, sizeof(TrieMapNode *) * n->numChildren);

  // reduce the node to be just one child long with no score
  n->numChildren = 1;
  n->len = offset;
  n->value = NULL;
  // the parent node is now non terminal and non sorted
  n->flags &= ~(TM_NODE_TERMINAL | TM_NODE_DELETED);

  n = realloc(n, __trieMapNode_Sizeof(n->numChildren, n->len));
  __trieMapNode_children(n)[0] = newChild;

  return n;
}

/* If a node has a single child after delete, we can merged them. This deletes
 * the node and returns a newly allocated node */
TrieMapNode *__trieMapNode_MergeWithSingleChild(TrieMapNode *n) {

  if (__trieMapNode_isTerminal(n) || n->numChildren != 1) {
    return n;
  }
  TrieMapNode *ch = *__trieMapNode_children(n);

  // Copy the current node's data and children to a new child node
  unsigned char nstr[n->len + ch->len + 1];
  memcpy(nstr, n->str, sizeof(unsigned char) * n->len);
  memcpy(&nstr[n->len], ch->str, sizeof(unsigned char) * ch->len);
  TrieMapNode *merged = __newTrieMapNode(nstr, 0, n->len + ch->len, ch->numChildren, ch->value,
                                         __trieMapNode_isTerminal(ch));

  merged->numChildren = ch->numChildren;
  merged->flags = ch->flags;
  TrieMapNode **children = __trieMapNode_children(ch);
  TrieMapNode **newChildren = __trieMapNode_children(merged);
  memcpy(newChildren, children, sizeof(TrieMapNode *) * merged->numChildren);

  free(n);
  free(ch);

  return merged;
}

void TrieMapNode_Print(TrieMapNode *n, int idx, int depth, void (*printval)(void *)) {
  for (int i = 0; i < depth; i++) {
    printf("  ");
  }
  printf("%d) Value :", idx);
  printval(n->value);
  printf("\n");
  for (int i = 0; i < n->numChildren; i++) {
    TrieMapNode_Print(__trieMapNode_children(n)[i], i, depth + 1, printval);
  }
}

int TrieMapNode_Add(TrieMapNode **np, unsigned char *str, tm_len_t len, void *value,
                    TrieMapReplaceFunc cb) {
  if (len == 0) {
    return 0;
  }

  TrieMapNode *n = *np;

  int offset = 0;
  for (; offset < len && offset < n->len; offset++) {
    if (str[offset] != n->str[offset]) {
      break;
    }
  }
  // we broke off before the end of the string
  if (offset < n->len) {
    // split the node and create 2 child nodes:
    // 1. a child representing the new string from the diverted offset onwards
    // 2. a child representing the old node's suffix from the diverted offset
    // and the old children
    n = __trieMapNode_Split(n, offset);

    // the new string matches the split node exactly!
    // we simply turn the split node, which is now non terminal, into a terminal
    // node
    if (offset == len) {
      n->value = value;
      n->flags |= TM_NODE_TERMINAL;
    } else {
      // we add a child
      n = __trieMapNode_AddChild(n, str, offset, len, value);
    }
    *np = n;
    return 1;
  }

  // we're inserting in an existing node - just replace the value
  if (offset == len) {
    int term = __trieMapNode_isTerminal(n);
    int deleted = __trieMapNode_isDeleted(n);

    if (cb) {
      n->value = cb(n->value, value);
    } else {
      n->value = value;
    }

    // set the node as terminal
    n->flags |= TM_NODE_TERMINAL;
    // if it was deleted, make sure it's not now
    n->flags &= ~TM_NODE_DELETED;
    *np = n;
    // if the node existed - we return 0, otherwise return 1 as it's a new node
    return (term && !deleted) ? 0 : 1;
  }

  // proceed to the next child or add a new child for the current unsigned char
  for (tm_len_t i = 0; i < n->numChildren; i++) {
    TrieMapNode *child = __trieMapNode_children(n)[i];

    if (str[offset] == child->str[0]) {
      int rc = TrieMapNode_Add(&child, str + offset, len - offset, value, cb);
      __trieMapNode_children(n)[i] = child;
      return rc;
    }
  }

  *np = __trieMapNode_AddChild(n, str, offset, len, value);
  return 1;
}

void *TrieMapNode_Find(TrieMapNode *n, unsigned char *str, tm_len_t len) {
  tm_len_t offset = 0;
  while (n && offset < len) {
    // printf("n %.*s offset %d, len %d\n", n->len, n->str, offset,
    // len);
    tm_len_t localOffset = 0;
    for (; offset < len && localOffset < n->len; offset++, localOffset++) {
      if (str[offset] != n->str[localOffset]) {
        break;
      }
    }

    if (offset == len) {
      // we're at the end of both strings!
      if (localOffset == n->len) return __trieMapNode_isDeleted(n) ? NULL : n->value;

    } else if (localOffset == n->len) {
      // we've reached the end of the node's string but not the search string
      // let's find a child to continue to
      tm_len_t i = 0;
      TrieMapNode *nextChild = NULL;
      for (; i < n->numChildren; i++) {
        TrieMapNode *child = __trieMapNode_children(n)[i];

        if (str[offset] == child->str[0]) {
          nextChild = child;
          break;
        }
      }

      // we couldn't find a matching child
      n = nextChild;

    } else {
      return 0;
    }
  }

  return 0;
}

/* Optimize the node and its children:
*   1. If a child should be deleted - delete it and reduce the child count
*   2. If a child has a single child - merge them
*/
void __trieMapNode_optimizeChildren(TrieMapNode *n, void (*freeCB)(void *)) {

  int i = 0;
  TrieMapNode **nodes = __trieMapNode_children(n);
  // free deleted terminal nodes
  while (i < n->numChildren) {

    // if this is a deleted node with no children - remove it
    if (nodes[i]->numChildren == 0 && __trieMapNode_isDeleted(nodes[i])) {
      TrieMapNode_Free(nodes[i], freeCB);

      nodes[i] = NULL;
      // just "fill" the hole with the next node up
      while (i < n->numChildren - 1) {
        nodes[i] = nodes[i + 1];
        i++;
      }
      // reduce child count
      n->numChildren--;
    } else {

      // this node is ok!
      // if needed - merge this node with it its single child
      if (nodes[i] && nodes[i]->numChildren == 1) {
        nodes[i] = __trieMapNode_MergeWithSingleChild(nodes[i]);
      }
    }
    i++;
  }
}

int TrieMapNode_Delete(TrieMapNode *n, unsigned char *str, tm_len_t len, void (*freeCB)(void *)) {
  tm_len_t offset = 0;
  static TrieMapNode *stack[TM_MAX_STRING_LEN];
  int stackPos = 0;
  int rc = 0;
  while (n && offset < len) {
    stack[stackPos++] = n;
    tm_len_t localOffset = 0;
    for (; offset < len && localOffset < n->len; offset++, localOffset++) {
      if (str[offset] != n->str[localOffset]) {
        break;
      }
    }

    if (offset == len) {
      // we're at the end of both strings!
      // this means we've found what we're looking for
      if (localOffset == n->len) {
        if (!(n->flags & TM_NODE_DELETED)) {

          n->flags |= TM_NODE_DELETED;
          n->flags &= ~TM_NODE_TERMINAL;
          n->value = NULL;
          rc = 1;
        }
        goto end;
      }

    } else if (localOffset == n->len) {
      // we've reached the end of the node's string but not the search string
      // let's find a child to continue to
      tm_len_t i = 0;
      TrieMapNode *nextChild = NULL;
      for (; i < n->numChildren; i++) {
        TrieMapNode *child = __trieMapNode_children(n)[i];

        if (str[offset] == child->str[0]) {
          nextChild = child;
          break;
        }
      }

      // we couldn't find a matching child
      n = nextChild;

    } else {
      goto end;
    }
  }

end:

  while (stackPos--) {
    __trieMapNode_optimizeChildren(stack[stackPos], freeCB);
  }
  return rc;
}

size_t TrieMapNode_MemUsage(TrieMapNode *n) {

  size_t ret = __trieMapNode_Sizeof(n->numChildren, n->len);
  for (tm_len_t i = 0; i < n->numChildren; i++) {
    TrieMapNode *child = __trieMapNode_children(n)[i];
    ret += TrieMapNode_MemUsage(child);
  }
  return ret;
}

void TrieMapNode_Free(TrieMapNode *n, void (*freeCB)(void *)) {
  for (tm_len_t i = 0; i < n->numChildren; i++) {
    TrieMapNode *child = __trieMapNode_children(n)[i];
    TrieMapNode_Free(child, freeCB);
  }
  if (n->value) {
    if (freeCB) {
      freeCB(n->value);
    } else {
      free(n->value);
    }
  }

  free(n);
}
