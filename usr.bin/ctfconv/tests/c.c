/* local scopes, weak alias, abs symbols, nested/anon types, unsigned enum, 64-bit enum, zero-size array, typedef chains */
int gl(int n) { static int counter; struct local { int a; } l = { n }; typedef struct local local_t; local_t *p = &l; counter += p->a; return counter; }
int strong(void) { return 1; }
int weakalias(void) __attribute__((weak, alias("strong")));
enum uns { U1 = 0x80000000u };
enum big64 { B1 = 0x100000000ll };
struct zsa { int n; int z[0]; };
typedef int myint; typedef myint myint2; typedef myint2 *pmyint2;
pmyint2 pm; struct zsa gz; enum uns gu; enum big64 gbg;
union { int a; char b; } anonglobal;
struct outer { struct inner { int x; } in; enum { EA, EB } e; };
struct outer go;
extern int external_decl;              /* declaration only, no definition */
int use_ext(void) { return external_decl; }
