#include <stddef.h>
typedef unsigned long size_t_;
struct fwd;                       /* never defined here */
struct node { int v; struct node *next; struct fwd *f; };
struct bits { unsigned a:3; unsigned b:5; int c:9; char d; unsigned long long e:40; };
struct __attribute__((packed)) pk { char c; int i; short s; };
struct anon { union { int x; float y; }; struct { char p; char q; } s; };
struct flex { int n; int data[]; };
struct big { char buf[70000]; };
enum color { RED = -1, GREEN = 5, BLUE = 0x7fffffff };
enum __attribute__((packed)) small { S1, S2 };
typedef struct node node_t;
typedef int (*cb_t)(int, char *, ...);
typedef void (*vcb_t)(void);
union u { int i; char c[4]; };
int gi; static int si; const volatile int cvi = 3; int arr2d[3][4]; char *restrict rp;
_Bool gb; signed char sc; unsigned char uc; char ch; long double ld; double _Complex dc; float fl;
node_t *head; cb_t cb; vcb_t vcb; union u gu; struct anon ga; struct flex gf; struct big gbig; enum color gc; enum small gs; struct pk gpk; struct bits gbits;
void *vp; const char *ccp; int (*fnp)(void); int *const cpi = &gi;
int f1(int a, char *b, ...) { return a + (int)b[0] + si; }
static int f2(struct node *n, unsigned long long x) { return n->v + (int)x; }
void f3(void) { }
int f4() { return f2(head, 1); }   /* K&R, unprototyped */
int f5(int a, ...); int f5(int a, ...) { return a; }
static inline int inl(int z) { return z * 2; }
int f6(int q) { return inl(q) + f1(1, "x") + f4() + f5(1); }
